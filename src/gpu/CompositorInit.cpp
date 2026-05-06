/*
 * CompositorInit.cpp -- init, shutdown, create* methods.
 *
 * Split from Compositor.cpp for maintainability.
 */

#include <volk.h>
#include "Compositor.h"

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vk_mem_alloc.h>

#include <spdlog/spdlog.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <cmath>


namespace fs = std::filesystem;

namespace rt {

// ── Constructor / Destructor ────────────────────────────────────────────────

Compositor::Compositor() = default;

Compositor::~Compositor()
{
    shutdown();
}

// ── init ────────────────────────────────────────────────────────────────────

bool Compositor::init(Device& device,
                      Allocator& allocator,
                      CommandPool& cmdPool,
                      VkQueue computeQueue,
                      const CompositorConfig& config)
{
    if (m_initialized)
    {
        spdlog::warn("Compositor already initialized");
        return true;
    }

    m_device    = &device;
    m_allocator = &allocator;
    m_cmdPool   = &cmdPool;
    m_queue     = computeQueue;
    m_config    = config;

    // Initialize pipeline manager
    if (!m_pipelineManager.create(device))
    {
        spdlog::error("Compositor: Failed to create pipeline manager");
        return false;
    }

    // Get timestamp period for GPU timing
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(device.physicalDevice(), &props);
    m_timestampPeriod = props.limits.timestampPeriod;

    // Create resources
    if (!createOutputTexture())      { shutdown(); return false; }
    if (!createDescriptorResources()){ shutdown(); return false; }
    if (!createComputePipeline())    { shutdown(); return false; }
    if (!createTimestampQueries())   { shutdown(); return false; }

    // Create SSBO for layer parameters (host-visible for easy updates)
    if (!m_layerParamsBuffer.create(m_allocator->handle(),
                                    sizeof(LayerParamsGPU),
                                    BufferUsage::Uniform))
    {
        spdlog::error("Compositor: Failed to create layer params SSBO");
        shutdown();
        return false;
    }

    // Create 1x1 transparent placeholder texture for unused layer slots
    {
        uint32_t transparent = 0x00000000;
        TextureConfig placeholderConfig;
        placeholderConfig.width  = 1;
        placeholderConfig.height = 1;
        placeholderConfig.format = VK_FORMAT_R8G8B8A8_UNORM;
        placeholderConfig.usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (!m_placeholderTexture.createFromData(
                m_allocator->handle(), device.handle(),
                placeholderConfig,
                &transparent, 4,
                cmdPool, computeQueue))
        {
            spdlog::error("Compositor: Failed to create placeholder texture");
            shutdown();
            return false;
        }
    }

    // Initialize descriptor set with placeholder textures
    m_layersDirty = true;
    clearLayers();
    updateSSBO();
    updateDescriptorSet();

    m_initialized = true;
    spdlog::info("Compositor initialized ({}x{}, max {} layers)",
                 config.outputWidth, config.outputHeight, kMaxCompositorLayers);
    return true;
}

// ── shutdown ────────────────────────────────────────────────────────────────

void Compositor::shutdown()
{
    if (!m_device) return;

    VkDevice dev = m_device->handle();

    // Wait for GPU idle before destroying
    if (dev != VK_NULL_HANDLE)
        vkDeviceWaitIdle(dev);

    // Destroy timestamp query pool
    if (m_queryPool != VK_NULL_HANDLE)
    {
        vkDestroyQueryPool(dev, m_queryPool, nullptr);
        m_queryPool = VK_NULL_HANDLE;
    }

    // Destroy descriptor resources
    if (m_descriptorPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(dev, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
        m_descriptorSet  = VK_NULL_HANDLE;
    }

    if (m_descriptorSetLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(dev, m_descriptorSetLayout, nullptr);
        m_descriptorSetLayout = VK_NULL_HANDLE;
    }

    // Destroy pipeline
    m_pipelineManager.destroy();
    m_pipeline       = VK_NULL_HANDLE;
    m_pipelineLayout = VK_NULL_HANDLE;

    // Destroy buffers and textures
    m_readbackStaging.destroy();
    m_layerParamsBuffer.destroy();
    m_placeholderTexture.destroy();
    m_outputTexture.destroy();

    m_layers.clear();
    m_layerCount  = 0;
    m_initialized = false;
    m_device      = nullptr;
    m_allocator   = nullptr;
    m_cmdPool     = nullptr;
    m_queue       = VK_NULL_HANDLE;
}

// ── createOutputTexture ─────────────────────────────────────────────────────

bool Compositor::createOutputTexture()
{
    TextureConfig cfg;
    cfg.width  = m_config.outputWidth;
    cfg.height = m_config.outputHeight;
    cfg.format = m_config.outputFormat;
    // Need STORAGE for compute write, SAMPLED for reading, TRANSFER for readback
    cfg.usage  = VK_IMAGE_USAGE_STORAGE_BIT
               | VK_IMAGE_USAGE_SAMPLED_BIT
               | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
               | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    // Enable concurrent sharing between compute and graphics queue families
    // so the output can be sampled without ownership transfers.
    const auto& families = m_device->queueFamilies();
    uint32_t computeFamily  = families.compute.value_or(families.graphics.value());
    uint32_t graphicsFamily = families.graphics.value();
    if (computeFamily != graphicsFamily) {
        cfg.concurrentQueueFamilies = {computeFamily, graphicsFamily};
    }

    if (!m_outputTexture.create(m_allocator->handle(), m_device->handle(), cfg))
    {
        spdlog::error("Compositor: Failed to create output texture");
        return false;
    }

    // Transition to GENERAL for compute shader writes
    VkCommandBuffer cmd = m_cmdPool->beginSingleTime();
    m_outputTexture.transitionLayout(cmd, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    m_cmdPool->endSingleTime(cmd, m_queue);

    return true;
}

// ── createComputePipeline ───────────────────────────────────────────────────

bool Compositor::createComputePipeline()
{
    // Find shader
    fs::path shaderPaths[] = {
        fs::path(__FILE__).parent_path().parent_path().parent_path() / "build" / "shaders" / "composite.comp.spv",
        fs::current_path() / "shaders" / "composite.comp.spv",
        fs::current_path().parent_path() / "shaders" / "composite.comp.spv",
        fs::current_path().parent_path() / "build" / "shaders" / "composite.comp.spv",
    };

    fs::path shaderPath;
    for (auto& p : shaderPaths)
    {
        if (fs::exists(p)) { shaderPath = p; break; }
    }

    if (shaderPath.empty())
    {
        spdlog::error("Compositor: composite.comp.spv not found");
        return false;
    }

    VkShaderModule compShader = m_pipelineManager.loadShader(shaderPath);
    if (compShader == VK_NULL_HANDLE)
    {
        spdlog::error("Compositor: Failed to load composite.comp.spv");
        return false;
    }

    // Create pipeline layout with push constants + descriptor set layout
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset     = 0;
    pushRange.size       = sizeof(CompositePushConstants);

    m_pipelineLayout = m_pipelineManager.createLayout({pushRange}, {m_descriptorSetLayout});
    if (m_pipelineLayout == VK_NULL_HANDLE)
    {
        spdlog::error("Compositor: Failed to create pipeline layout");
        return false;
    }

    ComputePipelineConfig pipeConfig;
    pipeConfig.compShader = compShader;
    pipeConfig.layout     = m_pipelineLayout;

    m_pipeline = m_pipelineManager.createComputePipeline(pipeConfig);
    if (m_pipeline == VK_NULL_HANDLE)
    {
        spdlog::error("Compositor: Failed to create compute pipeline");
        return false;
    }

    spdlog::debug("Compositor: Compute pipeline created successfully");
    return true;
}

// ── createDescriptorResources ───────────────────────────────────────────────

bool Compositor::createDescriptorResources()
{
    VkDevice dev = m_device->handle();

    // ── Descriptor set layout ───────────────────────────────────────────
    // binding 0: storage image (output)
    // binding 1: combined image sampler array[32] (layer textures)
    // binding 2: storage buffer (layer params SSBO)
    // binding 3: combined image sampler array[32] (mask textures)

    VkDescriptorSetLayoutBinding bindings[4] = {};

    // Binding 0: output storage image
    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    // Binding 1: layer textures (array of 32 samplers)
    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = kMaxCompositorLayers;
    bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    // Binding 2: layer params SSBO
    bindings[2].binding         = 2;
    bindings[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    // Binding 3: mask textures (array of 32 samplers)
    bindings[3].binding         = 3;
    bindings[3].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[3].descriptorCount = kMaxCompositorLayers;
    bindings[3].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 4;
    layoutInfo.pBindings    = bindings;

    if (vkCreateDescriptorSetLayout(dev, &layoutInfo, nullptr, &m_descriptorSetLayout) != VK_SUCCESS)
    {
        spdlog::error("Compositor: Failed to create descriptor set layout");
        return false;
    }

    // ── Descriptor pool ─────────────────────────────────────────────────
    VkDescriptorPoolSize poolSizes[4] = {};
    poolSizes[0].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[0].descriptorCount = 1;
    poolSizes[1].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = kMaxCompositorLayers;
    poolSizes[2].type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[2].descriptorCount = 1;
    poolSizes[3].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[3].descriptorCount = kMaxCompositorLayers;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets       = 1;
    poolInfo.poolSizeCount = 4;
    poolInfo.pPoolSizes    = poolSizes;

    if (vkCreateDescriptorPool(dev, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS)
    {
        spdlog::error("Compositor: Failed to create descriptor pool");
        return false;
    }

    // ── Allocate descriptor set ─────────────────────────────────────────
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &m_descriptorSetLayout;

    if (vkAllocateDescriptorSets(dev, &allocInfo, &m_descriptorSet) != VK_SUCCESS)
    {
        spdlog::error("Compositor: Failed to allocate descriptor set");
        return false;
    }

    return true;
}

// ── createTimestampQueries ──────────────────────────────────────────────────

bool Compositor::createTimestampQueries()
{
    VkQueryPoolCreateInfo queryInfo{};
    queryInfo.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    queryInfo.queryType  = VK_QUERY_TYPE_TIMESTAMP;
    queryInfo.queryCount = 2;  // begin + end

    if (vkCreateQueryPool(m_device->handle(), &queryInfo, nullptr, &m_queryPool) != VK_SUCCESS)
    {
        spdlog::warn("Compositor: Failed to create timestamp query pool (timing disabled)");
        m_queryPool = VK_NULL_HANDLE;
        // Non-fatal — timing is optional
    }

    return true;
}


} // namespace rt
