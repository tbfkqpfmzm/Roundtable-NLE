/*
 * TimelinePanelInternal.h - Shared internal types for TimelinePanel TUs.
 *
 * Contains GhostTrackOverlay and InsertTrackAtCommand, which are
 * implementation-only classes used by both TimelinePanel.cpp and
 * TimelinePanelMouse.cpp.  NOT part of the public API.
 */
#pragma once

#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "command/Command.h"

#include <QWidget>
#include <QPainter>
#include <QPen>

#include <memory>
#include <string>

namespace rt {

static constexpr const char* kTransitionMimeType = "application/x-roundtable-transition";
static constexpr int kEdgeSnapPixels = 15; // How close cursor must be to a clip edge

// ═════════════════════════════════════════════════════════════════════════════
//  GhostTrackOverlay — transparent overlay widget for drag-to-create previews
// ═════════════════════════════════════════════════════════════════════════════

class GhostTrackOverlay : public QWidget
{
public:
    bool isAbove = true;
    bool reorderMode = false;  // when true, paint as thin insertion bar

    explicit GhostTrackOverlay(QWidget* parent)
        : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setAutoFillBackground(false);
        hide();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        if (reorderMode) {
            QColor bar(255, 180, 60, 220);   // bright orange insertion bar
            p.fillRect(rect(), bar);
            QPen pen(QColor(255, 220, 120, 255), 1);
            p.setPen(pen);
            p.drawRect(rect().adjusted(0, 0, -1, -1));
            return;
        }

        QColor ghostColor = isAbove
            ? QColor(80, 130, 200, 60)    // video = blue tint
            : QColor(60, 160, 90, 60);    // audio = green tint
        QColor borderColor = isAbove
            ? QColor(80, 130, 200, 140)
            : QColor(60, 160, 90, 140);

        p.fillRect(rect(), ghostColor);

        QPen pen(borderColor, 2, Qt::DashLine);
        p.setPen(pen);
        p.drawRect(rect().adjusted(1, 1, -1, -1));

        p.setPen(QColor(200, 200, 200, 180));
        QFont f = p.font();
        f.setPixelSize(12);
        p.setFont(f);
        QString label = isAbove
            ? QStringLiteral("+ New Video Track")
            : QStringLiteral("+ New Audio Track");
        p.drawText(rect(), Qt::AlignCenter, label);
    }
};

// ═════════════════════════════════════════════════════════════════════════════
//  InsertTrackAtCommand — undoable track insertion for ghost-track drag
//
//  The track has ALREADY been inserted by the caller.  This command just
//  records the index so that undo removes it and redo re-inserts it.
// ═════════════════════════════════════════════════════════════════════════════

class InsertTrackAtCommand : public Command
{
public:
    InsertTrackAtCommand(Timeline* tl, size_t index)
        : m_timeline(tl), m_index(index) {}

    void execute() override
    {
        // On redo: re-insert the track we saved during undo
        if (m_held) {
            m_timeline->insertTrack(m_index, std::move(m_held));
            m_held = nullptr;
        }
        // On first push (via pushWithoutExecute) this is a no-op
        // because the track is already in place.
    }

    void undo() override
    {
        m_held = m_timeline->takeTrack(m_index);
    }

    [[nodiscard]] std::string description() const override { return "Insert ghost track"; }
    [[nodiscard]] int typeId() const override { return -1; }

private:
    Timeline*              m_timeline;
    size_t                 m_index;
    std::unique_ptr<Track> m_held;
};

} // namespace rt
