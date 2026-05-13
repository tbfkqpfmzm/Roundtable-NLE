/*
 * Nv12ConverterYuv420p.cpp — YUV420P → BGRA conversion for Nv12Converter.
 *
 * Extracted from Nv12Converter.cpp to reduce translation unit size.
 * Contains: convertYuv420pSync(), convertYuv420pSyncScaled()
 */

#include <volk.h>
#include "Nv12Converter.h"

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vk_mem_alloc.h>

#include <spdlog/spdlog.h>
#include <cstring>
#include <vector>

namespace rt {

//══════════════════════════════════════════════════════════════════════════
//  YUV420P → BGRA synchronous conversion (CPU upload → GPU compute)
//══════════════════════════════════════════════════════════════════════════

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

    // ── Upload Y plane (R8, W × H) ─────────────────────────────────────
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

    // ── Upload U plane (R8, W/2 × H/2) ─────────────────────────────────
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

    // ── Upload V plane (R8, W/2 × H/2) ─────────────────────────────────
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

    // ── Barrier: transfer writes → compute reads ───────────────────────
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

    // ── Update YUV420P descriptor set ──────────────────────────────────
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

    // ── Dispatch compute shader ────────────────────────────────────────
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

    // ── Barrier: compute writes → shader reads / transfer reads ────────
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

//══════════════════════════════════════════════════════════════════════════
//  Scaled YUV420P→BGRA (upload at srcW×srcH, output at dstW×dstH)
//══════════════════════════════════════════════════════════════════════════

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

} // namespace rt
