/*
 * KeyframeEditor.cpp â€” graph editor for keyframe animation curves.
 *
 * Step 21: Keyframe Editor / Graph Editor
 */

#include "panels/effects/KeyframeEditor.h"
#include "Theme.h"

#include "Constants.h"
#include "timeline/Clip.h"
#include "timeline/Keyframe.h"
#include "timeline/KeyframeTrack.h"
#include "command/CommandStack.h"
#include "command/commands/KeyframeCmds.h"

#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QContextMenuEvent>
#include <QMenu>
#include <QAction>
#include <QApplication>

#include <algorithm>
#include <cmath>
#include <limits>

namespace rt {

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Constants
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

static constexpr int    kGridLineAlpha       = 40;
static constexpr int    kGridTextAlpha       = 140;
static constexpr int    kCurvePenWidth       = 2;
static constexpr double kKeyframeRadius      = 5.0;
static constexpr double kTangentRadius       = 4.0;
static constexpr double kTangentLineLen      = 50.0; // pixels
static constexpr double kZoomFactor          = 1.15;
static constexpr int    kBezierSubdivisions  = 64;  // segments per curve
static constexpr double kMinViewSpan         = 100.0; // minimum view span (ticks or value units)
static constexpr int    kMarginLeft          = 50;
static constexpr int    kMarginRight         = 10;
static constexpr int    kMarginTop           = 10;
static constexpr int    kMarginBottom        = 25;

// Curve colors for the 6 base clip properties + extras
static const QColor kCurveColors[] = {
    QColor(255, 100, 100),  // opacity    - red
    QColor(100, 200, 255),  // positionX  - blue
    QColor(100, 255, 100),  // positionY  - green
    QColor(255, 200, 100),  // scaleX     - orange
    QColor(255, 255, 100),  // scaleY     - yellow
    QColor(200, 100, 255),  // rotation   - purple
    QColor(255, 150, 200),  // extra 1
    QColor(150, 255, 200),  // extra 2
};
static constexpr int kNumCurveColors = sizeof(kCurveColors) / sizeof(kCurveColors[0]);

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Construction
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

KeyframeEditor::KeyframeEditor(QWidget* parent)
    : QWidget(parent)
{
    setupUI();
    setMinimumSize(200, 120);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
}

KeyframeEditor::~KeyframeEditor() = default;

void KeyframeEditor::setupUI()
{
    // Build context menu
    m_contextMenu       = new QMenu(this);
    m_actAddKeyframe    = m_contextMenu->addAction(QStringLiteral("Add Keyframe"));
    m_actDeleteKeyframes = m_contextMenu->addAction(QStringLiteral("Delete Selected"));
    m_contextMenu->addSeparator();
    m_actLinear         = m_contextMenu->addAction(QStringLiteral("Linear"));
    m_actBezier         = m_contextMenu->addAction(QStringLiteral("Bezier"));
    m_actHold           = m_contextMenu->addAction(QStringLiteral("Hold"));
    m_contextMenu->addSeparator();
    m_actCopy           = m_contextMenu->addAction(QStringLiteral("Copy"));
    m_actPaste          = m_contextMenu->addAction(QStringLiteral("Paste"));
    m_contextMenu->addSeparator();
    m_actFitAll         = m_contextMenu->addAction(QStringLiteral("Fit All"));
    m_actFitSelection   = m_contextMenu->addAction(QStringLiteral("Fit Selection"));
    m_actSelectAll      = m_contextMenu->addAction(QStringLiteral("Select All"));

    // Connect context-menu actions
    connect(m_actAddKeyframe, &QAction::triggered, this, [this]() {
        // Add keyframe at the first visible curve at context-menu position
        for (int i = 0; i < static_cast<int>(m_curves.size()); ++i) {
            if (m_curves[i].visible) {
                auto time = static_cast<int64_t>(m_contextMenuGraphPos.x());
                addKeyframe(i, time, static_cast<float>(m_contextMenuGraphPos.y()));
                break;
            }
        }
    });
    connect(m_actDeleteKeyframes, &QAction::triggered, this, &KeyframeEditor::deleteSelectedKeyframes);
    connect(m_actLinear, &QAction::triggered, this, [this]() { setInterpolation(0); });
    connect(m_actBezier, &QAction::triggered, this, [this]() { setInterpolation(1); });
    connect(m_actHold,   &QAction::triggered, this, [this]() { setInterpolation(2); });
    connect(m_actCopy,   &QAction::triggered, this, &KeyframeEditor::copySelectedKeyframes);
    connect(m_actPaste,  &QAction::triggered, this, [this]() {
        pasteKeyframes(static_cast<int64_t>(m_contextMenuGraphPos.x()));
    });
    connect(m_actFitAll,      &QAction::triggered, this, &KeyframeEditor::fitViewToAll);
    connect(m_actFitSelection,&QAction::triggered, this, &KeyframeEditor::fitViewToSelection);
    connect(m_actSelectAll,   &QAction::triggered, this, &KeyframeEditor::selectAll);
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Clip binding
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

void KeyframeEditor::setClip(Clip* clip)
{
    m_clip = clip;
    m_selection.clear();
    rebuildCurves();
    emit selectionChanged();
    update();
}

void KeyframeEditor::rebuildCurves()
{
    m_curves.clear();
    if (!m_clip) return;

    // Add the 6 base properties
    auto addCurve = [&](const std::string& name, KeyframeTrack<float>* track, int colorIdx) {
        m_curves.push_back({name, kCurveColors[colorIdx % kNumCurveColors], track, true});
    };

    addCurve("Opacity",    &m_clip->opacity(),   0);
    addCurve("Position X", &m_clip->positionX(), 1);
    addCurve("Position Y", &m_clip->positionY(), 2);
    addCurve("Scale X",    &m_clip->scaleX(),    3);
    addCurve("Scale Y",    &m_clip->scaleY(),    4);
    addCurve("Rotation",   &m_clip->rotation(),  5);
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Curve accessors
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

int KeyframeEditor::curveCount() const noexcept
{
    return static_cast<int>(m_curves.size());
}

const CurveEntry& KeyframeEditor::curve(int i) const
{
    return m_curves[static_cast<size_t>(i)];
}

void KeyframeEditor::setCurveVisible(int index, bool visible)
{
    if (index >= 0 && index < static_cast<int>(m_curves.size())) {
        m_curves[static_cast<size_t>(index)].visible = visible;
        update();
    }
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Selection
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

void KeyframeEditor::clearSelection()
{
    m_selection.clear();
    emit selectionChanged();
    update();
}

void KeyframeEditor::selectKey(int curveIndex, int keyIndex, bool addToSelection)
{
    if (!addToSelection) m_selection.clear();
    m_selection.insert({curveIndex, keyIndex});
    emit selectionChanged();
    update();
}

void KeyframeEditor::selectAll()
{
    m_selection.clear();
    for (int ci = 0; ci < static_cast<int>(m_curves.size()); ++ci) {
        if (!m_curves[ci].visible || !m_curves[ci].track) continue;
        auto count = static_cast<int>(m_curves[ci].track->keyframeCount());
        for (int ki = 0; ki < count; ++ki)
            m_selection.insert({ci, ki});
    }
    emit selectionChanged();
    update();
}

void KeyframeEditor::boxSelect(const QRectF& graphRect, bool addToSelection)
{
    if (!addToSelection) m_selection.clear();

    for (int ci = 0; ci < static_cast<int>(m_curves.size()); ++ci) {
        if (!m_curves[ci].visible || !m_curves[ci].track) continue;
        auto& track = *m_curves[ci].track;
        for (int ki = 0; ki < static_cast<int>(track.keyframeCount()); ++ki) {
            const auto& kf = track.keyframe(ki);
            if (graphRect.contains(static_cast<double>(kf.time), static_cast<double>(kf.value)))
                m_selection.insert({ci, ki});
        }
    }
    emit selectionChanged();
    update();
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Keyframe operations
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

void KeyframeEditor::addKeyframe(int curveIndex, int64_t time, float value)
{
    if (curveIndex < 0 || curveIndex >= static_cast<int>(m_curves.size())) return;
    auto* track = m_curves[curveIndex].track;
    if (!track) return;

    if (m_commandStack) {
        m_commandStack->execute(
            std::make_unique<AddKeyframeCommand>(track, time, value, InterpMode::Linear));
    } else {
        track->addKeyframe(time, value, InterpMode::Linear);
    }
    emit keyframeChanged();
    update();
}

void KeyframeEditor::deleteSelectedKeyframes()
{
    if (m_selection.empty()) return;

    // Delete in reverse key-index order to preserve indices
    std::vector<SelectedKey> sorted(m_selection.begin(), m_selection.end());
    std::sort(sorted.begin(), sorted.end(), [](const SelectedKey& a, const SelectedKey& b) {
        if (a.curveIndex != b.curveIndex) return a.curveIndex > b.curveIndex;
        return a.keyIndex > b.keyIndex;
    });

    for (auto& sk : sorted) {
        if (sk.curveIndex < 0 || sk.curveIndex >= static_cast<int>(m_curves.size())) continue;
        auto* track = m_curves[sk.curveIndex].track;
        if (!track || sk.keyIndex < 0 || sk.keyIndex >= static_cast<int>(track->keyframeCount()))
            continue;
        auto t = track->keyframe(sk.keyIndex).time;
        if (m_commandStack)
            m_commandStack->execute(std::make_unique<RemoveKeyframeCommand>(track, t));
        else
            track->removeKeyframeAtTime(t);
    }

    m_selection.clear();
    emit keyframeChanged();
    emit selectionChanged();
    update();
}

void KeyframeEditor::setInterpolation(int interpMode)
{
    if (m_selection.empty()) return;

    auto mode = static_cast<InterpMode>(std::clamp(interpMode, 0, 2));

    for (auto& sk : m_selection) {
        if (sk.curveIndex < 0 || sk.curveIndex >= static_cast<int>(m_curves.size())) continue;
        auto* track = m_curves[sk.curveIndex].track;
        if (!track || sk.keyIndex < 0 || sk.keyIndex >= static_cast<int>(track->keyframeCount()))
            continue;
        track->keyframe(sk.keyIndex).interp = mode;
    }
    emit keyframeChanged();
    update();
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Clipboard
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

void KeyframeEditor::copySelectedKeyframes()
{
    m_clipboard.clear();
    if (m_selection.empty()) return;

    // Find earliest time for relative offsets
    int64_t earliest = std::numeric_limits<int64_t>::max();
    for (auto& sk : m_selection) {
        if (sk.curveIndex < 0 || sk.curveIndex >= static_cast<int>(m_curves.size())) continue;
        auto* track = m_curves[sk.curveIndex].track;
        if (!track || sk.keyIndex < 0 || sk.keyIndex >= static_cast<int>(track->keyframeCount()))
            continue;
        earliest = std::min(earliest, track->keyframe(sk.keyIndex).time);
    }

    for (auto& sk : m_selection) {
        if (sk.curveIndex < 0 || sk.curveIndex >= static_cast<int>(m_curves.size())) continue;
        auto* track = m_curves[sk.curveIndex].track;
        if (!track || sk.keyIndex < 0 || sk.keyIndex >= static_cast<int>(track->keyframeCount()))
            continue;
        const auto& kf = track->keyframe(sk.keyIndex);
        m_clipboard.push_back({
            sk.curveIndex,
            kf.time - earliest,
            kf.value,
            static_cast<int>(kf.interp)
        });
    }
}

void KeyframeEditor::pasteKeyframes(int64_t time)
{
    if (m_clipboard.empty()) return;

    m_selection.clear();
    for (auto& ce : m_clipboard) {
        if (ce.curveIndex < 0 || ce.curveIndex >= static_cast<int>(m_curves.size())) continue;
        auto* track = m_curves[ce.curveIndex].track;
        if (!track) continue;

        auto t = time + ce.relativeTime;
        auto interp = static_cast<InterpMode>(ce.interp);

        if (m_commandStack)
            m_commandStack->execute(
                std::make_unique<AddKeyframeCommand>(track, t, ce.value, interp));
        else
            track->addKeyframe(t, ce.value, interp);

        // Select the pasted keyframe
        for (int ki = 0; ki < static_cast<int>(track->keyframeCount()); ++ki) {
            if (track->keyframe(ki).time == t) {
                m_selection.insert({ce.curveIndex, ki});
                break;
            }
        }
    }
    emit keyframeChanged();
    emit selectionChanged();
    update();
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  View management
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

void KeyframeEditor::setViewRange(double timeMin, double timeMax,
                                   double valueMin, double valueMax)
{
    m_viewTimeMin  = timeMin;
    m_viewTimeMax  = timeMax;
    m_viewValueMin = valueMin;
    m_viewValueMax = valueMax;
    emit viewChanged();
    update();
}

void KeyframeEditor::fitViewToAll()
{
    if (m_curves.empty()) return;

    double tMin =  std::numeric_limits<double>::max();
    double tMax = -std::numeric_limits<double>::max();
    double vMin =  std::numeric_limits<double>::max();
    double vMax = -std::numeric_limits<double>::max();
    bool any = false;

    for (auto& ce : m_curves) {
        if (!ce.visible || !ce.track) continue;
        for (size_t i = 0; i < ce.track->keyframeCount(); ++i) {
            const auto& kf = ce.track->keyframe(i);
            tMin = std::min(tMin, static_cast<double>(kf.time));
            tMax = std::max(tMax, static_cast<double>(kf.time));
            vMin = std::min(vMin, static_cast<double>(kf.value));
            vMax = std::max(vMax, static_cast<double>(kf.value));
            any = true;
        }
    }

    if (!any) return;

    // Add padding (10% of range, minimum 1 unit)
    double tPad = std::max((tMax - tMin) * 0.1, static_cast<double>(kTicksPerSecond));
    double vPad = std::max((vMax - vMin) * 0.1, 0.1);
    setViewRange(tMin - tPad, tMax + tPad, vMin - vPad, vMax + vPad);
}

void KeyframeEditor::fitViewToSelection()
{
    if (m_selection.empty()) {
        fitViewToAll();
        return;
    }

    double tMin =  std::numeric_limits<double>::max();
    double tMax = -std::numeric_limits<double>::max();
    double vMin =  std::numeric_limits<double>::max();
    double vMax = -std::numeric_limits<double>::max();

    for (auto& sk : m_selection) {
        if (sk.curveIndex < 0 || sk.curveIndex >= static_cast<int>(m_curves.size())) continue;
        auto* track = m_curves[sk.curveIndex].track;
        if (!track || sk.keyIndex < 0 || sk.keyIndex >= static_cast<int>(track->keyframeCount()))
            continue;
        const auto& kf = track->keyframe(sk.keyIndex);
        tMin = std::min(tMin, static_cast<double>(kf.time));
        tMax = std::max(tMax, static_cast<double>(kf.time));
        vMin = std::min(vMin, static_cast<double>(kf.value));
        vMax = std::max(vMax, static_cast<double>(kf.value));
    }

    double tPad = std::max((tMax - tMin) * 0.1, static_cast<double>(kTicksPerSecond));
    double vPad = std::max((vMax - vMin) * 0.1, 0.1);
    setViewRange(tMin - tPad, tMax + tPad, vMin - vPad, vMax + vPad);
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Coordinate transforms
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

QPointF KeyframeEditor::graphToPixel(double time, double value) const
{
    double plotW = width()  - kMarginLeft - kMarginRight;
    double plotH = height() - kMarginTop  - kMarginBottom;
    if (plotW <= 0) plotW = 1;
    if (plotH <= 0) plotH = 1;

    double timeRange  = m_viewTimeMax  - m_viewTimeMin;
    double valueRange = m_viewValueMax - m_viewValueMin;
    if (timeRange  == 0.0) timeRange  = 1.0;
    if (valueRange == 0.0) valueRange = 1.0;

    double x = kMarginLeft + (time  - m_viewTimeMin)  / timeRange  * plotW;
    double y = kMarginTop  + (1.0 - (value - m_viewValueMin) / valueRange) * plotH;
    return {x, y};
}

QPointF KeyframeEditor::pixelToGraph(double px, double py) const
{
    double plotW = width()  - kMarginLeft - kMarginRight;
    double plotH = height() - kMarginTop  - kMarginBottom;
    if (plotW <= 0) plotW = 1;
    if (plotH <= 0) plotH = 1;

    double timeRange  = m_viewTimeMax  - m_viewTimeMin;
    double valueRange = m_viewValueMax - m_viewValueMin;

    double time  = m_viewTimeMin  + (px - kMarginLeft) / plotW * timeRange;
    double value = m_viewValueMin + (1.0 - (py - kMarginTop) / plotH) * valueRange;
    return {time, value};
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Hit testing
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

KeyframeEditor::HitResult KeyframeEditor::hitTest(const QPointF& pos) const
{
    HitResult best;
    double bestDist = kHitRadius;

    for (int ci = 0; ci < static_cast<int>(m_curves.size()); ++ci) {
        if (!m_curves[ci].visible || !m_curves[ci].track) continue;
        auto& track = *m_curves[ci].track;

        for (int ki = 0; ki < static_cast<int>(track.keyframeCount()); ++ki) {
            const auto& kf = track.keyframe(ki);
            QPointF kfPx = graphToPixel(kf.time, kf.value);
            double dist = QLineF(pos, kfPx).length();
            if (dist < bestDist) {
                bestDist = dist;
                best = {ci, ki, false, false};
            }

            // Check tangent handles (only if selected and bezier)
            SelectedKey sk{ci, ki};
            if (m_selection.count(sk) && kf.interp == InterpMode::Bezier) {
                // Out tangent
                if (ki + 1 < static_cast<int>(track.keyframeCount())) {
                    const auto& kfNext = track.keyframe(ki + 1);
                    double dt = static_cast<double>(kfNext.time - kf.time);
                    double dv = static_cast<double>(kfNext.value - kf.value);
                    QPointF tanPx = graphToPixel(
                        kf.time + kf.bezierOutX * dt,
                        kf.value + kf.bezierOutY * dv);
                    double td = QLineF(pos, tanPx).length();
                    if (td < kTangentHitRadius && td < bestDist) {
                        bestDist = td;
                        best = {ci, ki, false, true};
                    }
                }
            }

            // In tangent of previous keyframe (if this kf is selected)
            if (ki > 0 && m_selection.count(sk)) {
                const auto& kfPrev = track.keyframe(ki - 1);
                if (kfPrev.interp == InterpMode::Bezier) {
                    double dt = static_cast<double>(kf.time - kfPrev.time);
                    double dv = static_cast<double>(kf.value - kfPrev.value);
                    QPointF tanPx = graphToPixel(
                        kfPrev.time + kfPrev.bezierInX * dt,
                        kfPrev.value + kfPrev.bezierInY * dv);
                    double td = QLineF(pos, tanPx).length();
                    if (td < kTangentHitRadius && td < bestDist) {
                        bestDist = td;
                        best = {ci, ki - 1, true, false};
                    }
                }
            }
        }
    }
    return best;
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

} // namespace rt