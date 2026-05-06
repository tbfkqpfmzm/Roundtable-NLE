/*
 * CharactersPanel — lists downloaded characters with their Spine poses and
 * animation names, draggable to the timeline.
 *
 * Stances (Default / Aim / Cover) are shown as expandable groups; animation
 * names are loaded lazily from the skeleton file when the user expands a
 * stance node.  Dragging an animation leaf creates a SpineClip on the timeline.
 *
 * Uses a QTreeWidget so the timeline's existing drop handler
 * (which checks qobject_cast<QTreeWidget*>(event->source())) accepts it.
 */

#pragma once

#include <QWidget>

class QLabel;
class QLineEdit;
class QTimer;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;
namespace rt { class MediaDragTreeWidget; }

namespace rt {

class AnimationVideoCache;
class ModelManager;
class MediaPool;

class CharactersPanel : public QWidget
{
    Q_OBJECT

public:
    explicit CharactersPanel(QWidget* parent = nullptr);
    ~CharactersPanel() override = default;

    void setModelManager(ModelManager* mgr)          { m_modelManager = mgr; }
    void setAnimVideoCache(AnimationVideoCache* cache){ m_animVideoCache = cache; }
    void setMediaPool(MediaPool* pool)                { m_mediaPool = pool; }

    /// Rebuild the tree from current ModelManager state.
    void refresh();

    /// Access the tree widget (for timeline drop-source checks).
    [[nodiscard]] QTreeWidget* treeWidget() const noexcept;

private:
    void buildUI();

    /// Lazily populate animation names under a stance group item.
    /// Called when the user expands a stance node.
    void populateStanceAnims(QTreeWidgetItem* stanceItem,
                             const std::string& charName,
                             const std::string& outfit,
                             int stanceInt);

    ModelManager*          m_modelManager{nullptr};
    AnimationVideoCache*   m_animVideoCache{nullptr};
    MediaPool*             m_mediaPool{nullptr};

    QLineEdit*              m_searchEdit{nullptr};
    MediaDragTreeWidget*    m_tree{nullptr};
    QLabel*                 m_statusLabel{nullptr};
    QLabel*                 m_emptyLabel{nullptr};
    QTimer*                 m_searchDebounce{nullptr};
    QToolButton*            m_btnRefresh{nullptr};
    QToolButton*            m_btnSortAZ{nullptr};
};

} // namespace rt
