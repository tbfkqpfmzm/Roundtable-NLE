/*
 * RelinkMediaDialog — Premiere Pro-style dialog for relinking offline media.
 */

#pragma once

#include <QDialog>
#include <QTreeWidget>
#include <QPushButton>
#include <QLabel>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace rt {

class AssetDatabase;
class MediaPool;

class RelinkMediaDialog : public QDialog
{
    Q_OBJECT

public:
    explicit RelinkMediaDialog(AssetDatabase* assetDb,
                               MediaPool* pool,
                               QWidget* parent = nullptr);

    /// Returns the list of asset IDs that were successfully relinked.
    [[nodiscard]] const std::vector<uint64_t>& relinkedIds() const noexcept { return m_relinked; }

private:
    void populateList();
    void relinkSelected();
    void relinkAll();
    void locateFile(QTreeWidgetItem* item);

    AssetDatabase*       m_assetDb{nullptr};
    MediaPool*           m_pool{nullptr};
    QTreeWidget*         m_tree{nullptr};
    QPushButton*         m_relinkBtn{nullptr};
    QPushButton*         m_relinkAllBtn{nullptr};
    QPushButton*         m_closeBtn{nullptr};
    QLabel*              m_statusLabel{nullptr};
    std::vector<uint64_t> m_relinked;
};

} // namespace rt
