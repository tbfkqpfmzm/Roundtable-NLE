/*
 * EffectProcessor.cpp — GPU compute-shader effects pipeline.
 *
 * Step 22: Effects System
 *
 * Applies effects using Vulkan compute shaders with ping-pong storage images.
 * Each effect is dispatched as a separate compute shader invocation.
 * The processor chains effects by alternating read/write between two images.
 *
 * Shader bindings (must match color_correct.comp, blur.comp, etc.):
 *   binding 0: writeonly image2D  (output storage image)
 *   binding 1: sampler2D          (input combined image sampler)
 *
 * Push constants: EffectPushConstants (width, height, paramCount, params[16])
 */

#include <volk.h>
#include "EffectProcessor.h"
#include "GpuContext.h"

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vk_mem_alloc.h>

#include <spdlog/spdlog.h>
#include <algorithm>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

namespace rt {

// ═══════════════════════════════════════════════════════════════════════════
//  Construction / Destruction
// ═══════════════════════════════════════════════════════════════════════════

EffectProcessor::EffectProcessor() = default;

EffectProcessor::~EffectProcessor()
{
    shutdown();
}

// ═══════════════════════════════════════════════════════════════════════════
//  Helper: locate SPIR-V shader file
// ═══════════════════════════════════════════════════════════════════════════

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

// ═══════════════════════════════════════════════════════════════════════════
//  Lifecycle
// ═══════════════════════════════════════════════════════════════════════════

bool EffectProcessor::init(Device& device,
                           Allocator& allocator,
                           CommandPool& cmdPool,
                           VkQueue computeQueue,
                           const EffectProcessorConfig& config)
{
    m_device    = &device;
    m_allocator = &allocator;
    m_cmdPool   = &cmdPool;
    m_queue     = computeQueue;
    m_config    = config;

    // Query timestamp period for GPU timing
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(device.physicalDevice(), &props);
    m_timestampPeriod = props.limits.timestampPeriod;

    if (!createStorageTextures()) {
        spdlog::error("EffectProcessor: failed to create storage textures");
        return false;
    }

    if (!createDescriptorResources()) {
        spdlog::error("EffectProcessor: failed to create descriptor resources");
        return false;
    }

    if (!createPipelines()) {
        spdlog::error("EffectProcessor: failed to create pipelines");
        return false;
    }

    // Create timestamp query pool (2 queries: begin + end)
    VkQueryPoolCreateInfo qpci{};
    qpci.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    qpci.queryType   = VK_QUERY_TYPE_TIMESTAMP;
    qpci.queryCount  = 2;
    if (vkCreateQueryPool(device.handle(), &qpci, nullptr, &m_queryPool) != VK_SUCCESS)
        m_queryPool = VK_NULL_HANDLE; // non-fatal

    // Create a reusable fence for processSync (avoids per-call create/destroy)
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vkCreateFence(device.handle(), &fci, nullptr, &m_syncFence) != VK_SUCCESS)
        m_syncFence = VK_NULL_HANDLE; // non-fatal, will fall back to endSingleTime

    m_initialized = true;
    spdlog::info("EffectProcessor initialized ({}x{})", m_config.width, m_config.height);
    return true;
}

void EffectProcessor::shutdown()
{
    if (!m_device) return;

    VkDevice dev = m_device->handle();
    if (dev) vkDeviceWaitIdle(dev);

    // Query pool
    if (m_queryPool != VK_NULL_HANDLE) {
        vkDestroyQueryPool(dev, m_queryPool, nullptr);
        m_queryPool = VK_NULL_HANDLE;
    }

    // Reusable fence
    if (m_syncFence != VK_NULL_HANDLE) {
        vkDestroyFence(dev, m_syncFence, nullptr);
        m_syncFence = VK_NULL_HANDLE;
    }

    // Descriptor resources
    if (m_descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(dev, m_descriptorPool, nullptr);
        m_descriptorPool      = VK_NULL_HANDLE;
        m_descriptorSets[0]   = VK_NULL_HANDLE;
        m_descriptorSets[1]   = VK_NULL_HANDLE;
        m_sourceDescriptorSet = VK_NULL_HANDLE;
    }
    if (m_descriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, m_descriptorSetLayout, nullptr);
        m_descriptorSetLayout = VK_NULL_HANDLE;
    }

    // Pipelines
    m_pipelineManager.destroy();
    m_colorCorrectPipeline = VK_NULL_HANDLE;
    m_blurPipeline         = VK_NULL_HANDLE;
    m_sharpenPipeline      = VK_NULL_HANDLE;
    m_glowPipeline         = VK_NULL_HANDLE;
    m_chromaKeyPipeline    = VK_NULL_HANDLE;
    m_ultraKeyMattePipeline    = VK_NULL_HANDLE;
    m_ultraKeyCleanupPipeline  = VK_NULL_HANDLE;
    m_ultraKeyFinalizePipeline = VK_NULL_HANDLE;
    m_transform2dPipeline  = VK_NULL_HANDLE;
    m_pipelineLayout       = VK_NULL_HANDLE;

    // Storage textures + placeholder
    m_storageTextures[0].destroy();
    m_storageTextures[1].destroy();
    m_placeholderTexture.destroy();

    m_initialized = false;
    m_device      = nullptr;
    m_allocator   = nullptr;
    m_cmdPool     = nullptr;
    m_queue       = VK_NULL_HANDLE;
    spdlog::info("EffectProcessor shut down");
}

// ═══════════════════════════════════════════════════════════════════════════
//  Processing
// ═══════════════════════════════════════════════════════════════════════════

bool EffectProcessor::process(VkCommandBuffer cmd,
                              const VkDescriptorImageInfo& sourceImage,
                              const std::vector<EffectStack::EffectSnapshot>& effects)
{
    if (!m_initialized) return false;
    if (effects.empty()) return true;

    m_stats.effectsApplied = static_cast<int>(effects.size());
    m_currentOutput = 0;

    // Update the source descriptor set to point at the caller's image
    // (skip if the source hasn't changed since last call)
    if (sourceImage.imageView != m_lastSourceImageView)
    {
        VkDescriptorImageInfo srcInfo = sourceImage;
        // Ensure layout is set for shader read
        if (srcInfo.imageLayout == VK_IMAGE_LAYOUT_UNDEFINED)
            srcInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = m_sourceDescriptorSet;
        write.dstBinding      = 1; // input sampler at binding 1
        write.descriptorCount = 1;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo      = &srcInfo;
        vkUpdateDescriptorSets(m_device->handle(), 1, &write, 0, nullptr);
        m_lastSourceImageView = sourceImage.imageView;
    }

    // Timestamp begin
    if (m_queryPool)
        vkCmdResetQueryPool(cmd, m_queryPool, 0, 2);
    if (m_queryPool)
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_queryPool, 0);

    // For each effect, dispatch compute shader
    int sourceIdx = -1;  // -1 = use external source via m_sourceDescriptorSet
    int targetIdx = 0;

    for (size_t i = 0; i < effects.size(); ++i) {
        const auto& snap = effects[i];

        // Ultra Key uses a 3-pass pipeline (matte → cleanup → finalize)
        if (snap.type == EffectType::ChromaKey &&
            m_ultraKeyMattePipeline != VK_NULL_HANDLE)
        {
            int finalTarget = dispatchUltraKey(cmd, snap.params, sourceIdx, targetIdx);
            sourceIdx = finalTarget;
            targetIdx = 1 - finalTarget;
        }
        else if (!dispatchEffect(cmd, snap.type, snap.params, sourceIdx, targetIdx)) {
            spdlog::warn("EffectProcessor: failed to dispatch effect type {}",
                         static_cast<int>(snap.type));
        } else {
            // Ping-pong
            sourceIdx = targetIdx;
            targetIdx = 1 - targetIdx;
        }
    }

    // Timestamp end
    if (m_queryPool)
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_queryPool, 1);

    m_currentOutput = sourceIdx;
    return true;
}

bool EffectProcessor::processSync(const VkDescriptorImageInfo& sourceImage,
                                  const std::vector<EffectStack::EffectSnapshot>& effects)
{
    if (!m_initialized || !m_cmdPool) return false;
    if (effects.empty()) return true;

    VkCommandBuffer cmd = m_cmdPool->beginSingleTime();
    bool ok = process(cmd, sourceImage, effects);
    vkEndCommandBuffer(cmd);

    // Use persistent fence (reset + reuse) to avoid per-call create/destroy
    if (ok && m_syncFence != VK_NULL_HANDLE) {
        vkWaitForFences(m_device->handle(), 1, &m_syncFence, VK_TRUE, UINT64_MAX);
        vkResetFences(m_device->handle(), 1, &m_syncFence);

        VkSubmitInfo submitInfo{};
        submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers    = &cmd;
        {
            // EffectProcessor uses the compute queue by default.
            // Lock the compute queue mutex before submitting.
            std::lock_guard lock(rt::GpuContext::get().computeQueueMutex());
            vkQueueSubmit(m_queue, 1, &submitInfo, m_syncFence);
        }
        vkWaitForFences(m_device->handle(), 1, &m_syncFence, VK_TRUE, UINT64_MAX);

        m_cmdPool->freeBuffer(cmd);
    } else {
        // Fallback: use CommandPool's single-time path
        m_cmdPool->endSingleTime(cmd, m_queue);
    }

    // Read back GPU timestamps
    if (ok && m_queryPool) {
        uint64_t ts[2]{};
        VkResult qr = vkGetQueryPoolResults(
            m_device->handle(), m_queryPool,
            0, 2, sizeof(ts), ts, sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        if (qr == VK_SUCCESS)
            m_stats.gpuTimeMs = static_cast<float>(ts[1] - ts[0])
                              * m_timestampPeriod / 1e6f;
    }

    return ok;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Resize
// ═══════════════════════════════════════════════════════════════════════════

bool EffectProcessor::resize(uint32_t width, uint32_t height)
{
    if (width == m_config.width && height == m_config.height) return true;
    m_config.width  = width;
    m_config.height = height;

    if (m_initialized) {
        // Wait only on our compute queue rather than draining the entire
        // device — avoids stalling the graphics queue when processing
        // mixed-resolution clips back-to-back.
        vkQueueWaitIdle(m_queue);
        m_storageTextures[0].destroy();
        m_storageTextures[1].destroy();
        if (!createStorageTextures()) return false;

        // Re-point descriptor sets at new storage textures
        for (int i = 0; i < 2; ++i) {
            VkDescriptorImageInfo outInfo{};
            outInfo.imageView   = m_storageTextures[i].imageView();
            outInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            VkWriteDescriptorSet w{};
            w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet          = m_descriptorSets[i];
            w.dstBinding      = 0; // output storage image
            w.descriptorCount = 1;
            w.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            w.pImageInfo      = &outInfo;
            vkUpdateDescriptorSets(m_device->handle(), 1, &w, 0, nullptr);
        }
        // Invalidate cached source descriptor — storage images changed
        m_lastSourceImageView = VK_NULL_HANDLE;
    }
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Output access
// ═══════════════════════════════════════════════════════════════════════════

VkImage EffectProcessor::outputImage() const noexcept
{
    return m_storageTextures[m_currentOutput].image();
}

VkImageView EffectProcessor::outputImageView() const noexcept
{
    return m_storageTextures[m_currentOutput].imageView();
}

VkDescriptorImageInfo EffectProcessor::outputDescriptorInfo() const
{
    VkDescriptorImageInfo info{};
    info.sampler     = m_storageTextures[m_currentOutput].sampler();
    info.imageView   = outputImageView();
    info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    return info;
}

bool EffectProcessor::readbackOutput(std::vector<uint8_t>& outPixels)
{
    if (!m_initialized) return false;

    const VkDeviceSize bufSize = static_cast<VkDeviceSize>(m_config.width)
                               * m_config.height * 4;

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

    // Copy image → buffer
    VkCommandBuffer cmd = m_cmdPool->beginSingleTime();

    m_storageTextures[m_currentOutput].transitionLayout(
        cmd, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {m_config.width, m_config.height, 1};
    vkCmdCopyImageToBuffer(cmd, m_storageTextures[m_currentOutput].image(),
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           stagingBuf, 1, &region);

    m_storageTextures[m_currentOutput].transitionLayout(
        cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);

    m_cmdPool->endSingleTime(cmd, m_queue);

    // Map and copy to CPU
    void* mapped = nullptr;
    vmaMapMemory(m_allocator->handle(), stagingAlloc, &mapped);
    outPixels.resize(static_cast<size_t>(bufSize));
    std::memcpy(outPixels.data(), mapped, bufSize);
    vmaUnmapMemory(m_allocator->handle(), stagingAlloc);

    vmaDestroyBuffer(m_allocator->handle(), stagingBuf, stagingAlloc);
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Internal — create ping-pong storage textures
// ═══════════════════════════════════════════════════════════════════════════

bool EffectProcessor::createStorageTextures()
{
    for (int i = 0; i < 2; ++i) {
        TextureConfig cfg;
        cfg.width  = m_config.width;
        cfg.height = m_config.height;
        cfg.format = m_config.format;
        cfg.usage  = VK_IMAGE_USAGE_STORAGE_BIT
                   | VK_IMAGE_USAGE_SAMPLED_BIT
                   | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                   | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

        if (!m_storageTextures[i].create(m_allocator->handle(),
                                          m_device->handle(), cfg))
        {
            spdlog::error("EffectProcessor: Failed to create storage texture {}", i);
            return false;
        }

        // Transition to GENERAL for compute shader read/write
        VkCommandBuffer cmd = m_cmdPool->beginSingleTime();
        m_storageTextures[i].transitionLayout(
            cmd, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        m_cmdPool->endSingleTime(cmd, m_queue);
    }

    // Also create a 1×1 placeholder (for unused sampler slots)
    TextureConfig phCfg;
    phCfg.width  = 1;
    phCfg.height = 1;
    phCfg.format = m_config.format;
    phCfg.usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    uint32_t transparent = 0;
    if (!m_placeholderTexture.createFromData(
            m_allocator->handle(), m_device->handle(), phCfg,
            &transparent, 4, *m_cmdPool, m_queue))
    {
        spdlog::error("EffectProcessor: Failed to create placeholder texture");
        return false;
    }

    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Internal — create descriptor set layout, pool, and sets
// ═══════════════════════════════════════════════════════════════════════════

bool EffectProcessor::createDescriptorResources()
{
    VkDevice dev = m_device->handle();

    // Layout matches the effect shaders:
    //   binding 0: storage image   (writeonly output)
    //   binding 1: combined sampler (readonly input)
    VkDescriptorSetLayoutBinding bindings[2]{};

    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutCI{};
    layoutCI.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutCI.bindingCount = 2;
    layoutCI.pBindings    = bindings;

    if (vkCreateDescriptorSetLayout(dev, &layoutCI, nullptr,
                                     &m_descriptorSetLayout) != VK_SUCCESS)
    {
        spdlog::error("EffectProcessor: Failed to create descriptor set layout");
        return false;
    }

    // Pool: need 3 sets (ping-pong pair + source set).
    //   2 storage images + 3 combined samplers = enough.
    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[0].descriptorCount = 3;
    poolSizes[1].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = 3;

    VkDescriptorPoolCreateInfo poolCI{};
    poolCI.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolCI.maxSets       = 3;
    poolCI.poolSizeCount = 2;
    poolCI.pPoolSizes    = poolSizes;

    if (vkCreateDescriptorPool(dev, &poolCI, nullptr,
                                &m_descriptorPool) != VK_SUCCESS)
    {
        spdlog::error("EffectProcessor: Failed to create descriptor pool");
        return false;
    }

    // Allocate 3 descriptor sets from the same layout
    VkDescriptorSetLayout layouts[3] = {
        m_descriptorSetLayout, m_descriptorSetLayout, m_descriptorSetLayout
    };
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_descriptorPool;
    allocInfo.descriptorSetCount = 3;
    allocInfo.pSetLayouts        = layouts;

    VkDescriptorSet sets[3];
    if (vkAllocateDescriptorSets(dev, &allocInfo, sets) != VK_SUCCESS) {
        spdlog::error("EffectProcessor: Failed to allocate descriptor sets");
        return false;
    }
    m_descriptorSets[0]   = sets[0];  // target = storageTexture[0]
    m_descriptorSets[1]   = sets[1];  // target = storageTexture[1]
    m_sourceDescriptorSet = sets[2];  // external source input

    // ── Initialize descriptor sets for the ping-pong pair ───────────────
    // Each set has:
    //   binding 0 → storageTexture[i]  (output)
    //   binding 1 → storageTexture[1-i] (input, from previous ping-pong)
    //
    // The source descriptor set (sets[2]) has:
    //   binding 0 → storageTexture[0] (output for first effect)
    //   binding 1 → external source (will be updated per-call)

    for (int i = 0; i < 2; ++i) {
        VkDescriptorImageInfo outInfo{};
        outInfo.imageView   = m_storageTextures[i].imageView();
        outInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo inInfo{};
        inInfo.sampler     = m_storageTextures[1 - i].sampler();
        inInfo.imageView   = m_storageTextures[1 - i].imageView();
        inInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet writes[2]{};
        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = m_descriptorSets[i];
        writes[0].dstBinding      = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[0].pImageInfo      = &outInfo;

        writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet          = m_descriptorSets[i];
        writes[1].dstBinding      = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo      = &inInfo;

        vkUpdateDescriptorSets(dev, 2, writes, 0, nullptr);
    }

    // Source set: binding 0 → storageTexture[0], binding 1 → placeholder
    {
        VkDescriptorImageInfo outInfo{};
        outInfo.imageView   = m_storageTextures[0].imageView();
        outInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo phInfo = m_placeholderTexture.descriptorInfo();

        VkWriteDescriptorSet writes[2]{};
        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = m_sourceDescriptorSet;
        writes[0].dstBinding      = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[0].pImageInfo      = &outInfo;

        writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet          = m_sourceDescriptorSet;
        writes[1].dstBinding      = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo      = &phInfo;

        vkUpdateDescriptorSets(dev, 2, writes, 0, nullptr);
    }

    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Internal — create compute pipelines (one per effect type)
// ═══════════════════════════════════════════════════════════════════════════

bool EffectProcessor::createPipelines()
{
    if (!m_pipelineManager.create(*m_device)) {
        spdlog::error("EffectProcessor: Failed to create pipeline manager");
        return false;
    }

    // Pipeline layout shared by all effect shaders
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset     = 0;
    pushRange.size       = sizeof(EffectPushConstants);

    m_pipelineLayout = m_pipelineManager.createLayout(
        {pushRange}, {m_descriptorSetLayout});
    if (m_pipelineLayout == VK_NULL_HANDLE) {
        spdlog::error("EffectProcessor: Failed to create pipeline layout");
        return false;
    }

    // Helper lambda — load shader + create pipeline.  Non-fatal if not found
    // (we just leave that pipeline null and skip it at dispatch time).
    auto loadPipeline = [&](const char* spvName) -> VkPipeline {
        fs::path path = findShader(spvName);
        if (path.empty()) {
            spdlog::warn("EffectProcessor: shader {} not found — skipping", spvName);
            return VK_NULL_HANDLE;
        }
        VkShaderModule mod = m_pipelineManager.loadShader(path);
        if (mod == VK_NULL_HANDLE) {
            spdlog::warn("EffectProcessor: failed to load {}", spvName);
            return VK_NULL_HANDLE;
        }
        ComputePipelineConfig pc;
        pc.compShader = mod;
        pc.layout     = m_pipelineLayout;
        VkPipeline p = m_pipelineManager.createComputePipeline(pc);
        if (p == VK_NULL_HANDLE)
            spdlog::warn("EffectProcessor: failed to create pipeline for {}", spvName);
        return p;
    };

    m_colorCorrectPipeline = loadPipeline("color_correct.comp.spv");
    m_blurPipeline         = loadPipeline("blur.comp.spv");
    m_sharpenPipeline      = loadPipeline("sharpen.comp.spv");
    m_glowPipeline         = loadPipeline("glow.comp.spv");
    m_chromaKeyPipeline    = loadPipeline("chroma_key.comp.spv");
    m_ultraKeyMattePipeline    = loadPipeline("ultra_key_matte.comp.spv");
    m_ultraKeyCleanupPipeline  = loadPipeline("ultra_key_cleanup.comp.spv");
    m_ultraKeyFinalizePipeline = loadPipeline("ultra_key_finalize.comp.spv");
    m_transform2dPipeline  = loadPipeline("transform2d.comp.spv");
    m_vignettePipeline    = loadPipeline("vignette.comp.spv");
    m_lutPipeline         = loadPipeline("lut.comp.spv");
    m_letterboxPipeline   = loadPipeline("letterbox.comp.spv");
    m_lumetriColorPipeline = loadPipeline("lumetri_color.comp.spv");
    m_otsPipeline         = loadPipeline("ots.comp.spv");

    spdlog::info("EffectProcessor: pipelines created (colorCorrect={}, blur={}, lumetri={})",
                 m_colorCorrectPipeline != VK_NULL_HANDLE,
                 m_blurPipeline != VK_NULL_HANDLE,
                 m_lumetriColorPipeline != VK_NULL_HANDLE);
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Internal — dispatch a single effect
// ═══════════════════════════════════════════════════════════════════════════

bool EffectProcessor::dispatchEffect(VkCommandBuffer cmd,
                                     EffectType type,
                                     const std::vector<float>& params,
                                     int sourceIdx, int targetIdx)
{
    VkPipeline pipeline = getPipeline(type);
    if (pipeline == VK_NULL_HANDLE) return false;

    // Select descriptor set:
    //   sourceIdx == -1 → use m_sourceDescriptorSet (external source image)
    //   sourceIdx ==  0 or 1 → use m_descriptorSets[targetIdx]
    //     (descriptorSets[targetIdx] has binding0 = storage[targetIdx],
    //      binding1 = storage[1-targetIdx] which is the source)
    VkDescriptorSet ds;
    if (sourceIdx == -1) {
        // First effect: source is external image (already updated in process()).
        // The sourceDescriptorSet has binding0 = storage[0] (output).
        ds = m_sourceDescriptorSet;
    } else {
        // Subsequent effects: ping-pong.
        // m_descriptorSets[targetIdx] has binding0 = storage[targetIdx] (output),
        // binding1 = storage[1-targetIdx] = storage[sourceIdx] (input).
        ds = m_descriptorSets[targetIdx];
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            m_pipelineLayout, 0, 1, &ds, 0, nullptr);

    // Push constants
    EffectPushConstants pc{};
    pc.width      = static_cast<int32_t>(m_config.width);
    pc.height     = static_cast<int32_t>(m_config.height);
    pc.paramCount = static_cast<int32_t>(std::min(params.size(), size_t{28}));
    for (int i = 0; i < pc.paramCount; ++i)
        pc.params[i] = params[static_cast<size_t>(i)];

    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(pc), &pc);

    // Dispatch: 16×16 workgroups
    uint32_t gx = (m_config.width  + 15) / 16;
    uint32_t gy = (m_config.height + 15) / 16;
    vkCmdDispatch(cmd, gx, gy, 1);

    // Pipeline barrier: compute write → compute read for next effect
    VkMemoryBarrier barrier{};
    barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &barrier, 0, nullptr, 0, nullptr);

    return true;
}

VkPipeline EffectProcessor::getPipeline(EffectType type) const
{
    switch (type) {
    case EffectType::ColorCorrect: return m_colorCorrectPipeline;
    case EffectType::Blur:         return m_blurPipeline;
    case EffectType::Sharpen:      return m_sharpenPipeline;
    case EffectType::Glow:         return m_glowPipeline;
    case EffectType::ChromaKey:    return m_chromaKeyPipeline;
    case EffectType::Transform2D:  return m_transform2dPipeline;
    case EffectType::Vignette:      return m_vignettePipeline;
    case EffectType::LUT:            return m_lutPipeline;
    case EffectType::Letterbox:      return m_letterboxPipeline;
    case EffectType::LumetriColor:   return m_lumetriColorPipeline;
    case EffectType::OtsLeft:        return m_otsPipeline;
    case EffectType::OtsRight:       return m_otsPipeline;
    default:                       return VK_NULL_HANDLE;
    }
}

// ═════════════════════════════════════════════════════════════════════════
//  Ultra Key — 3-pass dispatch
// ═════════════════════════════════════════════════════════════════════════

void EffectProcessor::dispatchPass(VkCommandBuffer cmd, VkPipeline pipeline,
                                   VkDescriptorSet ds,
                                   const std::vector<float>& params)
{
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            m_pipelineLayout, 0, 1, &ds, 0, nullptr);

    EffectPushConstants pc{};
    pc.width      = static_cast<int32_t>(m_config.width);
    pc.height     = static_cast<int32_t>(m_config.height);
    pc.paramCount = static_cast<int32_t>(std::min(params.size(), size_t{28}));
    for (int i = 0; i < pc.paramCount; ++i)
        pc.params[i] = params[static_cast<size_t>(i)];

    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(pc), &pc);

    uint32_t gx = (m_config.width  + 15) / 16;
    uint32_t gy = (m_config.height + 15) / 16;
    vkCmdDispatch(cmd, gx, gy, 1);

    // Barrier between passes
    VkMemoryBarrier barrier{};
    barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &barrier, 0, nullptr, 0, nullptr);
}

int EffectProcessor::dispatchUltraKey(VkCommandBuffer cmd,
                                      const std::vector<float>& params,
                                      int sourceIdx, int targetIdx)
{
    // Pass 1: Matte generation  (source → target)
    {
        VkDescriptorSet ds = (sourceIdx == -1) ? m_sourceDescriptorSet
                                               : m_descriptorSets[targetIdx];
        dispatchPass(cmd, m_ultraKeyMattePipeline, ds, params);
    }

    int pass1Output = targetIdx;
    int pass2Target = 1 - pass1Output;

    // Pass 2: Matte cleanup  (pass1Output → pass2Target)
    {
        VkDescriptorSet ds = m_descriptorSets[pass2Target];
        dispatchPass(cmd, m_ultraKeyCleanupPipeline, ds, params);
    }

    int pass2Output = pass2Target;
    int pass3Target = 1 - pass2Output;

    // Pass 3: Finalize (spill suppress + color correct + output mode)
    {
        VkDescriptorSet ds = m_descriptorSets[pass3Target];
        dispatchPass(cmd, m_ultraKeyFinalizePipeline, ds, params);
    }

    return pass3Target; // final output slot
}

} // namespace rt
