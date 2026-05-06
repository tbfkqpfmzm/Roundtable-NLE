/*
 * CharactersPanel — lists downloaded characters with cached ProRes animations,
 * draggable to the timeline or project bin.
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

    /// Rebuild the tree from current ModelManager + AnimationVideoCache state.
    void refresh();

    /// Access the tree widget (for timeline drop-source checks).
    [[nodiscard]] QTreeWidget* treeWidget() const noexcept;

private:
    void buildUI();

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
