/*
 * CudaContext — CUDA device initialization via dynamic loading.
 *
 * Loads cuda.dll / nvcuda.dll at runtime from the NVIDIA driver.
 * No CUDA Toolkit install required — just an NVIDIA GPU with drivers.
 *
 * When ROUNDTABLE_HAS_CUDA is defined (CUDA Toolkit found at build time),
 * this uses native CUDA APIs. Otherwise, stub implementation.
 */

#pragma once

#include <cstdint>
#include <string>

namespace rt {

struct CudaDeviceInfo
{
    int          deviceIndex{-1};
    std::string  name;
    int          computeMajor{0};
    int          computeMinor{0};
    size_t       totalMemory{0};
    size_t       freeMemory{0};
    bool         nvdecSupported{false};
    bool         nvencSupported{false};
};

class CudaContext
{
public:
    CudaContext();
    ~CudaContext();

    // Non-copyable
    CudaContext(const CudaContext&) = delete;
    CudaContext& operator=(const CudaContext&) = delete;

    /// Initialize CUDA — returns true if a suitable GPU is found.
    bool init(int preferredDevice = 0);

    /// Shut down CUDA context.
    void shutdown();

    /// True if CUDA is initialized and usable.
    [[nodiscard]] bool isAvailable() const noexcept { return m_available; }

    /// Device info for the active GPU.
    [[nodiscard]] const CudaDeviceInfo& deviceInfo() const noexcept { return m_info; }

    /// Raw CUDA context handle (CUcontext). Returns nullptr if unavailable.
    [[nodiscard]] void* nativeContext() const noexcept { return m_context; }

    /// Raw CUDA device (CUdevice as int).
    [[nodiscard]] int nativeDevice() const noexcept { return m_device; }

    /// Push CUDA context onto current thread's context stack.
    /// Must be paired with popContext(). Safe to call from any thread.
    void pushContext();

    /// Pop CUDA context from current thread's context stack.
    void popContext();

    /// RAII guard that pushes on construction and pops on destruction.
    /// Guarantees paired push/pop even on exceptions.
    /// Usage: { CudaContextGuard guard(cuda); ... CUDA calls ... }
    class CudaContextGuard
    {
    public:
        explicit CudaContextGuard(CudaContext& ctx) : m_ctx(&ctx)
        {
            m_ctx->pushContext();
        }
        ~CudaContextGuard()
        {
            m_ctx->popContext();
        }
        CudaContextGuard(const CudaContextGuard&) = delete;
        CudaContextGuard& operator=(const CudaContextGuard&) = delete;

    private:
        CudaContext* m_ctx;
    };

private:
    bool        m_available{false};
    int         m_device{-1};
    void*       m_context{nullptr}; // CUcontext
    CudaDeviceInfo m_info;
};

} // namespace rt
