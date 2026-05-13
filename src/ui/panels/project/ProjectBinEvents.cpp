/*
 * ProjectBinEvents.cpp — Event filter and key handling for ProjectBin.
 * Extracted from ProjectBin.cpp (modularization phase).
 *
 * Contains: eventFilter() — the main event handler that processes
 * key events, drag-and-drop routing, search bar focus, and all
 * item deletion with timeline clip propagation.
 */

#include "panels/project/ProjectBin.h"
#include "Theme.h"
#include "widgets/MediaDragTreeWidget.h"
#include "widgets/ThumbnailGrid.h"
#include "project/Project.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/VideoClip.h"
#include "timeline/AudioClip.h"
#include "timeline/ImageClip.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"

#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeData>
#include <QTreeWidgetItem>
#include <QUrl>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
//  Event filter — key handling, search bar, drag-drop routing
// ═════════════════════════════════════════════════════════════════════════════

bool ProjectBin::eventFilter(QObject* obj, QEvent* event)
{
    // ── Search field: Escape clears focus ──
    if (obj == m_searchField && event->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Escape) {
            m_searchField->clear();
            // Return focus to whichever ancestor wants it (TimelineWorkspace)
            if (auto* p = parentWidget()) {
                // Walk up to the top-level workspace
                while (p->parentWidget() && p->parentWidget()->parentWidget())
                    p = p->parentWidget();
                p->setFocus();
            }
            return true;
        }
    }

    // ── Ctrl+F or '/' focuses the search bar (Premiere Pro behavior) ──
    if (obj == m_listWidget && event->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Slash ||
            (ke->key() == Qt::Key_F && (ke->modifiers() & Qt::ControlModifier))) {
            m_searchField->setFocus();
            m_searchField->selectAll();
            return true;
        }
    }

    // ── List widget / viewport: intercept drag-drop ──
    // delegated to ProjectBinDragDrop.cpp.
    if (obj == m_listWidget || obj == m_listWidget->viewport()) {
        if (event->type() == QEvent::Drop) {
            if (handleDropEvent(event))
                return true;
        }
        if (event->type() == QEvent::DragEnter) {
            if (handleDragEnterEvent(event))
                return true;
        }
        if (event->type() == QEvent::DragMove) {
            if (handleDragMoveEvent(event))
                return true;
        }
        if (event->type() == QEvent::DragLeave) {
            handleDragLeave();
        }
        // Double-click on empty space in list view → Import Media
        if (obj == m_listWidget->viewport() && event->type() == QEvent::MouseButtonDblClick) {
            auto* me = static_cast<QMouseEvent*>(event);
            if (me->button() == Qt::LeftButton) {
                QTreeWidgetItem* item = m_listWidget->itemAt(me->pos());
                if (!item) {
                    importFiles();
                    return true;
                }
            }
        }
    }

    // ── Create Sequence button: accept drag-and-drop of media files ──
    if (obj == m_btnCreateSequence) {
        if (event->type() == QEvent::DragEnter) {
            auto* de = static_cast<QDragEnterEvent*>(event);
            if (de->mimeData()->hasUrls()) {
                de->acceptProposedAction();
                m_btnCreateSequence->setStyleSheet(QStringLiteral(
                    "QToolButton { background: %1; border: none; color: %2; "
                    "font-size: 13px; border-radius: 3px; }")
                    .arg(Theme::hex(Theme::colors().accentDim))
                    .arg(Theme::hex(Theme::colors().accent)));
                return true;
            }
            if (de->mimeData()->hasFormat("application/x-roundtable-media")) {
                de->acceptProposedAction();
                m_btnCreateSequence->setStyleSheet(QStringLiteral(
                    "QToolButton { background: %1; border: none; color: %2; "
                    "font-size: 13px; border-radius: 3px; }")
                    .arg(Theme::hex(Theme::colors().accentDim))
                    .arg(Theme::hex(Theme::colors().accent)));
                return true;
            }
        }
        if (event->type() == QEvent::DragMove) {
            auto* de = static_cast<QDragMoveEvent*>(event);
            if (de->mimeData()->hasUrls() ||
                de->mimeData()->hasFormat("application/x-roundtable-media")) {
                de->acceptProposedAction();
                return true;
            }
        }
        if (event->type() == QEvent::DragLeave) {
            m_btnCreateSequence->setStyleSheet(QStringLiteral(
                "QToolButton { background: transparent; border: none; color: %1; "
                "font-size: 13px; border-radius: 3px; }"
                "QToolButton:hover { background: %2; color: %3; }"
                "QToolButton:pressed { background: %4; color: %5; }")
                .arg(Theme::hex(Theme::colors().textTertiary))
                .arg(Theme::hex(Theme::colors().controlBgHover))
                .arg(Theme::hex(Theme::colors().textPrimary))
                .arg(Theme::hex(Theme::colors().accentDim))
                .arg(Theme::hex(Theme::colors().accent)));
            return true;
        }
        if (event->type() == QEvent::Drop) {
            auto* de = static_cast<QDropEvent*>(event);
            // Reset button style
            m_btnCreateSequence->setStyleSheet(QStringLiteral(
                "QToolButton { background: transparent; border: none; color: %1; "
                "font-size: 13px; border-radius: 3px; }"
                "QToolButton:hover { background: %2; color: %3; }"
                "QToolButton:pressed { background: %4; color: %5; }")
                .arg(Theme::hex(Theme::colors().textTertiary))
                .arg(Theme::hex(Theme::colors().controlBgHover))
                .arg(Theme::hex(Theme::colors().textPrimary))
                .arg(Theme::hex(Theme::colors().accentDim))
                .arg(Theme::hex(Theme::colors().accent)));

            // External file drop (from Windows Explorer)
            if (de->mimeData()->hasUrls()) {
                for (const QUrl& url : de->mimeData()->urls()) {
                    if (url.isLocalFile()) {
                        std::filesystem::path p(url.toLocalFile().toStdString());
                        createSequenceFromMedia(p);
                        break;
                    }
                }
                de->acceptProposedAction();
                return true;
            }

            // Internal media drop (from the bin itself)
            if (de->mimeData()->hasFormat("application/x-roundtable-media")) {
                QByteArray mediaData = de->mimeData()->data("application/x-roundtable-media");
                QStringList paths = QString::fromUtf8(mediaData).split('\n', Qt::SkipEmptyParts);
                if (!paths.isEmpty()) {
                    std::filesystem::path p(paths.first().trimmed().toStdString());
                    createSequenceFromMedia(p);
                }
                de->acceptProposedAction();
                return true;
            }
        }
    }

    // ── List widget: Copy / Paste / Delete for sequences ──
    if (obj == m_listWidget && event->type() == QEvent::KeyPress && m_project) {
        auto* ke = static_cast<QKeyEvent*>(event);
        auto* selected = m_listWidget->currentItem();
        bool isSeq = selected && selected->data(0, Qt::UserRole + 3).toBool();
        size_t seqIdx = isSeq
            ? selected->data(0, Qt::UserRole + 4).toULongLong()
            : size_t(-1);

        // Ctrl+A — select all items
        if (ke->matches(QKeySequence::SelectAll)) {
            m_listWidget->selectAll();
            return true;
        }

        // Ctrl+C — copy selected sequence
        if (ke->matches(QKeySequence::Copy) && isSeq) {
            m_copiedSequenceIdx = static_cast<int>(seqIdx);
            spdlog::info("ProjectBin: copied sequence {} for paste", seqIdx);
            return true;
        }

        // Ctrl+V — paste (duplicate) copied sequence
        if (ke->matches(QKeySequence::Paste) &&
            m_copiedSequenceIdx >= 0 &&
            static_cast<size_t>(m_copiedSequenceIdx) < m_project->sequenceCount()) {
            size_t srcIdx = static_cast<size_t>(m_copiedSequenceIdx);
            if (m_commandStack) {
                size_t newIdx = m_project->sequenceCount();
                m_commandStack->execute(std::make_unique<LambdaCommand>(
                    "Paste Sequence",
                    [this, srcIdx]() {
                        if (m_destroying.load(std::memory_order_acquire)) return;
                        m_project->duplicateSequence(srcIdx);
                        syncListView();
                        emit sequencesChanged();
                    },
                    [this, newIdx]() {
                        if (m_destroying.load(std::memory_order_acquire)) return;
                        m_project->removeSequence(newIdx);
                        syncListView();
                        emit sequencesChanged();
                    }));
            } else {
                m_project->duplicateSequence(srcIdx);
                syncListView();
                emit sequencesChanged();
            }
            return true;
        }

        // Delete / Backspace — handle deletion
        if (ke->key() == Qt::Key_Delete || ke->key() == Qt::Key_Backspace) {
            // ── Helpers for "propagate delete into timeline" (like Premiere Pro) ──
            struct TimelineRef { size_t seqIdx; size_t trackIdx; uint64_t clipId; };
            auto findTimelineRefs =
                [this](const std::vector<std::filesystem::path>& paths)
                -> std::vector<TimelineRef>
            {
                std::vector<TimelineRef> refs;
                if (!m_project || paths.empty()) return refs;
                auto normalize = [](const std::filesystem::path& p) {
                    std::error_code ec;
                    auto c = std::filesystem::weakly_canonical(p, ec);
                    auto s = (ec ? p : c).generic_string();
                    std::transform(s.begin(), s.end(), s.begin(),
                                   [](unsigned char ch){ return std::tolower(ch); });
                    return s;
                };
                std::set<std::string> needles;
                for (const auto& p : paths)
                    needles.insert(normalize(p));
                for (size_t si = 0; si < m_project->sequenceCount(); ++si) {
                    auto* tl = m_project->sequence(si);
                    if (!tl) continue;
                    for (size_t ti = 0; ti < tl->trackCount(); ++ti) {
                        auto* tr = tl->track(ti);
                        if (!tr) continue;
                        for (size_t ci = 0; ci < tr->clipCount(); ++ci) {
                            Clip* c = tr->clip(ci);
                            if (!c) continue;
                            std::string clipPath;
                            if (auto* vc = dynamic_cast<VideoClip*>(c))
                                clipPath = vc->mediaPath();
                            else if (auto* ac = dynamic_cast<AudioClip*>(c))
                                clipPath = ac->mediaPath();
                            else if (auto* ic = dynamic_cast<ImageClip*>(c))
                                clipPath = ic->mediaPath();
                            if (clipPath.empty()) continue;
                            if (needles.count(normalize(clipPath)))
                                refs.push_back({si, ti, c->id()});
                        }
                    }
                }
                return refs;
            };

            auto confirmTimelinePropagation =
                [this](const std::vector<TimelineRef>& foundRefs) -> bool
            {
                if (foundRefs.empty()) return true;
                QMessageBox box(this);
                box.setWindowTitle(tr("Clip is in use"));
                box.setIcon(QMessageBox::Warning);
                box.setText(tr("%1 clip(s) on the timeline reference this media.")
                                .arg(foundRefs.size()));
                box.setInformativeText(
                    tr("Deleting from the bin will also remove these clip(s) "
                       "from the timeline.\n\nProceed?"));
                box.setStandardButtons(QMessageBox::Yes | QMessageBox::Cancel);
                box.setDefaultButton(QMessageBox::Cancel);
                return box.exec() == QMessageBox::Yes;
            };

            struct ExtractedClip {
                size_t seqIdx;
                size_t trackIdx;
                std::unique_ptr<Clip> clip;
            };
            auto extractTimelineClips =
                [this](const std::vector<TimelineRef>& refs)
                -> std::vector<ExtractedClip>
            {
                std::vector<ExtractedClip> out;
                out.reserve(refs.size());
                for (const auto& r : refs) {
                    auto* tl = m_project ? m_project->sequence(r.seqIdx) : nullptr;
                    if (!tl) continue;
                    auto* tr = tl->track(r.trackIdx);
                    if (!tr) continue;
                    auto removed = tr->removeClipById(r.clipId);
                    if (removed)
                        out.push_back({r.seqIdx, r.trackIdx, std::move(removed)});
                }
                return out;
            };
            auto restoreTimelineClips =
                [this](std::vector<ExtractedClip>& extracted)
            {
                for (auto& e : extracted) {
                    auto* tl = m_project ? m_project->sequence(e.seqIdx) : nullptr;
                    if (!tl) continue;
                    auto* tr = tl->track(e.trackIdx);
                    if (!tr) continue;
                    tr->addClip(std::move(e.clip));
                }
                extracted.clear();
            };

            std::function<void(QTreeWidgetItem*, std::vector<std::filesystem::path>&)>
                collectDescendantFiles =
                [&](QTreeWidgetItem* node, std::vector<std::filesystem::path>& out) {
                    if (!node) return;
                    for (int i = 0; i < node->childCount(); ++i) {
                        QTreeWidgetItem* ch = node->child(i);
                        if (!ch) continue;
                        bool chIsBin = ch->data(0, Qt::UserRole + 2).toBool();
                        bool chIsSeq = ch->data(0, Qt::UserRole + 3).toBool();
                        if (chIsBin) {
                            collectDescendantFiles(ch, out);
                        } else if (!chIsSeq) {
                            QString fp = ch->data(0, Qt::UserRole).toString();
                            if (!fp.isEmpty())
                                out.emplace_back(fp.toStdString());
                        }
                    }
                };

            auto allSelected = m_listWidget->selectedItems();

            // Multi-select: mass-delete selected items
            if (allSelected.size() > 1) {
                auto before = std::make_shared<BinSnapshot>(captureBinSnapshot());
                std::vector<std::filesystem::path> toRemove;
                std::vector<QTreeWidgetItem*> binsToDelete;
                for (auto* item : allSelected) {
                    bool itemIsSeq = item->data(0, Qt::UserRole + 3).toBool();
                    bool itemIsBin = item->data(0, Qt::UserRole + 2).toBool();
                    if (itemIsSeq) continue;
                    if (itemIsBin) {
                        collectDescendantFiles(item, toRemove);
                        binsToDelete.push_back(item);
                        continue;
                    }
                    QString fp = item->data(0, Qt::UserRole).toString();
                    if (!fp.isEmpty())
                        toRemove.emplace_back(fp.toStdString());
                }
                auto timelineRefs = findTimelineRefs(toRemove);
                if (!confirmTimelinePropagation(timelineRefs))
                    return true;
                auto extractedTimelineClips =
                    std::make_shared<std::vector<ExtractedClip>>(
                        extractTimelineClips(timelineRefs));
                for (const auto& path : toRemove)
                    removeFile(path);
                for (auto* binItem : binsToDelete) {
                    int idx = m_listWidget->indexOfTopLevelItem(binItem);
                    if (idx >= 0) {
                        delete m_listWidget->takeTopLevelItem(idx);
                    } else if (auto* parent = binItem->parent()) {
                        int ci = parent->indexOfChild(binItem);
                        if (ci >= 0) delete parent->takeChild(ci);
                    }
                }
                if (!toRemove.empty() || !binsToDelete.empty())
                    syncListView();

                if (!extractedTimelineClips->empty())
                    emit timelineClipsMutated();

                if ((!toRemove.empty() || !binsToDelete.empty()) && m_commandStack) {
                    auto after = std::make_shared<ProjectBin::BinSnapshot>(captureBinSnapshot());
                    auto timelineRefsShared =
                        std::make_shared<std::vector<TimelineRef>>(std::move(timelineRefs));
                    m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
                        "Delete Bin Items",
                        [this, after, timelineRefsShared,
                         extractedTimelineClips, extractTimelineClips]() {
                            if (m_destroying.load(std::memory_order_acquire)) return;
                            if (extractedTimelineClips->empty() && !timelineRefsShared->empty())
                                *extractedTimelineClips =
                                    extractTimelineClips(*timelineRefsShared);
                            this->applyBinSnapshot(*after);
                            if (!timelineRefsShared->empty())
                                emit timelineClipsMutated();
                        },
                        [this, before,
                         extractedTimelineClips, restoreTimelineClips]() {
                            if (m_destroying.load(std::memory_order_acquire)) return;
                            this->applyBinSnapshot(*before);
                            if (!extractedTimelineClips->empty()) {
                                restoreTimelineClips(*extractedTimelineClips);
                                emit timelineClipsMutated();
                            }
                        }));
                }
                return true;
            }

            // Single selection: sequence
            if (isSeq && m_project->sequenceCount() > 1) {
                if (m_commandStack) {
                    auto extracted = m_project->extractSequence(seqIdx);
                    if (!extracted) return false;
                    auto shared = std::make_shared<std::unique_ptr<Timeline>>(std::move(extracted));
                    m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
                        "Delete Sequence",
                        [this, seqIdx]() {
                            if (m_destroying.load(std::memory_order_acquire)) return;
                            m_project->removeSequence(seqIdx);
                            syncListView();
                            emit sequencesChanged();
                        },
                        [this, seqIdx, shared]() {
                            if (m_destroying.load(std::memory_order_acquire)) return;
                            m_project->insertSequence(seqIdx, std::move(*shared));
                            syncListView();
                            emit sequencesChanged();
                        }));
                    syncListView();
                    emit sequencesChanged();
                } else {
                    m_project->removeSequence(seqIdx);
                    syncListView();
                    emit sequencesChanged();
                }
                return true;
            }

            // Single selection: bin (delete bin AND its descendants)
            bool isBin = selected && selected->data(0, Qt::UserRole + 2).toBool();
            if (isBin) {
                auto before = std::make_shared<BinSnapshot>(captureBinSnapshot());
                QString binName = selected->text(0);
                spdlog::info("ProjectBin: deleting bin '{}'", binName.toStdString());

                std::vector<std::filesystem::path> contents;
                collectDescendantFiles(selected, contents);

                auto timelineRefs = findTimelineRefs(contents);
                if (!confirmTimelinePropagation(timelineRefs))
                    return true;
                auto extractedTimelineClips =
                    std::make_shared<std::vector<ExtractedClip>>(
                        extractTimelineClips(timelineRefs));

                for (const auto& p : contents)
                    removeFile(p);

                int idx = m_listWidget->indexOfTopLevelItem(selected);
                if (idx >= 0) {
                    delete m_listWidget->takeTopLevelItem(idx);
                } else if (auto* parent = selected->parent()) {
                    int ci = parent->indexOfChild(selected);
                    if (ci >= 0) delete parent->takeChild(ci);
                }
                syncListView();

                if (!extractedTimelineClips->empty())
                    emit timelineClipsMutated();

                if (m_commandStack) {
                    auto after = std::make_shared<BinSnapshot>(captureBinSnapshot());
                    auto timelineRefsShared =
                        std::make_shared<std::vector<TimelineRef>>(std::move(timelineRefs));
                    m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
                        "Delete Bin",
                        [this, after, timelineRefsShared,
                         extractedTimelineClips, extractTimelineClips]() {
                            if (m_destroying.load(std::memory_order_acquire)) return;
                            if (extractedTimelineClips->empty() && !timelineRefsShared->empty())
                                *extractedTimelineClips =
                                    extractTimelineClips(*timelineRefsShared);
                            this->applyBinSnapshot(*after);
                            if (!timelineRefsShared->empty())
                                emit timelineClipsMutated();
                        },
                        [this, before,
                         extractedTimelineClips, restoreTimelineClips]() {
                            if (m_destroying.load(std::memory_order_acquire)) return;
                            this->applyBinSnapshot(*before);
                            if (!extractedTimelineClips->empty()) {
                                restoreTimelineClips(*extractedTimelineClips);
                                emit timelineClipsMutated();
                            }
                        }));
                }
                return true;
            }

            // Single selection: media item
            if (!isSeq) {
                QString fp = selected ? selected->data(0, Qt::UserRole).toString() : QString();
                if (!fp.isEmpty()) {
                    auto before = std::make_shared<BinSnapshot>(captureBinSnapshot());

                    std::vector<std::filesystem::path> contents{
                        std::filesystem::path(fp.toStdString())};
                    auto timelineRefs = findTimelineRefs(contents);
                    if (!confirmTimelinePropagation(timelineRefs))
                        return true;
                    auto extractedTimelineClips =
                        std::make_shared<std::vector<ExtractedClip>>(
                            extractTimelineClips(timelineRefs));

                    removeFile(std::filesystem::path(fp.toStdString()));
                    syncListView();

                    if (!extractedTimelineClips->empty())
                        emit timelineClipsMutated();

                    if (m_commandStack) {
                        auto after = std::make_shared<BinSnapshot>(captureBinSnapshot());
                        auto timelineRefsShared =
                            std::make_shared<std::vector<TimelineRef>>(std::move(timelineRefs));
                        m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
                            "Delete Bin Item",
                            [this, after, timelineRefsShared,
                             extractedTimelineClips, extractTimelineClips]() {
                                if (m_destroying.load(std::memory_order_acquire)) return;
                                if (extractedTimelineClips->empty() && !timelineRefsShared->empty())
                                    *extractedTimelineClips =
                                        extractTimelineClips(*timelineRefsShared);
                                this->applyBinSnapshot(*after);
                                if (!timelineRefsShared->empty())
                                emit timelineClipsMutated();
                            },
                            [this, before,
                             extractedTimelineClips, restoreTimelineClips]() {
                                if (m_destroying.load(std::memory_order_acquire)) return;
                                this->applyBinSnapshot(*before);
                                if (!extractedTimelineClips->empty()) {
                                    restoreTimelineClips(*extractedTimelineClips);
                                    emit timelineClipsMutated();
                                }
                            }));
                    }
                }
                return true;
            }
        }
    }

    return QWidget::eventFilter(obj, event);
}

} // namespace rt
