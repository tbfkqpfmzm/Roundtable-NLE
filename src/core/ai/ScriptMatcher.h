/*
 * ScriptMatcher — Script parsing and fuzzy matching for audio sync.
 *
 * Parses scripts in multiple formats (plain text, HTML/Google Docs, JSON).
 * Matches transcribed segments to script lines using sequence matching
 * with sequential bias and timecode awareness.
 *
 * Ported from Python _OLD_/src/audio/script_matcher.py.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace rt {

// ─── Script data types ──────────────────────────────────────────────────────

/// A single line of dialogue from the script.
struct ScriptLine
{
    std::string            character;
    std::string            dialogue;
    int                    lineNumber{0};
    std::string            segment{"UNTITLED"}; // Segment/section this line belongs to
    std::optional<double>  timecode;             // Optional manual timecode (seconds)
};

/// Parsed script containing dialogue lines and metadata.
struct Script
{
    std::vector<ScriptLine>  lines;
    std::vector<std::string> characters;  // Sorted unique character names
    std::vector<std::string> segments;    // Ordered segment names

    // ── Queries ─────────────────────────────────────────────────────────

    /// Get lines belonging to a specific segment.
    [[nodiscard]] std::vector<const ScriptLine*>
    linesBySegment(const std::string& segmentName) const;

    /// Get segment name for a given line number. Empty if not found.
    [[nodiscard]] std::string segmentForLine(int lineNumber) const;

    /// Number of lines.
    [[nodiscard]] size_t lineCount() const noexcept { return lines.size(); }

    /// True if script has no lines.
    [[nodiscard]] bool isEmpty() const noexcept { return lines.empty(); }

    // ── Parsers ─────────────────────────────────────────────────────────

    /// Parse from plain text format.
    /// Format: "CHARACTER: Dialogue text" or "[Character] Dialogue text"
    /// Segment markers: "**SEGMENT NAME**"
    static Script fromText(const std::string& text);

    /// Parse from HTML (Google Docs export). Bold text = segment markers.
    static Script fromHtml(const std::string& html);

    /// Parse from JSON format.
    static Script fromJson(const std::string& jsonStr);

    /// Auto-detect format and parse.
    static Script load(const std::string& pathOrContent);
};

// ─── Match result ───────────────────────────────────────────────────────────

enum class MatchState : int
{
    Unmatched  = 0,
    Tentative  = 1,
    Confirmed  = 2
};

/// Result of matching one transcribed segment to a script line.
struct MatchResult
{
    int                    segmentIndex{-1};   // Index in transcription segments
    std::string            character;           // Matched character name (empty if unmatched)
    float                  confidence{0.0f};    // Match confidence 0-1
    int                    scriptLineNumber{-1};// Matched script line number
    std::string            scriptSegment;       // Script segment name
    MatchState             state{MatchState::Unmatched};
};

// ─── ScriptMatcher ──────────────────────────────────────────────────────────

class ScriptMatcher
{
public:
    explicit ScriptMatcher(const Script& script);
    ~ScriptMatcher() = default;

    // ── Single match ────────────────────────────────────────────────────

    /// Find the best matching script line for a segment of text.
    /// \return (matched line pointer, confidence). nullptr if no good match.
    [[nodiscard]] std::pair<const ScriptLine*, float>
    matchSegment(const std::string& segmentText) const;

    // ── Batch match ─────────────────────────────────────────────────────

    /// Input for batch matching: (text, start_time, end_time)
    struct SegmentInput
    {
        std::string text;
        double      start{0.0};
        double      end{0.0};
    };

    /// Match all transcribed segments to script lines.
    /// \param segments     List of (text, start, end) from transcription.
    /// \param allowRetakes If true, script lines can match multiple segments.
    [[nodiscard]] std::vector<MatchResult>
    matchAllSegments(const std::vector<SegmentInput>& segments,
                     bool allowRetakes = false) const;

    // ── Configuration ───────────────────────────────────────────────────

    /// Set match threshold (0-1). Default: 0.4
    void setThreshold(float threshold) noexcept { m_threshold = threshold; }
    [[nodiscard]] float threshold() const noexcept { return m_threshold; }

    /// Get characters from the script.
    [[nodiscard]] const std::vector<std::string>& characters() const;

    /// Get the script.
    [[nodiscard]] const Script& script() const noexcept { return m_script; }

    // ── Text utilities (public for testing) ──────────────────────────────

    /// Normalize text for comparison (lowercase, strip punctuation, collapse whitespace).
    [[nodiscard]] static std::string normalize(const std::string& text);

    /// Calculate sequence similarity ratio (0-1) between two strings.
    [[nodiscard]] static float sequenceRatio(const std::string& a, const std::string& b);

    /// Soundex phonetic code.
    [[nodiscard]] static std::string soundex(const std::string& word);

    /// Double Metaphone phonetic code.
    [[nodiscard]] static std::string metaphone(const std::string& word);

private:
    Script m_script;
    float  m_threshold{0.4f};
};

} // namespace rt
