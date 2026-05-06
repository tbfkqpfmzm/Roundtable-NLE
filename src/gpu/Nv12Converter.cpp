/*
 * Nv12Converter.cpp â€” GPU compute-shader NV12 â†’ BGRA color conversion.
 *
 * Eliminates CPU-side sws_scale for NV12 frames decoded by NVDEC.
 * Uses BT.709 color matrix via a Vulkan compute shader.
 */

#include <volk.h>
#include "Nv12Converter.h"

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vk_mem_alloc.h>

#include <spdlog/spdlog.h>
#include <cstring>
#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;

namespace rt {

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Helper: locate SPIR-V shader file
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•


Nv12Converter::Nv12Converter() = default;

Nv12Converter::~Nv12Converter()
{
    shutdown();
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Lifecycle
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

bool Nv12Converter::init(Device& device, Allocator& allocator,
                          CommandPool& cmdPool, VkQueue computeQueue,
                          const Nv12ConverterConfig& config)
{
    m_device    = &device;
    m_allocator = &allocator;
    m_cmdPool   = &cmdPool;
    m_queue     = computeQueue;
    m_config    = config;

    if (!createTextures()) {
        spdlog::error("Nv12Converter: failed to create textures");
        return false;
    }
    if (!createDescriptorResources()) {
        spdlog::error("Nv12Converter: failed to create descriptor resources");
        return false;
    }
    if (!createPipeline()) {
        spdlog::error("Nv12Converter: failed to create pipeline");
        return false;
    }

    m_initialized = true;
    spdlog::info("Nv12Converter initialized ({}x{})", m_config.width, m_config.height);
    return true;
}

void Nv12Converter::shutdown()
{
    if (!m_device) return;
    VkDevice dev = m_device->handle();
    if (dev) vkDeviceWaitIdle(dev);

    if (m_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
    }
    if (m_pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(dev, m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }
    if (m_shaderModule != VK_NULL_HANDLE) {
        vkDestroyShaderModule(dev, m_shaderModule, nullptr);
        m_shaderModule = VK_NULL_HANDLE;
    }
    if (m_descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(dev, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
        m_descriptorSet  = VK_NULL_HANDLE;
    }
    if (m_descriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, m_descriptorSetLayout, nullptr);
        m_descriptorSetLayout = VK_NULL_HANDLE;
    }

    m_yTexture.destroy();
    m_uvTexture.destroy();
    m_uTexture.destroy();
    m_vTexture.destroy();
    m_outputTexture.destroy();

    // YUV420P pipeline cleanup
    if (m_yuv420pPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, m_yuv420pPipeline, nullptr);
        m_yuv420pPipeline = VK_NULL_HANDLE;
    }
    if (m_yuv420pPipeLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(dev, m_yuv420pPipeLayout, nullptr);
        m_yuv420pPipeLayout = VK_NULL_HANDLE;
    }
    if (m_yuv420pShaderModule != VK_NULL_HANDLE) {
        vkDestroyShaderModule(dev, m_yuv420pShaderModule, nullptr);
        m_yuv420pShaderModule = VK_NULL_HANDLE;
    }
    if (m_yuv420pDescPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(dev, m_yuv420pDescPool, nullptr);
        m_yuv420pDescPool = VK_NULL_HANDLE;
        m_yuv420pDescSet  = VK_NULL_HANDLE;
    }
    if (m_yuv420pDescSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, m_yuv420pDescSetLayout, nullptr);
        m_yuv420pDescSetLayout = VK_NULL_HANDLE;
    }

    m_initialized = false;
    m_device      = nullptr;
    m_allocator   = nullptr;
    m_cmdPool     = nullptr;
    m_queue       = VK_NULL_HANDLE;
}



// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Helper: locate SPIR-V shader file
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

static fs::path findShader(const char* name)
{
    fs::path candidates[] = {
        fs::path(__FILE__).parent_path().parent_path().parent_path() / "build" / "shaders" / name,
        fs::current_path() / "shaders" / name,
        fs::current_path().parent_path() / "shaders" / name,
        fs::current_path().parent_path() / "build" / "shaders" / name,
    };
    for (auto& p : candidates)
        if (fs::exists(p)) return p;
    return {};
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•


// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Internal â€” create textures
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

bool Nv12Converter::createTextures()
{
    // Output BGRA storage image
    TextureConfig outCfg;
    outCfg.width  = m_config.width;
    outCfg.height = m_config.height;
    outCfg.format = VK_FORMAT_R8G8B8A8_UNORM;
    outCfg.usage  = VK_IMAGE_USAGE_STORAGE_BIT
                  | VK_IMAGE_USAGE_SAMPLED_BIT
                  | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    if (!m_outputTexture.create(m_allocator->handle(), m_device->handle(), outCfg)) {
        spdlog::error("Nv12Converter: failed to create output texture");
        return false;
    }

    // Transition to GENERAL for compute shader write
    VkCommandBuffer cmd = m_cmdPool->beginSingleTime();
    m_outputTexture.transitionLayout(
        cmd, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    m_cmdPool->endSingleTime(cmd, m_queue);

    // Y and UV textures are created on demand in convert() when we know the
    // actual frame dimensions (they may differ from config if resize hasn't
    // been called yet).

    return true;
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Internal â€” create descriptor resources
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

bool Nv12Converter::createDescriptorResources()
{
    VkDevice dev = m_device->handle();

    // Layout: binding 0 = Y sampler, binding 1 = UV sampler, binding 2 = output storage
    VkDescriptorSetLayoutBinding bindings[3]{};

    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[2].binding         = 2;
    bindings[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutCI{};
    layoutCI.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutCI.bindingCount = 3;
    layoutCI.pBindings    = bindings;

    if (vkCreateDescriptorSetLayout(dev, &layoutCI, nullptr,
                                     &m_descriptorSetLayout) != VK_SUCCESS)
    {
        spdlog::error("Nv12Converter: failed to create descriptor set layout");
        return false;
    }

    // Pool
    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = 2;
    poolSizes[1].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[1].descriptorCount = 1;

    VkDescriptorPoolCreateInfo poolCI{};
    poolCI.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolCI.maxSets       = 1;
    poolCI.poolSizeCount = 2;
    poolCI.pPoolSizes    = poolSizes;

    if (vkCreateDescriptorPool(dev, &poolCI, nullptr,
                                &m_descriptorPool) != VK_SUCCESS)
    {
        spdlog::error("Nv12Converter: failed to create descriptor pool");
        return false;
    }

    // Allocate set
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &m_descriptorSetLayout;

    if (vkAllocateDescriptorSets(dev, &allocInfo, &m_descriptorSet) != VK_SUCCESS) {
        spdlog::error("Nv12Converter: failed to allocate descriptor set");
        return false;
    }

    return true;
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Internal â€” create compute pipeline
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

bool Nv12Converter::createPipeline()
{
    VkDevice dev = m_device->handle();

    // Pipeline layout
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset     = 0;
    pushRange.size       = sizeof(int32_t) * 2; // width, height

    VkPipelineLayoutCreateInfo layoutCI{};
    layoutCI.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutCI.setLayoutCount         = 1;
    layoutCI.pSetLayouts            = &m_descriptorSetLayout;
    layoutCI.pushConstantRangeCount = 1;
    layoutCI.pPushConstantRanges    = &pushRange;

    if (vkCreatePipelineLayout(dev, &layoutCI, nullptr,
                                &m_pipelineLayout) != VK_SUCCESS)
    {
        spdlog::error("Nv12Converter: failed to create pipeline layout");
        return false;
    }

    // Load shader
    fs::path spvPath = findShader("nv12_to_bgra.comp.spv");
    if (spvPath.empty()) {
        spdlog::warn("Nv12Converter: nv12_to_bgra.comp.spv not found â€” "
                     "shader not compiled yet, conversion will be unavailable");
        return false;
    }

    // Read SPIR-V binary
    std::ifstream file(spvPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        spdlog::error("Nv12Converter: failed to open {}", spvPath.string());
        return false;
    }
    size_t fileSize = static_cast<size_t>(file.tellg());
    std::vector<char> spirv(fileSize);
    file.seekg(0);
    file.read(spirv.data(), static_cast<std::streamsize>(fileSize));

    VkShaderModuleCreateInfo smCI{};
    smCI.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smCI.codeSize = spirv.size();
    smCI.pCode    = reinterpret_cast<const uint32_t*>(spirv.data());

    if (vkCreateShaderModule(dev, &smCI, nullptr, &m_shaderModule) != VK_SUCCESS) {
        spdlog::error("Nv12Converter: failed to create shader module");
        return false;
    }

    // Compute pipeline
    VkComputePipelineCreateInfo pci{};
    pci.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pci.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pci.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    pci.stage.module = m_shaderModule;
    pci.stage.pName  = "main";
    pci.layout = m_pipelineLayout;

    if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pci, nullptr,
                                  &m_pipeline) != VK_SUCCESS)
    {
        spdlog::error("Nv12Converter: failed to create compute pipeline");
        return false;
    }

    spdlog::info("Nv12Converter: compute pipeline created");
    return true;
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Internal â€” create YUV420P pipeline (lazy, called on first use)
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

bool Nv12Converter::createYuv420pPipeline()
{
    VkDevice dev = m_device->handle();

    // Descriptor set layout: 3 samplers (Y, U, V) + 1 storage image (output)
    VkDescriptorSetLayoutBinding bindings[4]{};
    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[2].binding         = 2;
    bindings[2].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[3].binding         = 3;
    bindings[3].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutCI{};
    layoutCI.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutCI.bindingCount = 4;
    layoutCI.pBindings    = bindings;

    if (vkCreateDescriptorSetLayout(dev, &layoutCI, nullptr,
                                     &m_yuv420pDescSetLayout) != VK_SUCCESS)
    {
        spdlog::error("Nv12Converter: failed to create YUV420P descriptor set layout");
        return false;
    }

    // Descriptor pool
    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = 3;
    poolSizes[1].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[1].descriptorCount = 1;

    VkDescriptorPoolCreateInfo poolCI{};
    poolCI.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolCI.maxSets       = 1;
    poolCI.poolSizeCount = 2;
    poolCI.pPoolSizes    = poolSizes;

    if (vkCreateDescriptorPool(dev, &poolCI, nullptr,
                                &m_yuv420pDescPool) != VK_SUCCESS)
    {
        spdlog::error("Nv12Converter: failed to create YUV420P descriptor pool");
        return false;
    }

    // Allocate descriptor set
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_yuv420pDescPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &m_yuv420pDescSetLayout;

    if (vkAllocateDescriptorSets(dev, &allocInfo, &m_yuv420pDescSet) != VK_SUCCESS) {
        spdlog::error("Nv12Converter: failed to allocate YUV420P descriptor set");
        return false;
    }

    // Pipeline layout
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset     = 0;
    pushRange.size       = sizeof(int32_t) * 2; // width, height

    VkPipelineLayoutCreateInfo pipeLayoutCI{};
    pipeLayoutCI.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeLayoutCI.setLayoutCount         = 1;
    pipeLayoutCI.pSetLayouts            = &m_yuv420pDescSetLayout;
    pipeLayoutCI.pushConstantRangeCount = 1;
    pipeLayoutCI.pPushConstantRanges    = &pushRange;

    if (vkCreatePipelineLayout(dev, &pipeLayoutCI, nullptr,
                                &m_yuv420pPipeLayout) != VK_SUCCESS)
    {
        spdlog::error("Nv12Converter: failed to create YUV420P pipeline layout");
        return false;
    }

    // Load shader
    fs::path spvPath = findShader("yuv420p_to_bgra.comp.spv");
    if (spvPath.empty()) {
        spdlog::warn("Nv12Converter: yuv420p_to_bgra.comp.spv not found â€” "
                     "YUV420P GPU conversion unavailable");
        return false;
    }

    std::ifstream file(spvPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        spdlog::error("Nv12Converter: failed to open {}", spvPath.string());
        return false;
    }
    size_t fileSize = static_cast<size_t>(file.tellg());
    std::vector<char> spirv(fileSize);
    file.seekg(0);
    file.read(spirv.data(), static_cast<std::streamsize>(fileSize));

    VkShaderModuleCreateInfo smCI{};
    smCI.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smCI.codeSize = spirv.size();
    smCI.pCode    = reinterpret_cast<const uint32_t*>(spirv.data());

    if (vkCreateShaderModule(dev, &smCI, nullptr, &m_yuv420pShaderModule) != VK_SUCCESS) {
        spdlog::error("Nv12Converter: failed to create YUV420P shader module");
        return false;
    }

    // Compute pipeline
    VkComputePipelineCreateInfo pci{};
    pci.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pci.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pci.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    pci.stage.module = m_yuv420pShaderModule;
    pci.stage.pName  = "main";
    pci.layout = m_yuv420pPipeLayout;

    if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pci, nullptr,
                                  &m_yuv420pPipeline) != VK_SUCCESS)
    {
        spdlog::error("Nv12Converter: failed to create YUV420P compute pipeline");
        return false;
    }

    spdlog::info("Nv12Converter: YUV420P compute pipeline created");
    return true;
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  YUV420P â†’ BGRA synchronous conversion (CPU upload â†’ GPU compute)
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

bool Nv12Converter::convertYuv420pSync(const uint8_t* yData, int yLinesize,
                                        const uint8_t* uData, int uLinesize,
                                        const uint8_t* vData, int vLinesize,
                                        uint32_t width, uint32_t height)
{
    if (!m_initialized || !m_cmdPool) return false;

    // Lazy pipeline creation
    if (m_yuv420pPipeline == VK_NULL_HANDLE) {
        if (!createYuv420pPipeline()) {
            spdlog::warn("Nv12Converter: YUV420P pipeline not available");
            return false;
        }
    }

    // Auto-resize output if dimensions changed
    if (width != m_config.width || height != m_config.height) {
        if (!resize(width, height)) return false;
    }

    const uint32_t w  = m_config.width;
    const uint32_t h  = m_config.height;
    const uint32_t cw = w / 2; // chroma width
    const uint32_t ch = h / 2; // chroma height

    VkCommandBuffer cmd = m_cmdPool->beginSingleTime();
    std::vector<Texture::StagingCleanup> staging;

    // â”€â”€ Upload Y plane (R8, W Ã— H) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    {
        const VkDeviceSize ySize = static_cast<VkDeviceSize>(w) * h;
        std::vector<uint8_t> yPacked(static_cast<size_t>(ySize));
        for (uint32_t row = 0; row < h; ++row)
            std::memcpy(yPacked.data() + row * w, yData + row * yLinesize, w);

        Texture::StagingCleanup stg{};
        if (m_yTexture.image() == VK_NULL_HANDLE ||
            m_yTexture.width() != w || m_yTexture.height() != h)
        {
            m_yTexture.destroy();
            TextureConfig cfg;
            cfg.width  = w;
            cfg.height = h;
            cfg.format = VK_FORMAT_R8_UNORM;
            cfg.usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            if (!m_yTexture.createFromDataBatched(
                    m_allocator->handle(), m_device->handle(), cfg,
                    yPacked.data(), ySize, cmd, stg))
                return false;
        } else {
            if (!m_yTexture.updateDataBatched(yPacked.data(), ySize, cmd, stg))
                return false;
        }
        if (stg.buffer != VK_NULL_HANDLE) staging.push_back(stg);
    }

    // â”€â”€ Upload U plane (R8, W/2 Ã— H/2) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    {
        const VkDeviceSize uSize = static_cast<VkDeviceSize>(cw) * ch;
        std::vector<uint8_t> uPacked(static_cast<size_t>(uSize));
        for (uint32_t row = 0; row < ch; ++row)
            std::memcpy(uPacked.data() + row * cw, uData + row * uLinesize, cw);

        Texture::StagingCleanup stg{};
        if (m_uTexture.image() == VK_NULL_HANDLE ||
            m_uTexture.width() != cw || m_uTexture.height() != ch)
        {
            m_uTexture.destroy();
            TextureConfig cfg;
            cfg.width  = cw;
            cfg.height = ch;
            cfg.format = VK_FORMAT_R8_UNORM;
            cfg.usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            if (!m_uTexture.createFromDataBatched(
                    m_allocator->handle(), m_device->handle(), cfg,
                    uPacked.data(), uSize, cmd, stg))
                return false;
        } else {
            if (!m_uTexture.updateDataBatched(uPacked.data(), uSize, cmd, stg))
                return false;
        }
        if (stg.buffer != VK_NULL_HANDLE) staging.push_back(stg);
    }

    // â”€â”€ Upload V plane (R8, W/2 Ã— H/2) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    {
        const VkDeviceSize vSize = static_cast<VkDeviceSize>(cw) * ch;
        std::vector<uint8_t> vPacked(static_cast<size_t>(vSize));
        for (uint32_t row = 0; row < ch; ++row)
            std::memcpy(vPacked.data() + row * cw, vData + row * vLinesize, cw);

        Texture::StagingCleanup stg{};
        if (m_vTexture.image() == VK_NULL_HANDLE ||
            m_vTexture.width() != cw || m_vTexture.height() != ch)
        {
            m_vTexture.destroy();
            TextureConfig cfg;
            cfg.width  = cw;
            cfg.height = ch;
            cfg.format = VK_FORMAT_R8_UNORM;
            cfg.usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            if (!m_vTexture.createFromDataBatched(
                    m_allocator->handle(), m_device->handle(), cfg,
                    vPacked.data(), vSize, cmd, stg))
                return false;
        } else {
            if (!m_vTexture.updateDataBatched(vPacked.data(), vSize, cmd, stg))
                return false;
        }
        if (stg.buffer != VK_NULL_HANDLE) staging.push_back(stg);
    }

    // â”€â”€ Barrier: transfer writes â†’ compute reads â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    {
        VkMemoryBarrier barrier{};
        barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 1, &barrier, 0, nullptr, 0, nullptr);
    }

    // â”€â”€ Update YUV420P descriptor set â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    {
        VkDescriptorImageInfo yInfo  = m_yTexture.descriptorInfo();
        VkDescriptorImageInfo uInfo  = m_uTexture.descriptorInfo();
        VkDescriptorImageInfo vInfo  = m_vTexture.descriptorInfo();
        VkDescriptorImageInfo outInfo{};
        outInfo.imageView   = m_outputTexture.imageView();
        outInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet writes[4]{};
        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = m_yuv420pDescSet;
        writes[0].dstBinding      = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo      = &yInfo;

        writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet          = m_yuv420pDescSet;
        writes[1].dstBinding      = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo      = &uInfo;

        writes[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet          = m_yuv420pDescSet;
        writes[2].dstBinding      = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[2].pImageInfo      = &vInfo;

        writes[3].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[3].dstSet          = m_yuv420pDescSet;
        writes[3].dstBinding      = 3;
        writes[3].descriptorCount = 1;
        writes[3].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[3].pImageInfo      = &outInfo;

        vkUpdateDescriptorSets(m_device->handle(), 4, writes, 0, nullptr);
    }

    // â”€â”€ Dispatch compute shader â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_yuv420pPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            m_yuv420pPipeLayout, 0, 1, &m_yuv420pDescSet,
                            0, nullptr);

    struct { int32_t w, h; } pc;
    pc.w = static_cast<int32_t>(w);
    pc.h = static_cast<int32_t>(h);
    vkCmdPushConstants(cmd, m_yuv420pPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(pc), &pc);

    uint32_t gx = (w + 15) / 16;
    uint32_t gy = (h + 15) / 16;
    vkCmdDispatch(cmd, gx, gy, 1);

    // â”€â”€ Barrier: compute writes â†’ shader reads / transfer reads â”€â”€â”€â”€â”€â”€â”€â”€â”€
    {
        VkMemoryBarrier barrier{};
        barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT
                              | VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 1, &barrier, 0, nullptr, 0, nullptr);
    }

    m_cmdPool->endSingleTime(cmd, m_queue);

    // Clean up staging buffers
    for (auto& s : staging)
        s.destroy();

    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Scaled YUV420P→BGRA (upload at srcW×srcH, output at dstW×dstH)
// ═══════════════════════════════════════════════════════════════════════════

bool Nv12Converter::convertYuv420pSyncScaled(
    const uint8_t* yData, int yLinesize,
    const uint8_t* uData, int uLinesize,
    const uint8_t* vData, int vLinesize,
    uint32_t srcW, uint32_t srcH,
    uint32_t dstW, uint32_t dstH)
{
    if (!m_initialized || !m_cmdPool) return false;

    // Same-size: delegate to non-scaled path
    if (srcW == dstW && srcH == dstH)
        return convertYuv420pSync(yData, yLinesize, uData, uLinesize,
                                  vData, vLinesize, srcW, srcH);

    // Lazy pipeline creation
    if (m_yuv420pPipeline == VK_NULL_HANDLE) {
        if (!createYuv420pPipeline()) return false;
    }

    // Ensure output texture is at target (downscaled) size
    if (!ensureOutputSize(dstW, dstH)) return false;

    const uint32_t cw = srcW / 2;
    const uint32_t ch = srcH / 2;

    VkCommandBuffer cmd = m_cmdPool->beginSingleTime();
    std::vector<Texture::StagingCleanup> staging;

    // ── Upload Y plane at source resolution ─────────────────────────────
    {
        const VkDeviceSize ySize = static_cast<VkDeviceSize>(srcW) * srcH;
        std::vector<uint8_t> yPacked(static_cast<size_t>(ySize));
        for (uint32_t row = 0; row < srcH; ++row)
            std::memcpy(yPacked.data() + row * srcW, yData + row * yLinesize, srcW);

        Texture::StagingCleanup stg{};
        if (m_yTexture.image() == VK_NULL_HANDLE ||
            m_yTexture.width() != srcW || m_yTexture.height() != srcH)
        {
            m_yTexture.destroy();
            TextureConfig cfg;
            cfg.width  = srcW;
            cfg.height = srcH;
            cfg.format = VK_FORMAT_R8_UNORM;
            cfg.usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            if (!m_yTexture.createFromDataBatched(
                    m_allocator->handle(), m_device->handle(), cfg,
                    yPacked.data(), ySize, cmd, stg))
                { m_cmdPool->endSingleTime(cmd, m_queue); return false; }
        } else {
            if (!m_yTexture.updateDataBatched(yPacked.data(), ySize, cmd, stg))
                { m_cmdPool->endSingleTime(cmd, m_queue); return false; }
        }
        if (stg.buffer != VK_NULL_HANDLE) staging.push_back(stg);
    }

    // ── Upload U plane at source resolution ─────────────────────────────
    {
        const VkDeviceSize uSize = static_cast<VkDeviceSize>(cw) * ch;
        std::vector<uint8_t> uPacked(static_cast<size_t>(uSize));
        for (uint32_t row = 0; row < ch; ++row)
            std::memcpy(uPacked.data() + row * cw, uData + row * uLinesize, cw);

        Texture::StagingCleanup stg{};
        if (m_uTexture.image() == VK_NULL_HANDLE ||
            m_uTexture.width() != cw || m_uTexture.height() != ch)
        {
            m_uTexture.destroy();
            TextureConfig cfg;
            cfg.width  = cw;
            cfg.height = ch;
            cfg.format = VK_FORMAT_R8_UNORM;
            cfg.usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            if (!m_uTexture.createFromDataBatched(
                    m_allocator->handle(), m_device->handle(), cfg,
                    uPacked.data(), uSize, cmd, stg))
                { m_cmdPool->endSingleTime(cmd, m_queue); return false; }
        } else {
            if (!m_uTexture.updateDataBatched(uPacked.data(), uSize, cmd, stg))
                { m_cmdPool->endSingleTime(cmd, m_queue); return false; }
        }
        if (stg.buffer != VK_NULL_HANDLE) staging.push_back(stg);
    }

    // ── Upload V plane at source resolution ─────────────────────────────
    {
        const VkDeviceSize vSize = static_cast<VkDeviceSize>(cw) * ch;
        std::vector<uint8_t> vPacked(static_cast<size_t>(vSize));
        for (uint32_t row = 0; row < ch; ++row)
            std::memcpy(vPacked.data() + row * cw, vData + row * vLinesize, cw);

        Texture::StagingCleanup stg{};
        if (m_vTexture.image() == VK_NULL_HANDLE ||
            m_vTexture.width() != cw || m_vTexture.height() != ch)
        {
            m_vTexture.destroy();
            TextureConfig cfg;
            cfg.width  = cw;
            cfg.height = ch;
            cfg.format = VK_FORMAT_R8_UNORM;
            cfg.usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            if (!m_vTexture.createFromDataBatched(
                    m_allocator->handle(), m_device->handle(), cfg,
                    vPacked.data(), vSize, cmd, stg))
                { m_cmdPool->endSingleTime(cmd, m_queue); return false; }
        } else {
            if (!m_vTexture.updateDataBatched(vPacked.data(), vSize, cmd, stg))
                { m_cmdPool->endSingleTime(cmd, m_queue); return false; }
        }
        if (stg.buffer != VK_NULL_HANDLE) staging.push_back(stg);
    }

    // ── Barrier: transfer → compute ─────────────────────────────────────
    {
        VkMemoryBarrier barrier{};
        barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 1, &barrier, 0, nullptr, 0, nullptr);
    }

    // ── Update YUV420P descriptor set ───────────────────────────────────
    {
        VkDescriptorImageInfo yInfo  = m_yTexture.descriptorInfo();
        VkDescriptorImageInfo uInfo  = m_uTexture.descriptorInfo();
        VkDescriptorImageInfo vInfo  = m_vTexture.descriptorInfo();
        VkDescriptorImageInfo outInfo{};
        outInfo.imageView   = m_outputTexture.imageView();
        outInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet writes[4]{};
        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = m_yuv420pDescSet;
        writes[0].dstBinding      = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo      = &yInfo;

        writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet          = m_yuv420pDescSet;
        writes[1].dstBinding      = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo      = &uInfo;

        writes[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet          = m_yuv420pDescSet;
        writes[2].dstBinding      = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[2].pImageInfo      = &vInfo;

        writes[3].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[3].dstSet          = m_yuv420pDescSet;
        writes[3].dstBinding      = 3;
        writes[3].descriptorCount = 1;
        writes[3].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[3].pImageInfo      = &outInfo;

        vkUpdateDescriptorSets(m_device->handle(), 4, writes, 0, nullptr);
    }

    // ── Dispatch at OUTPUT dimensions ───────────────────────────────────
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_yuv420pPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            m_yuv420pPipeLayout, 0, 1, &m_yuv420pDescSet,
                            0, nullptr);

    struct { int32_t w, h; } pc;
    pc.w = static_cast<int32_t>(dstW);
    pc.h = static_cast<int32_t>(dstH);
    vkCmdPushConstants(cmd, m_yuv420pPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(pc), &pc);

    uint32_t gx = (dstW + 15) / 16;
    uint32_t gy = (dstH + 15) / 16;
    vkCmdDispatch(cmd, gx, gy, 1);

    // ── Barrier: compute → transfer ─────────────────────────────────────
    {
        VkMemoryBarrier barrier{};
        barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT
                              | VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 1, &barrier, 0, nullptr, 0, nullptr);
    }

    m_cmdPool->endSingleTime(cmd, m_queue);

    for (auto& s : staging)
        s.destroy();

    return true;
}


// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Conversion
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

bool Nv12Converter::convert(VkCommandBuffer cmd,
                             const uint8_t* yData, int yLinesize,
                             const uint8_t* uvData, int uvLinesize,
                             uint32_t width, uint32_t height,
                             std::vector<Texture::StagingCleanup>& stagingOut)
{
    if (!m_initialized) return false;

    // Auto-resize if dimensions changed
    if (width != m_config.width || height != m_config.height) {
        if (!resize(width, height)) return false;
    }

    const uint32_t w = m_config.width;
    const uint32_t h = m_config.height;

    // â”€â”€ Upload Y plane (R8, W Ã— H) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    {
        // Pack Y plane into contiguous buffer (skip linesize padding)
        const VkDeviceSize ySize = static_cast<VkDeviceSize>(w) * h;
        std::vector<uint8_t> yPacked(static_cast<size_t>(ySize));
        for (uint32_t row = 0; row < h; ++row) {
            std::memcpy(yPacked.data() + row * w,
                        yData + row * yLinesize,
                        w);
        }

        Texture::StagingCleanup staging{};
        if (m_yTexture.image() == VK_NULL_HANDLE ||
            m_yTexture.width() != w || m_yTexture.height() != h)
        {
            m_yTexture.destroy();
            TextureConfig cfg;
            cfg.width  = w;
            cfg.height = h;
            cfg.format = VK_FORMAT_R8_UNORM;
            cfg.usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            if (!m_yTexture.createFromDataBatched(
                    m_allocator->handle(), m_device->handle(), cfg,
                    yPacked.data(), ySize, cmd, staging))
            {
                spdlog::error("Nv12Converter: Y plane upload failed");
                return false;
            }
        } else {
            if (!m_yTexture.updateDataBatched(
                    yPacked.data(), ySize, cmd, staging))
            {
                spdlog::error("Nv12Converter: Y plane re-upload failed");
                return false;
            }
        }
        if (staging.buffer != VK_NULL_HANDLE)
            stagingOut.push_back(staging);
    }

    // â”€â”€ Upload UV plane (RG8, W/2 Ã— H/2) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    {
        const uint32_t uvW = w / 2;
        const uint32_t uvH = h / 2;
        const VkDeviceSize uvSize = static_cast<VkDeviceSize>(uvW) * uvH * 2;
        std::vector<uint8_t> uvPacked(static_cast<size_t>(uvSize));
        for (uint32_t row = 0; row < uvH; ++row) {
            std::memcpy(uvPacked.data() + row * uvW * 2,
                        uvData + row * uvLinesize,
                        static_cast<size_t>(uvW) * 2);
        }

        Texture::StagingCleanup staging{};
        if (m_uvTexture.image() == VK_NULL_HANDLE ||
            m_uvTexture.width() != uvW || m_uvTexture.height() != uvH)
        {
            m_uvTexture.destroy();
            TextureConfig cfg;
            cfg.width  = uvW;
            cfg.height = uvH;
            cfg.format = VK_FORMAT_R8G8_UNORM;
            cfg.usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            if (!m_uvTexture.createFromDataBatched(
                    m_allocator->handle(), m_device->handle(), cfg,
                    uvPacked.data(), uvSize, cmd, staging))
            {
                spdlog::error("Nv12Converter: UV plane upload failed");
                return false;
            }
        } else {
            if (!m_uvTexture.updateDataBatched(
                    uvPacked.data(), uvSize, cmd, staging))
            {
                spdlog::error("Nv12Converter: UV plane re-upload failed");
                return false;
            }
        }
        if (staging.buffer != VK_NULL_HANDLE)
            stagingOut.push_back(staging);
    }

    // â”€â”€ Barrier: transfer writes â†’ compute reads â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    {
        VkMemoryBarrier barrier{};
        barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 1, &barrier, 0, nullptr, 0, nullptr);
    }

    // â”€â”€ Update descriptor set with current plane textures â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    {
        VkDescriptorImageInfo yInfo = m_yTexture.descriptorInfo();
        VkDescriptorImageInfo uvInfo = m_uvTexture.descriptorInfo();
        VkDescriptorImageInfo outInfo{};
        outInfo.imageView   = m_outputTexture.imageView();
        outInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet writes[3]{};

        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = m_descriptorSet;
        writes[0].dstBinding      = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo      = &yInfo;

        writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet          = m_descriptorSet;
        writes[1].dstBinding      = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo      = &uvInfo;

        writes[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet          = m_descriptorSet;
        writes[2].dstBinding      = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[2].pImageInfo      = &outInfo;

        vkUpdateDescriptorSets(m_device->handle(), 3, writes, 0, nullptr);
    }

    // â”€â”€ Dispatch compute shader â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            m_pipelineLayout, 0, 1, &m_descriptorSet,
                            0, nullptr);

    struct { int32_t w, h; } pc;
    pc.w = static_cast<int32_t>(w);
    pc.h = static_cast<int32_t>(h);
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(pc), &pc);

    uint32_t gx = (w + 15) / 16;
    uint32_t gy = (h + 15) / 16;
    vkCmdDispatch(cmd, gx, gy, 1);

    // â”€â”€ Barrier: compute writes â†’ shader reads â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    {
        VkMemoryBarrier barrier{};
        barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT
                              | VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 1, &barrier, 0, nullptr, 0, nullptr);
    }

    return true;
}

bool Nv12Converter::convertSync(const uint8_t* yData, int yLinesize,
                                 const uint8_t* uvData, int uvLinesize,
                                 uint32_t width, uint32_t height)
{
    if (!m_initialized || !m_cmdPool) return false;

    VkCommandBuffer cmd = m_cmdPool->beginSingleTime();
    std::vector<Texture::StagingCleanup> staging;
    bool ok = convert(cmd, yData, yLinesize, uvData, uvLinesize,
                      width, height, staging);
    m_cmdPool->endSingleTime(cmd, m_queue);

    for (auto& s : staging)
        s.destroy();

    return ok;
}

// ═══════════════════════════════════════════════════════════════════════════
//  ensureOutputSize — resize only the output texture (not input planes)
// ═══════════════════════════════════════════════════════════════════════════

bool Nv12Converter::ensureOutputSize(uint32_t w, uint32_t h)
{
    if (m_outputTexture.image() != VK_NULL_HANDLE &&
        m_outputTexture.width() == w && m_outputTexture.height() == h)
        return true;

    vkDeviceWaitIdle(m_device->handle());
    m_outputTexture.destroy();

    TextureConfig outCfg;
    outCfg.width  = w;
    outCfg.height = h;
    outCfg.format = VK_FORMAT_R8G8B8A8_UNORM;
    outCfg.usage  = VK_IMAGE_USAGE_STORAGE_BIT
                  | VK_IMAGE_USAGE_SAMPLED_BIT
                  | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (!m_outputTexture.create(m_allocator->handle(), m_device->handle(), outCfg))
        return false;

    VkCommandBuffer cmd = m_cmdPool->beginSingleTime();
    m_outputTexture.transitionLayout(
        cmd, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    m_cmdPool->endSingleTime(cmd, m_queue);

    // Update config to reflect output dimensions (for readbackOutput)
    m_config.width  = w;
    m_config.height = h;
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Scaled NV12→BGRA (upload at srcW×srcH, output at dstW×dstH)
// ═══════════════════════════════════════════════════════════════════════════

bool Nv12Converter::convertSyncScaled(const uint8_t* yData, int yLinesize,
                                       const uint8_t* uvData, int uvLinesize,
                                       uint32_t srcW, uint32_t srcH,
                                       uint32_t dstW, uint32_t dstH)
{
    if (!m_initialized || !m_cmdPool) return false;

    // Same-size: delegate to non-scaled path
    if (srcW == dstW && srcH == dstH)
        return convertSync(yData, yLinesize, uvData, uvLinesize, srcW, srcH);

    // Ensure output texture is at target (downscaled) size
    if (!ensureOutputSize(dstW, dstH)) return false;

    VkCommandBuffer cmd = m_cmdPool->beginSingleTime();
    std::vector<Texture::StagingCleanup> staging;

    // ── Upload Y plane at source resolution ─────────────────────────────
    {
        const VkDeviceSize ySize = static_cast<VkDeviceSize>(srcW) * srcH;
        std::vector<uint8_t> yPacked(static_cast<size_t>(ySize));
        for (uint32_t row = 0; row < srcH; ++row)
            std::memcpy(yPacked.data() + row * srcW, yData + row * yLinesize, srcW);

        Texture::StagingCleanup stg{};
        if (m_yTexture.image() == VK_NULL_HANDLE ||
            m_yTexture.width() != srcW || m_yTexture.height() != srcH)
        {
            m_yTexture.destroy();
            TextureConfig cfg;
            cfg.width  = srcW;
            cfg.height = srcH;
            cfg.format = VK_FORMAT_R8_UNORM;
            cfg.usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            if (!m_yTexture.createFromDataBatched(
                    m_allocator->handle(), m_device->handle(), cfg,
                    yPacked.data(), ySize, cmd, stg)) {
                m_cmdPool->endSingleTime(cmd, m_queue);
                return false;
            }
        } else {
            if (!m_yTexture.updateDataBatched(yPacked.data(), ySize, cmd, stg)) {
                m_cmdPool->endSingleTime(cmd, m_queue);
                return false;
            }
        }
        if (stg.buffer != VK_NULL_HANDLE) staging.push_back(stg);
    }

    // ── Upload UV plane at source resolution ────────────────────────────
    {
        const uint32_t uvW = srcW / 2;
        const uint32_t uvH = srcH / 2;
        const VkDeviceSize uvSize = static_cast<VkDeviceSize>(uvW) * uvH * 2;
        std::vector<uint8_t> uvPacked(static_cast<size_t>(uvSize));
        for (uint32_t row = 0; row < uvH; ++row)
            std::memcpy(uvPacked.data() + row * uvW * 2,
                        uvData + row * uvLinesize,
                        static_cast<size_t>(uvW) * 2);

        Texture::StagingCleanup stg{};
        if (m_uvTexture.image() == VK_NULL_HANDLE ||
            m_uvTexture.width() != uvW || m_uvTexture.height() != uvH)
        {
            m_uvTexture.destroy();
            TextureConfig cfg;
            cfg.width  = uvW;
            cfg.height = uvH;
            cfg.format = VK_FORMAT_R8G8_UNORM;
            cfg.usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            if (!m_uvTexture.createFromDataBatched(
                    m_allocator->handle(), m_device->handle(), cfg,
                    uvPacked.data(), uvSize, cmd, stg)) {
                m_cmdPool->endSingleTime(cmd, m_queue);
                return false;
            }
        } else {
            if (!m_uvTexture.updateDataBatched(uvPacked.data(), uvSize, cmd, stg)) {
                m_cmdPool->endSingleTime(cmd, m_queue);
                return false;
            }
        }
        if (stg.buffer != VK_NULL_HANDLE) staging.push_back(stg);
    }

    // ── Barrier: transfer → compute ─────────────────────────────────────
    {
        VkMemoryBarrier barrier{};
        barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 1, &barrier, 0, nullptr, 0, nullptr);
    }

    // ── Update descriptors ──────────────────────────────────────────────
    {
        VkDescriptorImageInfo yInfo = m_yTexture.descriptorInfo();
        VkDescriptorImageInfo uvInfo = m_uvTexture.descriptorInfo();
        VkDescriptorImageInfo outInfo{};
        outInfo.imageView   = m_outputTexture.imageView();
        outInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet writes[3]{};
        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = m_descriptorSet;
        writes[0].dstBinding      = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo      = &yInfo;

        writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet          = m_descriptorSet;
        writes[1].dstBinding      = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo      = &uvInfo;

        writes[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet          = m_descriptorSet;
        writes[2].dstBinding      = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[2].pImageInfo      = &outInfo;

        vkUpdateDescriptorSets(m_device->handle(), 3, writes, 0, nullptr);
    }

    // ── Dispatch at OUTPUT dimensions ───────────────────────────────────
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            m_pipelineLayout, 0, 1, &m_descriptorSet,
                            0, nullptr);

    struct { int32_t w, h; } pc;
    pc.w = static_cast<int32_t>(dstW);
    pc.h = static_cast<int32_t>(dstH);
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(pc), &pc);

    uint32_t gx = (dstW + 15) / 16;
    uint32_t gy = (dstH + 15) / 16;
    vkCmdDispatch(cmd, gx, gy, 1);

    // ── Barrier: compute → transfer ─────────────────────────────────────
    {
        VkMemoryBarrier barrier{};
        barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT
                              | VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 1, &barrier, 0, nullptr, 0, nullptr);
    }

    m_cmdPool->endSingleTime(cmd, m_queue);

    for (auto& s : staging)
        s.destroy();

    return true;
}


// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Convert from VkBuffer (GPUâ†’GPU, zero PCIe transfers)
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

bool Nv12Converter::convertFromVkBuffer(VkCommandBuffer cmd,
                                         VkBuffer nv12Buffer,
                                         uint32_t width, uint32_t height,
                                         VkDeviceSize yOffset,  uint32_t yRowPitch,
                                         VkDeviceSize uvOffset, uint32_t uvRowPitch)
{
    if (!m_initialized) return false;

    // Auto-resize if dimensions changed
    if (width != m_config.width || height != m_config.height) {
        if (!resize(width, height)) return false;
    }

    const uint32_t w = m_config.width;
    const uint32_t h = m_config.height;

    // â”€â”€ Ensure Y and UV textures exist â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    if (m_yTexture.image() == VK_NULL_HANDLE ||
        m_yTexture.width() != w || m_yTexture.height() != h)
    {
        m_yTexture.destroy();
        TextureConfig cfg;
        cfg.width  = w;
        cfg.height = h;
        cfg.format = VK_FORMAT_R8_UNORM;
        cfg.usage  = VK_IMAGE_USAGE_SAMPLED_BIT
                   | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (!m_yTexture.create(m_allocator->handle(), m_device->handle(), cfg))
            return false;
    }

    const uint32_t uvW = w / 2;
    const uint32_t uvH = h / 2;
    if (m_uvTexture.image() == VK_NULL_HANDLE ||
        m_uvTexture.width() != uvW || m_uvTexture.height() != uvH)
    {
        m_uvTexture.destroy();
        TextureConfig cfg;
        cfg.width  = uvW;
        cfg.height = uvH;
        cfg.format = VK_FORMAT_R8G8_UNORM;
        cfg.usage  = VK_IMAGE_USAGE_SAMPLED_BIT
                   | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (!m_uvTexture.create(m_allocator->handle(), m_device->handle(), cfg))
            return false;
    }

    // â”€â”€ Transition Y and UV textures to TRANSFER_DST â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    VkImageLayout yOldLayout = m_yTexture.layout();
    VkImageLayout uvOldLayout = m_uvTexture.layout();

    m_yTexture.transitionLayout(cmd, yOldLayout,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    m_uvTexture.transitionLayout(cmd, uvOldLayout,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    // â”€â”€ Copy Y plane from VkBuffer â†’ m_yTexture (R8, WÃ—H) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    {
        VkBufferImageCopy region{};
        region.bufferOffset    = yOffset;
        region.bufferRowLength = yRowPitch ? yRowPitch : 0; // 0 = tightly packed
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {w, h, 1};
        vkCmdCopyBufferToImage(cmd, nv12Buffer, m_yTexture.image(),
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1, &region);
    }

    // â”€â”€ Copy UV plane from VkBuffer â†’ m_uvTexture (RG8, W/2 Ã— H/2) â”€â”€â”€â”€â”€
    {
        VkBufferImageCopy region{};
        region.bufferOffset    = uvOffset ? uvOffset
                                         : static_cast<VkDeviceSize>(w) * h;
        region.bufferRowLength = uvRowPitch ? (uvRowPitch / 2) : 0;
        // bufferRowLength is in TEXELS for the image format. RG8 = 2 bytes/texel.
        // If uvRowPitch is in bytes and format is RG8, row length in texels = pitch/2.
        // 0 means tightly packed.
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {uvW, uvH, 1};
        vkCmdCopyBufferToImage(cmd, nv12Buffer, m_uvTexture.image(),
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1, &region);
    }

    // â”€â”€ Transition Y and UV back to SHADER_READ_ONLY â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    m_yTexture.transitionLayout(cmd,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    m_uvTexture.transitionLayout(cmd,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // â”€â”€ Barrier: transfer writes â†’ compute reads â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    {
        VkMemoryBarrier barrier{};
        barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 1, &barrier, 0, nullptr, 0, nullptr);
    }

    // â”€â”€ Descriptor update + compute dispatch (shared with convert()) â”€â”€â”€â”€
    {
        VkDescriptorImageInfo yInfo  = m_yTexture.descriptorInfo();
        VkDescriptorImageInfo uvInfo = m_uvTexture.descriptorInfo();
        VkDescriptorImageInfo outInfo{};
        outInfo.imageView   = m_outputTexture.imageView();
        outInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet writes[3]{};
        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = m_descriptorSet;
        writes[0].dstBinding      = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo      = &yInfo;

        writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet          = m_descriptorSet;
        writes[1].dstBinding      = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo      = &uvInfo;

        writes[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet          = m_descriptorSet;
        writes[2].dstBinding      = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[2].pImageInfo      = &outInfo;

        vkUpdateDescriptorSets(m_device->handle(), 3, writes, 0, nullptr);
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            m_pipelineLayout, 0, 1, &m_descriptorSet,
                            0, nullptr);

    struct { int32_t w, h; } pc;
    pc.w = static_cast<int32_t>(w);
    pc.h = static_cast<int32_t>(h);
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(pc), &pc);

    uint32_t gx = (w + 15) / 16;
    uint32_t gy = (h + 15) / 16;
    vkCmdDispatch(cmd, gx, gy, 1);

    // â”€â”€ Barrier: compute writes â†’ shader reads / transfer reads â”€â”€â”€â”€â”€â”€â”€â”€â”€
    {
        VkMemoryBarrier barrier{};
        barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT
                              | VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 1, &barrier, 0, nullptr, 0, nullptr);
    }

    return true;
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Resize
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

bool Nv12Converter::resize(uint32_t width, uint32_t height)
{
    if (width == m_config.width && height == m_config.height) return true;

    m_config.width  = width;
    m_config.height = height;

    if (m_initialized) {
        vkDeviceWaitIdle(m_device->handle());
        m_yTexture.destroy();
        m_uvTexture.destroy();
        m_uTexture.destroy();
        m_vTexture.destroy();
        m_outputTexture.destroy();
        if (!createTextures()) return false;
    }
    return true;
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Output access
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

VkDescriptorImageInfo Nv12Converter::outputDescriptorInfo() const
{
    VkDescriptorImageInfo info{};
    info.sampler     = m_outputTexture.sampler();
    info.imageView   = m_outputTexture.imageView();
    info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    return info;
}

bool Nv12Converter::readbackOutput(std::vector<uint8_t>& outPixels)
{
    if (!m_initialized) return false;

    const uint32_t w = m_config.width;
    const uint32_t h = m_config.height;
    const VkDeviceSize bufSize = static_cast<VkDeviceSize>(w) * h * 4;

    // Create staging buffer
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size  = bufSize;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_CPU_ONLY;

    VkBuffer stagingBuf{VK_NULL_HANDLE};
    VmaAllocation stagingAlloc{nullptr};
    if (vmaCreateBuffer(m_allocator->handle(), &bci, &aci,
                        &stagingBuf, &stagingAlloc, nullptr) != VK_SUCCESS)
        return false;

    VkCommandBuffer cmd = m_cmdPool->beginSingleTime();

    m_outputTexture.transitionLayout(
        cmd, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(cmd, m_outputTexture.image(),
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           stagingBuf, 1, &region);

    m_outputTexture.transitionLayout(
        cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);

    m_cmdPool->endSingleTime(cmd, m_queue);

    outPixels.resize(static_cast<size_t>(bufSize));
    void* mapped = nullptr;
    vmaMapMemory(m_allocator->handle(), stagingAlloc, &mapped);
    std::memcpy(outPixels.data(), mapped, bufSize);
    vmaUnmapMemory(m_allocator->handle(), stagingAlloc);
    vmaDestroyBuffer(m_allocator->handle(), stagingBuf, stagingAlloc);

    return true;
}


} // namespace rt
