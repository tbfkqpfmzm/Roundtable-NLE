/*
 * ScriptMatcher.cpp — Script parsing and fuzzy matching.
 */

#include "ai/ScriptMatcher.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_set>

namespace rt {

// ═══════════════════════════════════════════════════════════════════════════
//  Script — queries
// ═══════════════════════════════════════════════════════════════════════════

std::vector<const ScriptLine*> Script::linesBySegment(const std::string& segmentName) const
{
    std::vector<const ScriptLine*> result;
    for (const auto& line : lines) {
        if (line.segment == segmentName)
            result.push_back(&line);
    }
    return result;
}

std::string Script::segmentForLine(int lineNumber) const
{
    for (const auto& line : lines) {
        if (line.lineNumber == lineNumber)
            return line.segment;
    }
    return {};
}

// ═══════════════════════════════════════════════════════════════════════════
//  Script — parsers
// ═══════════════════════════════════════════════════════════════════════════

namespace {

/// Title-case a string: first letter uppercase, rest lowercase.
std::string titleCase(const std::string& s)
{
    if (s.empty()) return s;
    std::string result;
    result.reserve(s.size());
    bool nextUpper = true;
    for (char c : s) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            result += c;
            nextUpper = true;
        } else if (nextUpper) {
            result += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            nextUpper = false;
        } else {
            result += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    return result;
}

/// Trim whitespace from both ends.
std::string trim(const std::string& s)
{
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(start, end - start);
}

/// Strip HTML tags from a string, inserting newlines for block elements.
std::string stripHtmlTags(const std::string& html)
{
    // Insert newlines before block-level closing tags
    static const std::regex blockRe("</(?:p|div|li|tr|h[1-6]|br)[^>]*>", std::regex::icase);
    std::string result = std::regex_replace(html, blockRe, "\n");
    // Also replace <br> and <br/> with newlines
    static const std::regex brRe("<br\\s*/?>" , std::regex::icase);
    result = std::regex_replace(result, brRe, "\n");
    // Strip remaining tags
    static const std::regex tagRe("<[^>]*>");
    return std::regex_replace(result, tagRe, "");
}

/// Parse timecode from "[MM:SS]" or "[MM:SS.ms]" at start of dialogue.
/// Returns (timecode_seconds, remainder_text) or (nullopt, original_text).
std::pair<std::optional<double>, std::string> parseTimecode(const std::string& dialogue)
{
    static const std::regex tcRe(R"(^\[(\d+):(\d+(?:\.\d+)?)\]\s*(.+)$)");
    std::smatch m;
    if (std::regex_match(dialogue, m, tcRe)) {
        double mins = std::stod(m[1].str());
        double secs = std::stod(m[2].str());
        return {mins * 60.0 + secs, m[3].str()};
    }
    return {std::nullopt, dialogue};
}

/// Encode a Unicode codepoint as UTF-8 bytes appended to `out`.
void encodeUtf8(int codepoint, std::string& out)
{
    if (codepoint < 0x80) {
        out += static_cast<char>(codepoint);
    } else if (codepoint < 0x800) {
        out += static_cast<char>(0xC0 | (codepoint >> 6));
        out += static_cast<char>(0x80 | (codepoint & 0x3F));
    } else if (codepoint < 0x10000) {
        out += static_cast<char>(0xE0 | (codepoint >> 12));
        out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (codepoint & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (codepoint >> 18));
        out += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (codepoint & 0x3F));
    }
}

/// Try to match dialogue patterns. Returns (character, dialogue) or empty strings.
std::pair<std::string, std::string> matchDialogue(const std::string& line)
{
    // Pattern 1: CHARACTER: dialogue
    static const std::regex p1(R"(^([A-Z][A-Z0-9_\s]+)\s*:\s*(.+)$)");
    // Pattern 2: [Character] dialogue
    static const std::regex p2(R"(^\[([A-Za-z0-9_\s]+)\]\s*(.+)$)");

    std::smatch m;
    if (std::regex_match(line, m, p1)) {
        return {trim(m[1].str()), trim(m[2].str())};
    }
    if (std::regex_match(line, m, p2)) {
        return {trim(m[1].str()), trim(m[2].str())};
    }
    return {"", ""};
}

void finalizeScript(Script& script)
{
    // Add UNTITLED to segments if used
    bool hasUntitled = false;
    for (const auto& line : script.lines) {
        if (line.segment == "UNTITLED") {
            hasUntitled = true;
            break;
        }
    }
    if (hasUntitled) {
        auto it = std::find(script.segments.begin(), script.segments.end(), "UNTITLED");
        if (it == script.segments.end())
            script.segments.insert(script.segments.begin(), "UNTITLED");
    }

    // Build sorted unique character list
    std::set<std::string> chars;
    for (const auto& line : script.lines)
        chars.insert(line.character);
    script.characters.assign(chars.begin(), chars.end());
}

} // namespace

Script Script::fromText(const std::string& text)
{
    Script script;
    std::string currentSegment = "UNTITLED";
    int lineNum = 0;
    std::unordered_set<std::string> seenSegments;

    // Segment marker pattern: **SEGMENT NAME**
    static const std::regex segmentRe(R"(^\*\*([^*]+)\*\*\s*$)");

    std::istringstream stream(text);
    std::string line;

    while (std::getline(stream, line)) {
        ++lineNum;
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;

        // Check for segment marker
        std::smatch sm;
        if (std::regex_match(line, sm, segmentRe)) {
            currentSegment = trim(sm[1].str());
            if (seenSegments.insert(currentSegment).second) {
                script.segments.push_back(currentSegment);
            }
            spdlog::trace("ScriptMatcher: Found segment marker: '{}'", currentSegment);
            continue;
        }

        // Try dialogue patterns
        auto [charName, dialogue] = matchDialogue(line);
        if (!charName.empty() && !dialogue.empty()) {
            charName = titleCase(charName);
            auto [timecode, cleanDialogue] = parseTimecode(dialogue);

            ScriptLine sl;
            sl.character   = charName;
            sl.dialogue    = cleanDialogue;
            sl.lineNumber  = lineNum;
            sl.segment     = currentSegment;
            sl.timecode    = timecode;
            script.lines.push_back(std::move(sl));
        }
    }

    finalizeScript(script);
    spdlog::debug("ScriptMatcher: Parsed {} lines, {} segments from text",
                  script.lines.size(), script.segments.size());
    return script;
}

Script Script::fromHtml(const std::string& html)
{
    // Simple HTML parser: strip tags, then parse as text.
    // Google Docs bold detection is complex — for now we strip and use text parser.
    // A more sophisticated parser could detect <b>/<strong>/CSS font-weight:700.
    std::string plainText = stripHtmlTags(html);

    // Decode numeric HTML character references: &#NNNN;
    {
        static const std::regex numericRef(R"(&#(\d+);)");
        std::string out;
        std::sregex_iterator it(plainText.begin(), plainText.end(), numericRef);
        std::sregex_iterator end;
        size_t lastPos = 0;
        for (; it != end; ++it) {
            out.append(plainText, lastPos, static_cast<size_t>(it->position()) - lastPos);
            encodeUtf8(std::stoi((*it)[1].str()), out);
            lastPos = static_cast<size_t>(it->position() + it->length());
        }
        out.append(plainText, lastPos, plainText.size() - lastPos);
        plainText = std::move(out);
    }

    // Decode hex character references: &#xHHHH;
    {
        static const std::regex hexRef(R"(&#x([0-9a-fA-F]+);)");
        std::string out;
        std::sregex_iterator it(plainText.begin(), plainText.end(), hexRef);
        std::sregex_iterator end;
        size_t lastPos = 0;
        for (; it != end; ++it) {
            out.append(plainText, lastPos, static_cast<size_t>(it->position()) - lastPos);
            encodeUtf8(std::stoi((*it)[1].str(), nullptr, 16), out);
            lastPos = static_cast<size_t>(it->position() + it->length());
        }
        out.append(plainText, lastPos, plainText.size() - lastPos);
        plainText = std::move(out);
    }

    // Decode named HTML entities using literal string replacement (no regex overhead).
    // Order matters: decode & last since other entities contain '&'
    auto replaceAll = [](std::string& s, const std::string& what, const std::string& with) {
        size_t pos = 0;
        while ((pos = s.find(what, pos)) != std::string::npos) {
            s.replace(pos, what.size(), with);
            pos += with.size();
        }
    };

    replaceAll(plainText, "&nbsp;",  " ");
    replaceAll(plainText, "&rsquo;", "\xe2\x80\x99");
    replaceAll(plainText, "&lsquo;", "\xe2\x80\x98");
    replaceAll(plainText, "&rdquo;", "\xe2\x80\x9d");
    replaceAll(plainText, "&ldquo;", "\xe2\x80\x9c");
    replaceAll(plainText, "&mdash;", "\xe2\x80\x94");
    replaceAll(plainText, "&ndash;", "\xe2\x80\x93");
    replaceAll(plainText, "&hellip;","\xe2\x80\xa6");
    replaceAll(plainText, "&bull;",  "\xe2\x80\xa2");
    replaceAll(plainText, "&trade;", "\xe2\x84\xa2");
    replaceAll(plainText, "&copy;",  "\xc2\xa9");
    replaceAll(plainText, "&reg;",   "\xc2\xae");
    replaceAll(plainText, "&deg;",   "\xc2\xb0");
    replaceAll(plainText, "&times;", "\xc3\x97");
    replaceAll(plainText, "&divide;","\xc3\xb7");
    {
      std::string amp(1, '&');
      replaceAll(plainText, amp + "apos;", "'");
      replaceAll(plainText, amp + "quot;",  "\"");
      replaceAll(plainText, amp + "lt;",    "<");
      replaceAll(plainText, amp + "gt;",    ">");
      replaceAll(plainText, amp + "amp;",   "&");  // Must be last!
    }

    return fromText(plainText);
}

Script Script::fromJson(const std::string& jsonStr)
{
    // Simple JSON parser for our script format.
    // Expected: {"lines": [{"character": "X", "dialogue": "Y", ...}], "segments": [...]}
    // For robustness we use a simple state-machine approach.
    Script script;

    // Find "lines" array and parse entries
    // This is intentionally simplified — production code would use nlohmann/json
    auto extractString = [](const std::string& s, const std::string& key) -> std::string {
        std::string search = "\"" + key + "\"";
        auto pos = s.find(search);
        if (pos == std::string::npos) return "";
        pos = s.find(':', pos + search.size());
        if (pos == std::string::npos) return "";
        pos = s.find('"', pos + 1);
        if (pos == std::string::npos) return "";
        auto end = s.find('"', pos + 1);
        if (end == std::string::npos) return "";
        return s.substr(pos + 1, end - pos - 1);
    };

    // Find each {...} object within the "lines" array
    auto linesStart = jsonStr.find("\"lines\"");
    if (linesStart == std::string::npos) {
        spdlog::warn("ScriptMatcher: No 'lines' key found in JSON");
        return script;
    }

    auto arrStart = jsonStr.find('[', linesStart);
    if (arrStart == std::string::npos) return script;

    // Find matching ]
    int depth = 0;
    size_t arrEnd = arrStart;
    for (size_t i = arrStart; i < jsonStr.size(); ++i) {
        if (jsonStr[i] == '[') ++depth;
        if (jsonStr[i] == ']') { --depth; if (depth == 0) { arrEnd = i; break; } }
    }

    // Parse each object
    int lineNum = 0;
    size_t pos = arrStart;
    std::unordered_set<std::string> seenSegments;
    while (pos < arrEnd) {
        auto objStart = jsonStr.find('{', pos);
        if (objStart == std::string::npos || objStart > arrEnd) break;
        auto objEnd = jsonStr.find('}', objStart);
        if (objEnd == std::string::npos) break;

        std::string obj = jsonStr.substr(objStart, objEnd - objStart + 1);
        ++lineNum;

        std::string character = titleCase(trim(extractString(obj, "character")));
        std::string dialogue  = trim(extractString(obj, "dialogue"));
        std::string segment   = extractString(obj, "segment");
        if (segment.empty()) segment = "UNTITLED";

        if (!character.empty() && !dialogue.empty()) {
            ScriptLine sl;
            sl.character  = character;
            sl.dialogue   = dialogue;
            sl.lineNumber = lineNum;
            sl.segment    = segment;

            // Optional timecode
            std::string tcStr = extractString(obj, "timecode");
            if (!tcStr.empty()) {
                try { sl.timecode = std::stod(tcStr); } catch (...) {}
            }

            script.lines.push_back(std::move(sl));

            if (seenSegments.insert(segment).second) {
                script.segments.push_back(segment);
            }
        }

        pos = objEnd + 1;
    }

    finalizeScript(script);
    spdlog::debug("ScriptMatcher: Parsed {} lines from JSON", script.lines.size());
    return script;
}

Script Script::load(const std::string& pathOrContent)
{
    // Auto-detect format
    std::string content = pathOrContent;

    // If it's a file path, read it
    if (content.size() < 500 && (content.find('\n') == std::string::npos)) {
        // Might be a path
        std::ifstream f(content);
        if (f.is_open()) {
            std::ostringstream ss;
            ss << f.rdbuf();
            content = ss.str();

            // JSON detection
            if (pathOrContent.size() > 5 &&
                pathOrContent.substr(pathOrContent.size() - 5) == ".json")
            {
                return fromJson(content);
            }
        }
    }

    // Content-based detection
    std::string trimmed = trim(content);
    if (trimmed.size() > 0 && (trimmed[0] == '{' || trimmed[0] == '[')) {
        return fromJson(content);
    }
    if (content.find("<html") != std::string::npos ||
        content.find("<body") != std::string::npos ||
        content.find("<HTML") != std::string::npos)
    {
        return fromHtml(content);
    }
    return fromText(content);
}

// ═══════════════════════════════════════════════════════════════════════════
//  ScriptMatcher — text utilities
// ═══════════════════════════════════════════════════════════════════════════

std::string ScriptMatcher::normalize(const std::string& text)
{
    std::string result;
    result.reserve(text.size());

    bool lastWasSpace = true;
    for (char c : text) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) || std::isspace(uc)) {
            if (std::isspace(uc)) {
                if (!lastWasSpace) {
                    result += ' ';
                    lastWasSpace = true;
                }
            } else {
                result += static_cast<char>(std::tolower(uc));
                lastWasSpace = false;
            }
        }
        // Punctuation is skipped
    }

    // Trim trailing space
    while (!result.empty() && result.back() == ' ')
        result.pop_back();

    return result;
}

float ScriptMatcher::sequenceRatio(const std::string& a, const std::string& b)
{
    // Ratcliff/Obershelp algorithm — equivalent to Python's
    // difflib.SequenceMatcher.ratio():  2.0 * M / T
    // where M = total matched characters from longest common contiguous
    // substrings found recursively, T = total length of both strings.
    if (a.empty() && b.empty()) return 1.0f;
    if (a.empty() || b.empty()) return 0.0f;

    // Recursive helper: find longest common contiguous substring,
    // then recurse on the left and right remainders.
    struct Helper {
        static size_t matchingChars(const std::string& s1, size_t lo1, size_t hi1,
                                    const std::string& s2, size_t lo2, size_t hi2)
        {
            if (lo1 >= hi1 || lo2 >= hi2) return 0;

            // Find longest common contiguous substring in s1[lo1..hi1) and s2[lo2..hi2)
            size_t bestLen = 0, bestI = lo1, bestJ = lo2;
            for (size_t i = lo1; i < hi1; ++i) {
                for (size_t j = lo2; j < hi2; ++j) {
                    size_t k = 0;
                    while (i + k < hi1 && j + k < hi2 && s1[i + k] == s2[j + k])
                        ++k;
                    if (k > bestLen) {
                        bestLen = k;
                        bestI = i;
                        bestJ = j;
                    }
                }
            }
            if (bestLen == 0) return 0;

            // Recurse on left and right of the match
            size_t leftMatch  = matchingChars(s1, lo1, bestI, s2, lo2, bestJ);
            size_t rightMatch = matchingChars(s1, bestI + bestLen, hi1,
                                              s2, bestJ + bestLen, hi2);
            return leftMatch + bestLen + rightMatch;
        }
    };

    size_t matched = Helper::matchingChars(a, 0, a.size(), b, 0, b.size());
    return static_cast<float>(2.0 * matched) / static_cast<float>(a.size() + b.size());
}

std::string ScriptMatcher::soundex(const std::string& word)
{
    if (word.empty()) return "0000";

    std::string result;
    result += static_cast<char>(std::toupper(static_cast<unsigned char>(word[0])));

    // Soundex coding table
    auto code = [](char c) -> char {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (c == 'b' || c == 'f' || c == 'p' || c == 'v') return '1';
        if (c == 'c' || c == 'g' || c == 'j' || c == 'k' || c == 'q' ||
            c == 's' || c == 'x' || c == 'z') return '2';
        if (c == 'd' || c == 't') return '3';
        if (c == 'l') return '4';
        if (c == 'm' || c == 'n') return '5';
        if (c == 'r') return '6';
        return '0'; // a, e, i, o, u, h, w, y
    };

    char lastCode = code(word[0]);
    for (size_t i = 1; i < word.size() && result.size() < 4; ++i) {
        char c = code(word[i]);
        if (c != '0' && c != lastCode) {
            result += c;
        }
        lastCode = c;
    }

    while (result.size() < 4) result += '0';
    return result;
}

std::string ScriptMatcher::metaphone(const std::string& word)
{
    // Simplified Double Metaphone — primary code only.
    if (word.empty()) return "";

    std::string w;
    for (char c : word)
        w += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    std::string result;
    size_t len = w.size();

    // Skip initial silent consonants
    if (len >= 2) {
        std::string first2 = w.substr(0, 2);
        if (first2 == "GN" || first2 == "KN" || first2 == "PN" ||
            first2 == "AE" || first2 == "WR")
        {
            w = w.substr(1);
            len = w.size();
        }
    }

    for (size_t i = 0; i < len && result.size() < 6; ++i) {
        char c = w[i];
        // Skip duplicates (except CC)
        if (i > 0 && c == w[i - 1] && c != 'C') continue;

        switch (c) {
        case 'A': case 'E': case 'I': case 'O': case 'U':
            if (i == 0) result += c;
            break;
        case 'B':
            if (!(i == len - 1 && i > 0 && w[i - 1] == 'M'))
                result += 'B';
            break;
        case 'C':
            if (i + 1 < len && (w[i + 1] == 'E' || w[i + 1] == 'I' || w[i + 1] == 'Y'))
                result += 'S';
            else
                result += 'K';
            break;
        case 'D':
            if (i + 1 < len && w[i + 1] == 'G')
                result += 'J';
            else
                result += 'T';
            break;
        case 'F': result += 'F'; break;
        case 'G':
            if (i + 1 < len && (w[i + 1] == 'H' || w[i + 1] == 'N'))
                ; // silent
            else
                result += 'K';
            break;
        case 'H':
            if (i + 1 < len && std::string("AEIOU").find(w[i + 1]) != std::string::npos)
                result += 'H';
            break;
        case 'J': result += 'J'; break;
        case 'K':
            if (i == 0 || (i > 0 && w[i - 1] != 'C'))
                result += 'K';
            break;
        case 'L': result += 'L'; break;
        case 'M': result += 'M'; break;
        case 'N': result += 'N'; break;
        case 'P':
            if (i + 1 < len && w[i + 1] == 'H')
                result += 'F';
            else
                result += 'P';
            break;
        case 'Q': result += 'K'; break;
        case 'R': result += 'R'; break;
        case 'S':
            if (i + 1 < len && w[i + 1] == 'H')
                result += 'X';
            else
                result += 'S';
            break;
        case 'T':
            if (i + 1 < len && w[i + 1] == 'H')
                result += '0'; // theta
            else
                result += 'T';
            break;
        case 'V': result += 'F'; break;
        case 'W': case 'Y':
            if (i + 1 < len && std::string("AEIOU").find(w[i + 1]) != std::string::npos)
                result += c;
            break;
        case 'X': result += "KS"; break;
        case 'Z': result += 'S'; break;
        default: break;
        }
    }

    return result;
}

// ═══════════════════════════════════════════════════════════════════════════
//  ScriptMatcher — matching
// ═══════════════════════════════════════════════════════════════════════════

ScriptMatcher::ScriptMatcher(const Script& script)
    : m_script(script)
{
}

std::pair<const ScriptLine*, float>
ScriptMatcher::matchSegment(const std::string& segmentText) const
{
    const ScriptLine* bestMatch = nullptr;
    float bestScore = 0.0f;

    std::string segClean = normalize(segmentText);

    for (const auto& line : m_script.lines) {
        std::string dialogueClean = normalize(line.dialogue);
        float score = sequenceRatio(segClean, dialogueClean);

        if (score > bestScore) {
            bestScore = score;
            bestMatch = &line;
        }
    }

    if (bestScore >= m_threshold)
        return {bestMatch, bestScore};
    return {nullptr, 0.0f};
}

std::vector<MatchResult>
ScriptMatcher::matchAllSegments(const std::vector<SegmentInput>& segments,
                                bool allowRetakes) const
{
    std::vector<MatchResult> results;
    results.reserve(segments.size());

    std::unordered_set<int> usedLines; // indices into m_script.lines
    int lastMatchedIdx = -1;

    for (size_t i = 0; i < segments.size(); ++i) {
        const auto& seg = segments[i];
        std::string segClean = normalize(seg.text);

        float bestScore = 0.0f;
        int bestLineIdx = -1;

        for (size_t j = 0; j < m_script.lines.size(); ++j) {
            // Skip used lines unless retakes allowed
            if (!allowRetakes && usedLines.count(static_cast<int>(j)))
                continue;

            std::string dialogueClean = normalize(m_script.lines[j].dialogue);
            float score = sequenceRatio(segClean, dialogueClean);

            // Sequential bias
            if (lastMatchedIdx != -1) {
                if (static_cast<int>(j) == lastMatchedIdx + 1)
                    score += 0.2f; // Strong bias for next line
                else if (static_cast<int>(j) > lastMatchedIdx)
                    score += 0.05f; // Slight bias for future lines
            }

            // Timecode bias
            if (m_script.lines[j].timecode.has_value()) {
                double timeDiff = std::abs(m_script.lines[j].timecode.value() - seg.start);
                if (timeDiff < 5.0) {
                    score += static_cast<float>(0.1 * (1.0 - timeDiff / 5.0));
                }
            }

            if (score > bestScore) {
                bestScore = score;
                bestLineIdx = static_cast<int>(j);
            }
        }

        float effectiveThreshold = allowRetakes ? 0.35f : m_threshold;

        MatchResult mr;
        mr.segmentIndex = static_cast<int>(i);

        if (bestScore >= effectiveThreshold && bestLineIdx >= 0) {
            const auto& line = m_script.lines[static_cast<size_t>(bestLineIdx)];
            mr.character        = line.character;
            mr.confidence       = bestScore;
            mr.scriptLineNumber = line.lineNumber;
            mr.scriptSegment    = line.segment;
            mr.state            = MatchState::Tentative;

            usedLines.insert(bestLineIdx);
            lastMatchedIdx = bestLineIdx;

            spdlog::trace("ScriptMatcher: Segment {} -> {} [{}] (Idx {}, {:.0f}%)",
                          i, line.character, line.segment, bestLineIdx, bestScore * 100);
        } else {
            mr.confidence = 0.0f;
            mr.state      = MatchState::Unmatched;
            spdlog::trace("ScriptMatcher: Segment {} -> No match", i);
        }

        results.push_back(std::move(mr));
    }

    return results;
}

const std::vector<std::string>& ScriptMatcher::characters() const
{
    return m_script.characters;
}

} // namespace rt
