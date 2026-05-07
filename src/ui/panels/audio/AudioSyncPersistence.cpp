/*
 * AudioSyncPersistence.cpp - Split/merge and project state persistence for AudioSync.
 * Split from AudioSyncData.cpp for maintainability.
 *
 * Contains: splitClip, mergeClipWithNext, saveProjectState, restoreProjectState.
 */

#include "panels/audio/AudioSync.h"

#include <spdlog/spdlog.h>

#include <QFile>
#include <QSettings>

#include "project/ClipSerialization.h"

namespace rt {
// ÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚Â
//  Split & merge
// ÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚Â

void AudioSync::splitClip(size_t clipIdx)
{
    if (clipIdx >= m_clips.size()) return;
    // Default: split at midpoint
    auto& clip = m_clips[clipIdx];
    double mid = (clip.start + clip.end) / 2.0;
    SyncClip newClip = clip;
    newClip.start = mid;
    newClip.id = static_cast<int>(m_clips.size());
    clip.end = mid;
    m_clips.insert(m_clips.begin() + static_cast<ptrdiff_t>(clipIdx) + 1, newClip);
    populateClipList();
}

void AudioSync::mergeClipWithNext(size_t clipIdx)
{
    if (clipIdx + 1 >= m_clips.size()) return;
    auto& clip = m_clips[clipIdx];
    const auto& next = m_clips[clipIdx + 1];
    clip.end = next.end;
    if (!next.editedText.empty()) {
        if (!clip.editedText.empty())
            clip.editedText += " ";
        clip.editedText += next.editedText;
    }
    if (!next.transcript.empty()) {
        if (!clip.transcript.empty())
            clip.transcript += " ";
        clip.transcript += next.transcript;
    }
    m_clips.erase(m_clips.begin() + static_cast<ptrdiff_t>(clipIdx) + 1);
    populateClipList();
}

// ÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚Â
//  Project state persistence
// ÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚Â

void AudioSync::saveProjectState(const QString& projectName)
{
    if (projectName.isEmpty()) return;

    QSettings settings("ROUNDTABLE", "NLE");
    QString prefix = "Project/" + projectName + "/AudioSync/";

    // Save script source and display name
    settings.setValue(prefix + "scriptSource", m_lastScriptSource);
    QString displayName;
    auto it = m_scriptSessions.find(m_activeScriptKey);
    if (it != m_scriptSessions.end())
        displayName = QString::fromStdString(it->second.displayName);
    settings.setValue(prefix + "scriptDisplayName", displayName);
    // Save raw script content so we can restore without re-fetching from URL
    settings.setValue(prefix + "scriptRawContent", QString::fromStdString(m_scriptRawContent));

    // Save audio paths
    QStringList audioPaths;
    for (const auto& p : m_audioPaths)
        audioPaths.append(QString::fromStdString(p));
    settings.setValue(prefix + "audioPaths", audioPaths);

    // Save workflow state flags
    settings.setValue(prefix + "scriptLoaded",      m_scriptLoaded);
    settings.setValue(prefix + "audioImported",      m_audioImported);
    settings.setValue(prefix + "transcriptionDone",  m_transcriptionDone);
    settings.setValue(prefix + "syncDone",           m_syncDone);

    // ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ Save all clips (full serialization) ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬
    settings.setValue(prefix + "clipCount", static_cast<int>(m_clips.size()));
    for (size_t i = 0; i < m_clips.size(); ++i) {
        const auto& c = m_clips[i];
        QString cp = prefix + "clip/" + QString::number(i) + "/";
        settings.setValue(cp + "id",              c.id);
        settings.setValue(cp + "sourceFile",      QString::fromStdString(c.sourceFile));
        settings.setValue(cp + "character",       QString::fromStdString(c.character));
        settings.setValue(cp + "start",           c.start);
        settings.setValue(cp + "end",             c.end);
        settings.setValue(cp + "transcript",      QString::fromStdString(c.transcript));
        settings.setValue(cp + "editedText",      QString::fromStdString(c.editedText));
        settings.setValue(cp + "matchState",      c.matchState);
        settings.setValue(cp + "confidence",      static_cast<double>(c.confidence));
        settings.setValue(cp + "scriptLineNumber", c.scriptLineNumber);
        settings.setValue(cp + "scriptSegment",   QString::fromStdString(c.scriptSegment));

        // Deleted regions
        QVariantList regions;
        for (const auto& [rs, re] : c.deletedRegions) {
            QVariantList pair;
            pair.append(rs);
            pair.append(re);
            regions.append(QVariant(pair));
        }
        settings.setValue(cp + "deletedRegions", regions);
    }

    for (size_t ci = 0; ci < m_clips.size(); ++ci) {
        const auto& dc = m_clips[ci];
        spdlog::debug("AudioSync: Saved clip {} state={} start={:.2f} end={:.2f} "
                      "scriptLine={} char='{}' delRegions={}",
                      ci, dc.matchState, dc.start, dc.end,
                      dc.scriptLineNumber, dc.character,
                      dc.deletedRegions.size());
    }
    spdlog::info("AudioSync: Saved project state ({} clips)", m_clips.size());
}

void AudioSync::restoreProjectState(const QString& projectName)
{
    if (projectName.isEmpty()) return;

    m_restoring = true;  // Suppress redundant UI rebuilds inside loadScript

    QSettings settings("ROUNDTABLE", "NLE");
    QString prefix = "Project/" + projectName + "/AudioSync/";

    // Restore script source
    QString scriptSource = settings.value(prefix + "scriptSource").toString();
    if (!scriptSource.isEmpty()) {
        m_lastScriptSource = scriptSource;

        // Restore display name so it persists across sessions
        QString savedName = settings.value(prefix + "scriptDisplayName").toString();
        if (!savedName.isEmpty())
            m_pendingSessionName = savedName.toStdString();

        // Restore raw script content to avoid auto-fetching from URL on project open.
        QString rawContent = settings.value(prefix + "scriptRawContent").toString();
        m_scriptRawContent = rawContent.toStdString();

        if (!m_scriptRawContent.empty()) {
            // Restore from saved content instead of re-fetching
            loadScript(m_scriptRawContent, scriptSource.toStdString());
        } else if (scriptSource.startsWith("http://") || scriptSource.startsWith("https://")) {
            fetchScriptFromUrl(scriptSource);
        } else {
            loadScript(scriptSource.toStdString());
        }
    }

    // Restore audio paths ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â batch import without per-file loadAudioSamples
    m_audioPaths.clear();
    if (m_audioFileList) m_audioFileList->clear();
    m_fileWaveforms.clear();
    m_filePlayBtns.clear();
    m_fileTimeLabels.clear();
    QStringList audioPaths = settings.value(prefix + "audioPaths").toStringList();
    for (const auto& path : audioPaths) {
        if (!path.isEmpty() && QFile::exists(path)) {
            m_audioPaths.push_back(path.toStdString());
        }
    }
    if (!m_audioPaths.empty()) {
        m_audioImported = true;
        m_audioPath = m_audioPaths.back();
        if (m_audioPathEdit) m_audioPathEdit->setText(QString::fromStdString(m_audioPath));
        m_audioStatus->setText(QString("%1 file(s) imported").arg(m_audioPaths.size()));
        loadAudioSamples();  // single batch load — must precede addAudioFileListItem
        // Build file-list items AFTER samples so durations are available
        if (m_audioFileList) {
            for (const auto& p : m_audioPaths)
                addAudioFileListItem(QString::fromStdString(p));
        }
        spdlog::info("AudioSync: Batch-imported {} audio files", m_audioPaths.size());
    }

    // ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ Restore clips (full deserialization) ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬
    int clipCount = settings.value(prefix + "clipCount", 0).toInt();
    if (clipCount > 0) {
        m_clips.clear();
        m_clips.reserve(static_cast<size_t>(clipCount));

        for (int i = 0; i < clipCount; ++i) {
            QString cp = prefix + "clip/" + QString::number(i) + "/";
            SyncClip c;
            c.id              = settings.value(cp + "id", i).toInt();
            c.sourceFile      = settings.value(cp + "sourceFile").toString().toStdString();
            c.character       = settings.value(cp + "character").toString().toStdString();
            c.start           = settings.value(cp + "start", 0.0).toDouble();
            c.end             = settings.value(cp + "end", 0.0).toDouble();
            c.transcript      = settings.value(cp + "transcript").toString().toStdString();
            c.editedText      = settings.value(cp + "editedText").toString().toStdString();
            c.matchState      = settings.value(cp + "matchState", 0).toInt();
            c.confidence      = static_cast<float>(settings.value(cp + "confidence", 0.0).toDouble());
            c.scriptLineNumber = settings.value(cp + "scriptLineNumber", -1).toInt();
            c.scriptSegment   = settings.value(cp + "scriptSegment").toString().toStdString();

            // Deleted regions
            QVariantList regions = settings.value(cp + "deletedRegions").toList();
            for (const auto& r : regions) {
                QVariantList pair = r.toList();
                if (pair.size() >= 2)
                    c.deletedRegions.emplace_back(pair[0].toDouble(), pair[1].toDouble());
            }

            m_clips.push_back(std::move(c));
        }

        for (size_t ci = 0; ci < m_clips.size(); ++ci) {
            const auto& dc = m_clips[ci];
            spdlog::debug("AudioSync: Clip {} restored: state={} start={:.2f} end={:.2f} "
                          "scriptLine={} char='{}' delRegions={}",
                          ci, dc.matchState, dc.start, dc.end,
                          dc.scriptLineNumber, dc.character,
                          dc.deletedRegions.size());
        }
        spdlog::info("AudioSync: Restored {} clips from project state", m_clips.size());
    }

    // Restore workflow state flags
    m_scriptLoaded      = settings.value(prefix + "scriptLoaded",     m_scriptLoaded).toBool();
    m_audioImported     = settings.value(prefix + "audioImported",    m_audioImported).toBool();
    m_transcriptionDone = settings.value(prefix + "transcriptionDone", m_transcriptionDone).toBool();
    m_syncDone          = settings.value(prefix + "syncDone",         m_syncDone).toBool();

    spdlog::info("AudioSync: Workflow flags — script={} audio={} transcribed={} synced={}",
                 m_scriptLoaded, m_audioImported, m_transcriptionDone, m_syncDone);

    // ── Rebuild m_allTranscriptionResults from restored clips ───────────
    // The raw whisper output is not persisted, but the transcribe tab
    // checks m_allTranscriptionResults[i].segments to decide "Pending"
    // vs "Transcribed".  Reconstruct a synthetic segment per audio file
    // from the saved clip data so the UI shows the correct status.
    if (m_transcriptionDone && !m_audioPaths.empty()) {
        m_allTranscriptionResults.resize(m_audioPaths.size());
        for (size_t i = 0; i < m_audioPaths.size(); ++i) {
            if (!m_allTranscriptionResults[i].segments.empty())
                continue;  // already populated

            // Collect transcript text from all clips belonging to this file
            std::string combined;
            for (const auto& c : m_clips) {
                if (c.sourceFile == m_audioPaths[i] && !c.transcript.empty()) {
                    if (!combined.empty()) combined += ' ';
                    combined += c.transcript;
                }
            }
            if (!combined.empty()) {
                TranscriptionSegment seg;
                seg.text = std::move(combined);
                m_allTranscriptionResults[i].segments.push_back(std::move(seg));
            }
        }
        spdlog::info("AudioSync: Rebuilt transcription results for {} files",
                     m_audioPaths.size());
    }
    // Persist restored data into the current session so that switching
    // sessions or save/restore cycles don't lose the data.
    saveCurrentSession();
    m_restoring = false;  // Re-enable normal UI rebuilds

    // Refresh UI to show restored state ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â single populateCards call
    updateWorkflowState();
    populateLeftList();
    // Only rebuild cards now if the script loaded synchronously (local file).
    // For URL-based scripts the async fetchScriptFromUrl callback will invoke
    // loadScript -> populateScriptList -> populateCards once the fetch completes.
    if (m_script) {
        if (isVisible()) {
            populateCards();
        } else {
            // Defer heavy card build until the panel is actually shown.
            m_cardsDirty = true;
            spdlog::info("AudioSync::restoreProjectState: panel hidden, deferring populateCards ({} clips)",
                         m_clips.size());
        }
    }

    // Default to showing the MATCH CHARACTERS side panel when clips exist
    if (!m_clips.empty() && m_syncDone)
        showAudioSidePanel(3);
}

void AudioSync::restoreAudioPaths(const QString& projectName)
{
    if (projectName.isEmpty() || !m_audioPaths.empty()) return;

    QSettings settings("ROUNDTABLE", "NLE");
    QString prefix = "Project/" + projectName + "/AudioSync/";

    QStringList audioPaths = settings.value(prefix + "audioPaths").toStringList();
    if (audioPaths.isEmpty()) return;

    m_audioPaths.clear();
    if (m_audioFileList) m_audioFileList->clear();
    for (const auto& path : audioPaths) {
        if (!path.isEmpty() && QFile::exists(path))
            m_audioPaths.push_back(path.toStdString());
    }
    if (!m_audioPaths.empty()) {
        m_audioImported = true;
        m_audioPath = m_audioPaths.back();
        if (m_audioPathEdit) m_audioPathEdit->setText(QString::fromStdString(m_audioPath));
        m_audioStatus->setText(QString("%1 file(s) imported").arg(m_audioPaths.size()));
        loadAudioSamples();
        if (m_audioFileList) {
            for (const auto& p : m_audioPaths)
                addAudioFileListItem(QString::fromStdString(p));
        }
        spdlog::info("AudioSync::restoreAudioPaths: supplemented {} paths from QSettings",
                     m_audioPaths.size());
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Binary blob serialization (for .rtp project file)
// ═══════════════════════════════════════════════════════════════════════════

static constexpr uint32_t AUDIO_SYNC_BLOB_VERSION = 3;

std::vector<uint8_t> AudioSync::serializeToBlob() const
{
    BinaryWriter w;
    w.writeU32(AUDIO_SYNC_BLOB_VERSION);

    // Script source
    w.writeString(m_lastScriptSource.isEmpty() ? std::string{}
                  : m_lastScriptSource.toStdString());

    // Script display name (v2+) — written early so restore can use it before loadScript
    QString displayName;
    auto dnIt = m_scriptSessions.find(m_activeScriptKey);
    if (dnIt != m_scriptSessions.end())
        displayName = QString::fromStdString(dnIt->second.displayName);
    w.writeString(displayName.toStdString());

    // Script raw content (v3+) — stored so we can restore without re-fetching from URL
    w.writeString(m_scriptRawContent);

    // Audio paths
    w.writeU32(static_cast<uint32_t>(m_audioPaths.size()));
    for (const auto& p : m_audioPaths)
        w.writeString(p);

    // Workflow flags
    w.writeU8(m_scriptLoaded      ? 1 : 0);
    w.writeU8(m_audioImported     ? 1 : 0);
    w.writeU8(m_transcriptionDone ? 1 : 0);
    w.writeU8(m_syncDone          ? 1 : 0);

    // Clips
    w.writeU32(static_cast<uint32_t>(m_clips.size()));
    for (const auto& c : m_clips) {
        w.writeU32(static_cast<uint32_t>(c.id));
        w.writeString(c.sourceFile);
        w.writeString(c.character);
        w.writeF64(c.start);
        w.writeF64(c.end);
        w.writeString(c.transcript);
        w.writeString(c.editedText);
        w.writeU32(static_cast<uint32_t>(c.matchState));
        w.writeF32(c.confidence);
        w.writeU32(static_cast<uint32_t>(c.scriptLineNumber));
        w.writeString(c.scriptSegment);

        // Deleted regions
        w.writeU32(static_cast<uint32_t>(c.deletedRegions.size()));
        for (const auto& [rs, re] : c.deletedRegions) {
            w.writeF64(rs);
            w.writeF64(re);
        }
    }

    spdlog::info("AudioSync::serializeToBlob: {} audioPaths, {} clips, {} bytes",
                 m_audioPaths.size(), m_clips.size(), w.data().size());
    for (size_t i = 0; i < m_audioPaths.size(); ++i)
        spdlog::info("  serialize audio[{}]: {}", i, m_audioPaths[i]);
    return w.data();
}

void AudioSync::deserializeFromBlob(const std::vector<uint8_t>& blob)
{
    if (blob.empty()) {
        spdlog::warn("AudioSync::deserializeFromBlob: blob is EMPTY");
        return;
    }
    spdlog::info("AudioSync::deserializeFromBlob: blob size={}", blob.size());

    BinaryReader r(blob.data(), blob.size());

    uint32_t version = r.readU32();
    if (version > AUDIO_SYNC_BLOB_VERSION) {
        spdlog::warn("AudioSync::deserializeFromBlob: version {} > supported {}",
                     version, AUDIO_SYNC_BLOB_VERSION);
        return;
    }

    m_restoring = true;

    // Script source
    std::string scriptSource = r.readString();

    // Script display name (v2+) — set as pending name so loadScript uses it
    if (version >= 2 && r.remaining() > 0) {
        std::string savedName = r.readString();
        if (!savedName.empty())
            m_pendingSessionName = savedName;
    }

    // Script raw content (v3+) — allows offline restore without re-fetching from URL
    if (version >= 3 && r.remaining() > 0) {
        m_scriptRawContent = r.readString();
    }

    if (!scriptSource.empty()) {
        m_lastScriptSource = QString::fromStdString(scriptSource);

        // If we have the raw content (v3+ blob), restore from that instead of
        // re-fetching from URL.  This avoids auto-syncing on project open.
        // Users can manually sync via the right-click "Sync with GDrive" menu.
        if (!m_scriptRawContent.empty()) {
            loadScript(m_scriptRawContent, scriptSource);
        } else if (m_lastScriptSource.startsWith("http://") || m_lastScriptSource.startsWith("https://")) {
            fetchScriptFromUrl(m_lastScriptSource);
        } else {
            loadScript(scriptSource);
        }
    }

    // Audio paths
    uint32_t pathCount = r.readU32();
    spdlog::info("AudioSync::deserializeFromBlob: reading {} audio paths", pathCount);
    m_audioPaths.clear();
    if (m_audioFileList) m_audioFileList->clear();
    m_fileWaveforms.clear();
    m_filePlayBtns.clear();
    m_fileTimeLabels.clear();
    for (uint32_t i = 0; i < pathCount; ++i) {
        std::string p = r.readString();
        bool exists = QFile::exists(QString::fromStdString(p));
        spdlog::info("  audio[{}]: exists={} path={}", i, exists, p);
        if (!p.empty() && exists)
            m_audioPaths.push_back(std::move(p));
    }
    spdlog::info("AudioSync::deserializeFromBlob: kept {} audio paths", m_audioPaths.size());
    if (!m_audioPaths.empty()) {
        m_audioImported = true;
        m_audioPath = m_audioPaths.back();
        if (m_audioPathEdit)
            m_audioPathEdit->setText(QString::fromStdString(m_audioPath));
        m_audioStatus->setText(QString("%1 file(s) imported").arg(m_audioPaths.size()));
        loadAudioSamples();
        if (m_audioFileList) {
            for (const auto& ap : m_audioPaths)
                addAudioFileListItem(QString::fromStdString(ap));
        }
    }

    // Workflow flags
    m_scriptLoaded      = (r.readU8() != 0);
    m_audioImported     = (r.readU8() != 0);
    m_transcriptionDone = (r.readU8() != 0);
    m_syncDone          = (r.readU8() != 0);

    // Clips
    uint32_t clipCount = r.readU32();
    m_clips.clear();
    m_clips.reserve(clipCount);
    for (uint32_t i = 0; i < clipCount; ++i) {
        SyncClip c;
        c.id              = static_cast<int>(r.readU32());
        c.sourceFile      = r.readString();
        c.character       = r.readString();
        c.start           = r.readF64();
        c.end             = r.readF64();
        c.transcript      = r.readString();
        c.editedText      = r.readString();
        c.matchState      = static_cast<int>(r.readU32());
        c.confidence      = r.readF32();
        c.scriptLineNumber = static_cast<int>(r.readU32());
        c.scriptSegment   = r.readString();

        uint32_t delCount = r.readU32();
        c.deletedRegions.reserve(delCount);
        for (uint32_t d = 0; d < delCount; ++d) {
            double rs = r.readF64();
            double re = r.readF64();
            c.deletedRegions.emplace_back(rs, re);
        }
        m_clips.push_back(std::move(c));
    }

    // Rebuild transcription results from clips
    if (m_transcriptionDone && !m_audioPaths.empty()) {
        m_allTranscriptionResults.resize(m_audioPaths.size());
        for (size_t fi = 0; fi < m_audioPaths.size(); ++fi) {
            if (!m_allTranscriptionResults[fi].segments.empty())
                continue;
            std::string combined;
            for (const auto& c : m_clips) {
                if (c.sourceFile == m_audioPaths[fi] && !c.transcript.empty()) {
                    if (!combined.empty()) combined += ' ';
                    combined += c.transcript;
                }
            }
            if (!combined.empty()) {
                TranscriptionSegment seg;
                seg.text = std::move(combined);
                m_allTranscriptionResults[fi].segments.push_back(std::move(seg));
            }
        }
    }

    m_restoring = false;

    spdlog::info("AudioSync::deserializeFromBlob: restored {} clips, {} audio paths",
                 m_clips.size(), m_audioPaths.size());

    // Refresh session list so restored display names appear
    populateScriptSessionList();
    updateWorkflowState();
    populateLeftList();
    if (m_script) populateCards();
    if (!m_clips.empty() && m_syncDone) showAudioSidePanel(3);
}

} // namespace rt

