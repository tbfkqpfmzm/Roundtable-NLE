/*
 * EditOperationsSelection.cpp -- SelectionSet and SnapEngine.
 *
 * Split from EditOperations.cpp for maintainability.
 */

#include "timeline/EditOperations.h"

#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Clip.h"
#include "timeline/Marker.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
//  SelectionSet
// ═════════════════════════════════════════════════════════════════════════════

void SelectionSet::selectClip(const ClipRef& ref, bool addToSelection)
{
    if (!addToSelection)
        m_selected.clear();

    // Don't add duplicates
    for (const auto& s : m_selected)
        if (s == ref) return;

    m_selected.push_back(ref);
}

void SelectionSet::deselectClip(const ClipRef& ref)
{
    m_selected.erase(
        std::remove(m_selected.begin(), m_selected.end(), ref),
        m_selected.end());
}

void SelectionSet::toggleClip(const ClipRef& ref)
{
    if (isSelected(ref))
        deselectClip(ref);
    else
        selectClip(ref, true);
}

void SelectionSet::selectRect(const Timeline& timeline, const TimelineRect& rect)
{
    m_selected.clear();

    size_t firstTrack = std::min(rect.topTrack, rect.bottomTrack);
    size_t lastTrack  = std::max(rect.topTrack, rect.bottomTrack);
    int64_t startT    = std::min(rect.startTick, rect.endTick);
    int64_t endT      = std::max(rect.startTick, rect.endTick);

    for (size_t ti = firstTrack; ti <= lastTrack && ti < timeline.trackCount(); ++ti)
    {
        const Track* track = timeline.track(ti);
        for (size_t ci = 0; ci < track->clipCount(); ++ci)
        {
            const Clip* clip = track->clip(ci);
            // Clip overlaps the rectangle?
            if (clip->timelineOut() > startT && clip->timelineIn() < endT)
            {
                m_selected.push_back({ti, clip->id()});
            }
        }
    }
}

void SelectionSet::selectAll(const Timeline& timeline)
{
    m_selected.clear();
    for (size_t ti = 0; ti < timeline.trackCount(); ++ti)
    {
        const Track* track = timeline.track(ti);
        for (size_t ci = 0; ci < track->clipCount(); ++ci)
        {
            m_selected.push_back({ti, track->clip(ci)->id()});
        }
    }
}

void SelectionSet::clear()
{
    m_selected.clear();
}

bool SelectionSet::isSelected(const ClipRef& ref) const
{
    for (const auto& s : m_selected)
        if (s == ref) return true;
    return false;
}

bool SelectionSet::isSelectedById(uint64_t clipId) const
{
    for (const auto& s : m_selected)
        if (s.clipId == clipId) return true;
    return false;
}

std::optional<ClipRef> SelectionSet::singleSelection() const
{
    if (m_selected.size() == 1)
        return m_selected[0];
    return std::nullopt;
}

//  SnapEngine
// ═════════════════════════════════════════════════════════════════════════════

int64_t SnapEngine::thresholdTicks() const
{
    if (m_pps <= 0.0) return 0;
    // Convert pixel threshold to tick threshold
    double secondsThreshold = m_thresholdPx / m_pps;
    return static_cast<int64_t>(secondsThreshold * 48000.0);
}

void SnapEngine::buildTargets(const Timeline& timeline,
                               int64_t playhead,
                               double /*frameRate*/,
                               const std::vector<uint64_t>& excludeClipIds)
{
    m_targets.clear();

    // Convert excludeClipIds to a set for O(log n) lookup
    std::set<uint64_t> excludeSet(excludeClipIds.begin(), excludeClipIds.end());

    // Playhead
    m_targets.push_back({playhead, SnapTarget::Type::Playhead});

    // In/Out points
    if (timeline.inPoint() >= 0)
        m_targets.push_back({timeline.inPoint(), SnapTarget::Type::InPoint});
    if (timeline.outPoint() >= 0)
        m_targets.push_back({timeline.outPoint(), SnapTarget::Type::OutPoint});

    // Markers
    for (const auto& marker : timeline.markers())
        m_targets.push_back({marker.time, SnapTarget::Type::Marker});

    // Clip edges (all tracks)
    for (size_t ti = 0; ti < timeline.trackCount(); ++ti)
    {
        const Track* track = timeline.track(ti);
        for (size_t ci = 0; ci < track->clipCount(); ++ci)
        {
            const Clip* clip = track->clip(ci);
            if (excludeSet.count(clip->id())) continue;

            m_targets.push_back({clip->timelineIn(), SnapTarget::Type::ClipEdge});
            m_targets.push_back({clip->timelineOut(), SnapTarget::Type::ClipEdge});
        }
    }

    // Sort for binary search
    std::sort(m_targets.begin(), m_targets.end(),
              [](const SnapTarget& a, const SnapTarget& b) { return a.tick < b.tick; });
}

void SnapEngine::clearTargets()
{
    m_targets.clear();
}

void SnapEngine::addTarget(const SnapTarget& target)
{
    m_targets.push_back(target);
}

SnapResult SnapEngine::snap(int64_t tick) const
{
    if (!m_enabled || m_targets.empty())
        return {tick, false, 0, SnapTarget::Type::GridLine};

    int64_t threshold = thresholdTicks();
    if (threshold <= 0)
        return {tick, false, 0, SnapTarget::Type::GridLine};

    int64_t bestDist = std::numeric_limits<int64_t>::max();
    int64_t bestTick = tick;
    SnapTarget::Type bestType = SnapTarget::Type::GridLine;

    for (const auto& target : m_targets)
    {
        int64_t dist = std::abs(tick - target.tick);
        if (dist < bestDist)
        {
            bestDist = dist;
            bestTick = target.tick;
            bestType = target.type;
        }
    }

    if (bestDist <= threshold)
        return {bestTick, true, bestTick - tick, bestType};

    return {tick, false, 0, SnapTarget::Type::GridLine};
}

SnapResult SnapEngine::snapPair(int64_t tickA, int64_t tickB) const
{
    SnapResult resultA = snap(tickA);
    SnapResult resultB = snap(tickB);

    if (!resultA.didSnap && !resultB.didSnap)
        return {tickA, false, 0, SnapTarget::Type::GridLine};

    if (resultA.didSnap && !resultB.didSnap)
        return resultA;

    if (!resultA.didSnap && resultB.didSnap)
    {
        // Return the delta that would be applied to tickA
        return {tickA + resultB.delta, true, resultB.delta, resultB.snapType};
    }

    // Both snapped — pick the one with smaller delta
    if (std::abs(resultA.delta) <= std::abs(resultB.delta))
        return resultA;
    else
        return {tickA + resultB.delta, true, resultB.delta, resultB.snapType};
}

// ═════════════════════════════════════════════════════════════════════════════

} // namespace rt
