/*
 * ShotAnimWriter — converts a loaded spine::SkeletonData + SpineAtlas
 * into a .shotanim binary blob on disk.
 *
 * This is the IMPORT-ONLY side. It is invoked once per character at
 * download time and never again. Its output is the .shotanim file that
 * the NativeSpineEngine consumes at runtime.
 *
 * Compilation: this file requires ROUNDTABLE_HAS_SPINE because it reads
 * spine-cpp data structures. The reader (ShotAnimReader) has no such
 * dependency.
 *
 * Usage:
 *   ShotAnimWriter writer;
 *   if (!writer.write(skelData, atlas, outPath)) {
 *       spdlog::error("convert failed: {}", writer.lastError());
 *   }
 */

#pragma once

#ifdef ROUNDTABLE_HAS_SPINE

#include <string>
#include <vector>
#include <cstdint>

namespace spine {
    class SkeletonData;
}

namespace rt {

class SpineAtlas;

class ShotAnimWriter {
public:
    ShotAnimWriter();
    ~ShotAnimWriter();

    /// Convert and write a .shotanim file.
    /// @param skelData   Loaded spine::SkeletonData
    /// @param atlas      Loaded SpineAtlas (same scale + texture pages)
    /// @param outPath    Path to write to. Directory must exist; file
    ///                   will be overwritten if present.
    /// @return true on success. On false, see lastError().
    bool write(const spine::SkeletonData* skelData,
               const SpineAtlas& atlas,
               const std::string& outPath);

    /// Last error message (empty if no error). Resets on each write().
    [[nodiscard]] const std::string& lastError() const noexcept { return m_lastError; }

    /// Coverage diagnostic — populated after write(). Lets the caller log
    /// any features the writer skipped. Each entry is a short reason
    /// string (e.g. "PathConstraintTimeline skipped"). Empty = full
    /// coverage achieved.
    [[nodiscard]] const std::vector<std::string>& skippedFeatures() const noexcept {
        return m_skipped;
    }

private:
    std::string              m_lastError;
    std::vector<std::string> m_skipped;
};

} // namespace rt

#endif // ROUNDTABLE_HAS_SPINE
