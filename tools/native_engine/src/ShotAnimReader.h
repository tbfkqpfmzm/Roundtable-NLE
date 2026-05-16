/*
 * ShotAnimReader — loads a .shotanim binary blob into rt::SkeletonPkg.
 *
 * This reader has ZERO spine-cpp dependency. It only knows our own
 * binary format. It is the only "import" code path used by the
 * NativeSpineEngine at runtime.
 *
 * Lifecycle:
 *   1. Construct ShotAnimReader
 *   2. Call read(path, out) — populates `out` (rt::SkeletonPkg)
 *   3. Inspect lastError() if read returns false
 *
 * Thread safety: no shared state; each ShotAnimReader instance is
 * independent. The caller may keep multiple readers alive simultaneously.
 */

#pragma once

#include "skeleton/SkeletonTypes.h"

#include <string>
#include <vector>
#include <cstdint>

namespace rt {

class ShotAnimReader {
public:
    ShotAnimReader();
    ~ShotAnimReader();

    /// Read a .shotanim file from disk into `out`. Returns false on any
    /// error (missing file, bad magic, truncated, CRC mismatch). On false,
    /// `out` is left in an unspecified state and lastError() has details.
    bool readFile(const std::string& path, rt::SkeletonPkg& out);

    /// Read a .shotanim blob from memory into `out`.
    bool readBuffer(const uint8_t* data, size_t size, rt::SkeletonPkg& out);

    /// Last error message (empty if no error). Resets on each read().
    [[nodiscard]] const std::string& lastError() const noexcept { return m_lastError; }

    /// File-format version that was read (valid after a successful read).
    [[nodiscard]] uint32_t lastVersion() const noexcept { return m_lastVersion; }

private:
    std::string m_lastError;
    uint32_t    m_lastVersion = 0;
};

} // namespace rt
