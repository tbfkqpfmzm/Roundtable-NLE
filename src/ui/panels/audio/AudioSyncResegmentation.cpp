/*
 * AudioSyncResegmentation.cpp - Script-guided Audio Sync resegmentation.
 * Split from AudioSyncData.cpp for maintainability.
 */
#include "panels/audio/AudioSync.h"
#include "ai/ScriptMatcher.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <sstream>
#include <unordered_map>

namespace rt {

void AudioSync::resegmentByScript()
{
    // ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬
    // Script-guided re-segmentation: split clips whose transcript spans
    // multiple script lines.  Whisper often produces one long segment that
    // actually covers 2-3 script lines ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â this step re-cuts those clips so
    // each new clip aligns with a single script line.
    //
    // The algorithm iterates up to 3 rounds so that a segment spanning 3+
    // lines is progressively split (first into 2, then the longer half
    // may split again).
    // ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬
    if (m_clips.empty() || !m_script || m_script->lines.empty()) return;

    auto normalizeChar = [](const std::string& name) -> std::string {
        std::string lower;
        for (char c : name)
            lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        while (!lower.empty() && lower.back() == ' ') lower.pop_back();
        while (!lower.empty() && lower.front() == ' ') lower.erase(lower.begin());
        if (lower == "marian") return "modernia";
        return lower;
    };

    auto norm = [](const std::string& text) {
        return ScriptMatcher::normalize(text);
    };

    int totalSplits = 0;

    // Iterate up to 3 rounds to handle multi-line segments
    for (int round = 0; round < 3; ++round) {
        // Group clips by character
        std::unordered_map<std::string, std::vector<size_t>> clipsByChar;
        for (size_t i = 0; i < m_clips.size(); ++i)
            clipsByChar[normalizeChar(m_clips[i].character)].push_back(i);

        // Group script lines by character
        std::unordered_map<std::string, std::vector<size_t>> linesByChar;
        for (size_t i = 0; i < m_script->lines.size(); ++i)
            linesByChar[normalizeChar(m_script->lines[i].character)].push_back(i);

        // Collect splits to apply
        struct SplitOp {
            size_t clipIdx;     // index in m_clips
            size_t splitWord;   // split after this many words
        };
        std::vector<SplitOp> splits;

        for (auto& [charKey, clipIndices] : clipsByChar) {
            auto lineIt = linesByChar.find(charKey);
            if (lineIt == linesByChar.end() || lineIt->second.empty()) continue;
            const auto& lineIndices = lineIt->second;

            // Pre-normalize script lines
            std::vector<std::string> normLines;
            normLines.reserve(lineIndices.size());
            for (size_t li : lineIndices)
                normLines.push_back(norm(m_script->lines[li].dialogue));

            for (size_t ci : clipIndices) {
                const auto& clip = m_clips[ci];
                if (clip.matchState == 2) continue; // confirmed ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â don't touch

                std::string clipText = norm(
                    clip.editedText.empty() ? clip.transcript : clip.editedText);

                // Split clip text into words
                std::vector<std::string> words;
                { std::istringstream ss(clipText); std::string w; while (ss >> w) words.push_back(w); }

                if (words.size() < 4) continue; // too short to split meaningfully

                // Best single-line match score
                float bestSingleScore = 0.0f;
                for (const auto& lt : normLines) {
                    float s = ScriptMatcher::sequenceRatio(clipText, lt);
                    bestSingleScore = std::max(bestSingleScore, s);
                }
                if (bestSingleScore >= 0.85f) continue; // already matches well

                // Try every split point: find the split that produces two halves
                // where the LONGER half matches a script line better than the
                // whole clip does.  We weight each half's score by its proportion
                // of the total word count so short garbage prefixes/suffixes
                // don't dominate.
                float bestSplitScore = -1.0f;
                size_t bestSplitPoint = 0;

                for (size_t sp = 2; sp <= words.size() - 2; ++sp) {
                    // Build both halves
                    std::string half1, half2;
                    for (size_t w = 0; w < sp; ++w) {
                        if (!half1.empty()) half1 += ' ';
                        half1 += words[w];
                    }
                    for (size_t w = sp; w < words.size(); ++w) {
                        if (!half2.empty()) half2 += ' ';
                        half2 += words[w];
                    }

                    // Find best matching script line for each half
                    float bestH1 = 0.0f, bestH2 = 0.0f;
                    size_t bestL1 = SIZE_MAX, bestL2 = SIZE_MAX;
                    for (size_t li = 0; li < normLines.size(); ++li) {
                        float s1 = ScriptMatcher::sequenceRatio(half1, normLines[li]);
                        float s2 = ScriptMatcher::sequenceRatio(half2, normLines[li]);
                        if (s1 > bestH1) { bestH1 = s1; bestL1 = li; }
                        if (s2 > bestH2) { bestH2 = s2; bestL2 = li; }
                    }

                    // At least the better half must match well (> 0.50)
                    float bestHalf = std::max(bestH1, bestH2);
                    if (bestHalf < 0.50f) continue;

                    // Weight-averaged score: each half weighted by word proportion
                    float frac1 = static_cast<float>(sp) / static_cast<float>(words.size());
                    float weighted = bestH1 * frac1 + bestH2 * (1.0f - frac1);

                    // Accept if weighted score beats single AND the better half
                    // improves over the single-clip match
                    if (weighted > bestSingleScore && bestHalf > bestSingleScore + 0.02f
                        && weighted > bestSplitScore) {
                        bestSplitScore = weighted;
                        bestSplitPoint = sp;
                    }
                }

                if (bestSplitPoint > 0) {
                    splits.push_back({ci, bestSplitPoint});
                    spdlog::debug("AudioSync: Splitting clip {} at word {} "
                                  "(single={:.2f} split={:.2f})",
                                  ci, bestSplitPoint, bestSingleScore, bestSplitScore);
                }
            }
        }

        if (splits.empty()) break; // no more splits needed

        // Apply splits in reverse order so indices stay valid
        std::sort(splits.begin(), splits.end(),
                  [](const SplitOp& a, const SplitOp& b) {
                      return a.clipIdx > b.clipIdx;
                  });

        for (const auto& op : splits) {
            auto& clip = m_clips[op.clipIdx];
            std::string text = clip.editedText.empty() ? clip.transcript : clip.editedText;

            // Split the text by words
            std::vector<std::string> words;
            { std::istringstream ss(ScriptMatcher::normalize(text)); std::string w; while (ss >> w) words.push_back(w); }

            // Build first half text (use original-ish words from normalized)
            std::string h1, h2;
            for (size_t w = 0; w < op.splitWord; ++w) {
                if (!h1.empty()) h1 += ' ';
                h1 += words[w];
            }
            for (size_t w = op.splitWord; w < words.size(); ++w) {
                if (!h2.empty()) h2 += ' ';
                h2 += words[w];
            }

            // Estimate split time proportionally, then refine to nearest silence
            double totalWords = static_cast<double>(words.size());
            double splitFrac = static_cast<double>(op.splitWord) / totalWords;
            double splitTimeEst = clip.start + (clip.end - clip.start) * splitFrac;
            double splitTime = splitTimeEst;

            // Refine: search ±0.5s around the estimate for the quietest
            // 20ms window in the audio — that's the natural breath/pause.
            auto sampIt = m_audioSamples.find(clip.sourceFile);
            if (sampIt != m_audioSamples.end()) {
                const auto& ad = sampIt->second;
                int sr = static_cast<int>(ad.sampleRate);
                int winSz = static_cast<int>(0.020 * sr);
                int center = static_cast<int>(splitTimeEst * sr);
                int searchRadius = static_cast<int>(0.5 * sr);
                int lo = std::max(static_cast<int>(clip.start * sr) + winSz, center - searchRadius);
                int hi = std::min(static_cast<int>(clip.end * sr) - winSz, center + searchRadius);
                float bestRms = 1e9f;
                int bestPos = center;
                for (int pos = lo; pos + winSz <= hi; pos += winSz / 2) {
                    float sumSq = 0.0f;
                    for (int j = 0; j < winSz && static_cast<size_t>(pos + j) < ad.samples.size(); ++j) {
                        float s = ad.samples[static_cast<size_t>(pos + j)];
                        sumSq += s * s;
                    }
                    float rms = std::sqrt(sumSq / static_cast<float>(winSz));
                    if (rms < bestRms) { bestRms = rms; bestPos = pos; }
                }
                splitTime = static_cast<double>(bestPos) / sr;
                if (splitTime < clip.start + 0.1) splitTime = clip.start + 0.1;
                if (splitTime > clip.end - 0.1) splitTime = clip.end - 0.1;
            }

            // Create second clip (insert after current)
            SyncClip clip2;
            clip2.id         = 0; // will renumber later
            clip2.sourceFile = clip.sourceFile;
            clip2.character  = clip.character;
            clip2.start      = splitTime;
            clip2.end        = clip.end;
            clip2.transcript = h2;
            clip2.editedText = h2;
            clip2.matchState = 0;
            clip2.confidence = 0.0f;
            clip2.scriptLineNumber = -1;

            // Trim current clip
            clip.end        = splitTime;
            clip.transcript = h1;
            clip.editedText = h1;

            // Insert clip2 after current clip
            m_clips.insert(m_clips.begin() + static_cast<int>(op.clipIdx) + 1, clip2);
            ++totalSplits;
        }

        // Re-number IDs
        for (size_t i = 0; i < m_clips.size(); ++i)
            m_clips[i].id = static_cast<int>(i);
    }

    if (totalSplits > 0)
        spdlog::info("AudioSync: Re-segmentation split {} clips (now {} clips)",
                     totalSplits, m_clips.size());
}

} // namespace rt
