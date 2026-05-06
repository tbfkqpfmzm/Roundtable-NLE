/*
 * ShotComposerUI.cpp - UI construction methods for ShotComposer.
 * Split from ShotComposer.cpp for maintainability.
 */

#include "panels/characters/ShotComposer.h"
#include "panels/characters/ShotComposerInternal.h"

#include "Theme.h"
#include "timeline/MediaRelinker.h"

#ifdef ROUNDTABLE_HAS_SPINE
#include "spine/ModelManager.h"
#include "widgets/SpinePreviewWidget.h"
#endif

#include <QApplication>
#include <QComboBox>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QDrag>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QSlider>
#include <QSplitter>
#include <QStyledItemDelegate>
#include <QTabWidget>
#include <QToolTip>
#include <QVBoxLayout>

#include <spdlog/spdlog.h>

namespace rt {

// ─── Draggable library list ─────────────────────────────────────────────────
// Subclass QListWidget so that drags from the asset library carry a custom MIME
// type ("application/x-roundtable-asset") with the asset kind + identifying data.
// Drop handling lives in ShotComposer::eventFilter().

static constexpr const char* kAssetMime = "application/x-roundtable-asset";

class DragAssetList : public QListWidget {
public:
    using QListWidget::QListWidget;
protected:
    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override {
        if (items.isEmpty()) return nullptr;
        auto* item = items.first();

        QByteArray payload;
        QDataStream ds(&payload, QIODevice::WriteOnly);

        // Determine asset kind from item data
        QString kind = item->data(Qt::UserRole).toString();
        if (kind == QStringLiteral("video")) {
            // Video character in Characters tab
            ds << QStringLiteral("videoChar")
               << item->text()
               << item->data(Qt::UserRole + 1).toString()   // mute path
               << item->data(Qt::UserRole + 2).toString();   // talk path
        } else if (kind == QStringLiteral("videoChar")) {
            // Video character in Videos tab
            ds << QStringLiteral("videoChar")
               << item->data(Qt::UserRole + 1).toString()   // char name
               << item->data(Qt::UserRole + 2).toString()   // mute path
               << item->data(Qt::UserRole + 3).toString();   // talk path
        } else if (kind == QStringLiteral("video_file")) {
            // Standalone video file
            ds << QStringLiteral("video") << item->text();
        } else if (kind.isEmpty()) {
            // Background (text is the filename, no UserRole data)
            ds << QStringLiteral("background") << item->text();
        } else {
            // Regular character — UserRole is the folder name
            ds << QStringLiteral("character") << kind;
        }

        auto* md = new QMimeData;
        md->setData(QString::fromLatin1(kAssetMime), payload);
        return md;
    }
};

QWidget* ShotComposer::createShotsColumn()
{
    const auto& c = Theme::colors();
    const auto& m = Theme::metrics();

    auto* column = new QWidget;
    column->setObjectName("ShotsColumn");
    column->setMinimumWidth(200);
    column->setMaximumWidth(320);
    column->setStyleSheet(QStringLiteral(
        "#ShotsColumn { background: %1; border-right: 1px solid %2; }")
        .arg(Theme::hex(c.surface1))
        .arg(Theme::hex(c.border)));

    auto* colLayout = new QVBoxLayout(column);
    colLayout->setContentsMargins(6, 8, 6, 6);
    colLayout->setSpacing(4);

    // â”€â”€ Header â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    auto* headerLabel = new QLabel(QStringLiteral("\xF0\x9F\x8E\xAC  SHOTS"));
    headerLabel->setStyleSheet(QStringLiteral(
        "font-weight: bold; font-size: 13px; color: %1; padding: 2px 0;")
        .arg(Theme::hex(c.textSecondary)));
    colLayout->addWidget(headerLabel);

    // â”€â”€ Action buttons â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    auto* shotBtnsRow = new QHBoxLayout;
    shotBtnsRow->setSpacing(m.spacingSm);

    m_newShotBtn = new QPushButton("+ NEW");
    m_newShotBtn->setToolTip("New Shot (Ctrl+N)");
    m_newShotBtn->setFixedHeight(28);
    m_newShotBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    shotBtnsRow->addWidget(m_newShotBtn);

    m_saveShotBtn = new QPushButton(QStringLiteral("SAVE"));
    m_saveShotBtn->setToolTip("Save Shot (Ctrl+S)");
    m_saveShotBtn->setFixedHeight(28);
    m_saveShotBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_saveShotBtn->setObjectName("SaveBtn");
    shotBtnsRow->addWidget(m_saveShotBtn);

    m_deleteShotBtn = new QPushButton(QStringLiteral("\xF0\x9F\x97\x91 DELETE"));
    m_deleteShotBtn->setToolTip("Delete Shot");
    m_deleteShotBtn->setFixedHeight(28);
    m_deleteShotBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_deleteShotBtn->setObjectName("DangerBtn");
    shotBtnsRow->addWidget(m_deleteShotBtn);

    colLayout->addLayout(shotBtnsRow);

    // â”€â”€ Search â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    m_shotSearchEdit = new QLineEdit;
    m_shotSearchEdit->setPlaceholderText(QStringLiteral("\xF0\x9F\x94\x8D Search shots..."));
    m_shotSearchEdit->setClearButtonEnabled(true);
    colLayout->addWidget(m_shotSearchEdit);

    // â”€â”€ Shot thumbnail grid (vertical scrolling) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    m_shotList = new QListWidget;
    m_shotList->setObjectName("ShotThumbnailStrip");
    m_shotList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_shotList->setViewMode(QListView::ListMode);
    m_shotList->setFlow(QListView::TopToBottom);
    m_shotList->setWrapping(false);
    m_shotList->setMovement(QListView::Static);
    m_shotList->setResizeMode(QListView::Adjust);
    m_shotList->setIconSize(QSize(80, 45));
    m_shotList->setSpacing(2);
    m_shotList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_shotList->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_shotList->setContextMenuPolicy(Qt::CustomContextMenu);
    m_shotList->setStyleSheet(QStringLiteral(
        "QListWidget { background: %1; border: none; outline: none; }"
        "QListWidget::item { background: %2; border: 2px solid transparent;"
        "  border-radius: 4px; padding: 1px; }"
        "QListWidget::item:selected { border-color: %3; background: %4; }"
        "QListWidget::item:hover { background: %5; }")
        .arg(Theme::hex(c.surface0))
        .arg(Theme::hex(c.surface1))
        .arg(Theme::hex(c.accent))
        .arg(Theme::hex(c.surface2))
        .arg(Theme::hex(c.surface2)));

    // Custom delegate that preserves per-item foreground color when selected
    class ShotItemDelegate : public QStyledItemDelegate {
    public:
        using QStyledItemDelegate::QStyledItemDelegate;
        void paint(QPainter* painter, const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override {
            QStyleOptionViewItem opt = option;
            QVariant fg = index.data(Qt::ForegroundRole);
            if (fg.isValid())
                opt.palette.setColor(QPalette::HighlightedText,
                                     fg.value<QBrush>().color());
            QStyledItemDelegate::paint(painter, opt, index);
        }
    };
    m_shotList->setItemDelegate(new ShotItemDelegate(m_shotList));

    colLayout->addWidget(m_shotList, 1);

    // â”€â”€ Shot dropdown combo â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    m_shotCombo = new QComboBox;
    m_shotCombo->setObjectName("ShotCombo");
    m_shotCombo->setMinimumHeight(28);
    m_shotCombo->setStyleSheet(QStringLiteral(
        "QComboBox { background: %1; color: %2; border: 1px solid %3;"
        "  border-radius: 4px; padding: 4px 8px; font-size: 12px; }"
        "QComboBox::drop-down { border: none; }"
        "QComboBox QAbstractItemView { background: %1; color: %2;"
        "  selection-background-color: %4; }")
        .arg(Theme::hex(c.surface1))
        .arg(Theme::hex(c.text))
        .arg(Theme::hex(c.border))
        .arg(Theme::hex(c.accent)));
    colLayout->addWidget(m_shotCombo);

    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    // Connections
    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

    // Shot thumbnail strip selection â†’ load preset and sync combo
    connect(m_shotList, &QListWidget::currentItemChanged,
            this, [this](QListWidgetItem* current, QListWidgetItem* /*previous*/) {
        if (!current) return;
        auto name = current->data(Qt::UserRole).toString().toStdString();
        if (name.empty()) return;

        m_shotCombo->blockSignals(true);
        int comboIdx = m_shotCombo->findData(current->data(Qt::UserRole));
        if (comboIdx >= 0) m_shotCombo->setCurrentIndex(comboIdx);
        m_shotCombo->blockSignals(false);

        auto preset = m_presetManager.load(name);
        if (preset)
            setCurrentShot(*preset);
    });

    // Shot dropdown combo selection â†’ load preset and sync thumbnail strip
    connect(m_shotCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int index) {
        if (index < 0) return;
        auto name = m_shotCombo->itemData(index).toString().toStdString();
        if (name.empty()) return;

        m_shotList->blockSignals(true);
        for (int i = 0; i < m_shotList->count(); ++i) {
            auto* item = m_shotList->item(i);
            if (item && item->data(Qt::UserRole).toString().toStdString() == name) {
                m_shotList->setCurrentRow(i);
                break;
            }
        }
        m_shotList->blockSignals(false);

        auto preset = m_presetManager.load(name);
        if (preset)
            setCurrentShot(*preset);
    });

    connect(m_newShotBtn, &QPushButton::clicked, this, [this]() { newShot(); });

    connect(m_saveShotBtn, &QPushButton::clicked, this, [this]() {
        saveCurrentShot();
    });

    connect(m_deleteShotBtn, &QPushButton::clicked, this, [this]() {
        auto* current = m_shotList->currentItem();
        if (!current) return;
        auto name = current->data(Qt::UserRole).toString();
        if (name.isEmpty()) return;
        auto reply = QMessageBox::question(
            this, "Delete Shot?",
            QString("Are you sure you want to delete '%1'?\nThis cannot be undone.").arg(name),
            QMessageBox::Yes | QMessageBox::No);
        if (reply != QMessageBox::Yes) return;
        m_presetManager.remove(name.toStdString());
        m_currentShot = ShotPreset();
        m_selectedLayer = -1;
        m_shotNameEdit->clear();
        m_shotNameEdit->setEnabled(false);
        m_defaultShotCheck->setEnabled(false);
        m_defaultCharCombo->setEnabled(false);
        m_defaultCharCombo->clear();
        m_setDefaultBtn->setEnabled(false);
        refreshShotList();
        refreshLayerList();
        clearLayerProperties();
    });

    // Shot list context menu (right-click) â†’ Delete / Duplicate / Rename
    connect(m_shotList, &QListWidget::customContextMenuRequested,
            this, [this](const QPoint& pos) {
        auto* item = m_shotList->itemAt(pos);
        if (!item) return;
        QString shotName = item->data(Qt::UserRole).toString();
        if (shotName.isEmpty()) return;

        QMenu menu(this);
        QAction* actDuplicate = menu.addAction(QStringLiteral("\U0001F4CB Duplicate"));
        QAction* actRename    = menu.addAction(QStringLiteral("\u270F\uFE0F Rename"));
        menu.addSeparator();
        QAction* actDelete    = menu.addAction(QStringLiteral("\U0001F5D1 Delete"));

        QAction* chosen = menu.exec(m_shotList->viewport()->mapToGlobal(pos));
        if (!chosen) return;

        if (chosen == actDuplicate) {
            // Load the shot, prompt for a name, save as duplicate
            auto preset = m_presetManager.load(shotName.toStdString());
            if (!preset) return;
            std::string baseName = shotName.toStdString() + " Copy";
            std::string newName  = baseName;
            int counter = 2;
            while (m_presetManager.hasPreset(newName))
                newName = baseName + " " + std::to_string(counter++);
            bool ok = false;
            QString dupeName = QInputDialog::getText(this, "Duplicate Shot",
                "Name for the duplicate:", QLineEdit::Normal,
                QString::fromStdString(newName), &ok);
            if (!ok || dupeName.trimmed().isEmpty()) return;
            ShotPreset dupe = *preset;
            dupe.setName(dupeName.trimmed().toStdString());
            m_presetManager.save(dupe);
            setCurrentShot(dupe);
            refreshShotList();
            spdlog::info("ShotComposer: Duplicated shot '{}' as '{}'",
                shotName.toStdString(), dupe.name());
        }
        else if (chosen == actRename) {
            bool ok = false;
            QString newName = QInputDialog::getText(this, "Rename Shot",
                "New name:", QLineEdit::Normal, shotName, &ok);
            if (!ok || newName.trimmed().isEmpty() || newName.trimmed() == shotName)
                return;
            std::string newNameStd = newName.trimmed().toStdString();
            if (m_presetManager.hasPreset(newNameStd)) {
                QMessageBox::warning(this, "Rename Shot",
                    QString("A shot named '%1' already exists.").arg(newName.trimmed()));
                return;
            }
            auto preset = m_presetManager.load(shotName.toStdString());
            if (!preset) return;
            ShotPreset renamed = *preset;
            renamed.setName(newNameStd);
            m_presetManager.save(renamed);
            m_presetManager.remove(shotName.toStdString());
            setCurrentShot(renamed);
            refreshShotList();
            spdlog::info("ShotComposer: Renamed shot '{}' â†’ '{}'",
                shotName.toStdString(), newNameStd);
        }
        else if (chosen == actDelete) {
            auto reply = QMessageBox::question(
                this, "Delete Shot?",
                QString("Are you sure you want to delete '%1'?\nThis cannot be undone.").arg(shotName),
                QMessageBox::Yes | QMessageBox::No);
            if (reply != QMessageBox::Yes) return;
            m_presetManager.remove(shotName.toStdString());
            if (m_currentShot.name() == shotName.toStdString()) {
                m_currentShot = ShotPreset();
                m_selectedLayer = -1;
                m_shotNameEdit->clear();
                m_shotNameEdit->setEnabled(false);
                m_defaultShotCheck->setEnabled(false);
                m_defaultCharCombo->setEnabled(false);
                m_defaultCharCombo->clear();
                m_setDefaultBtn->setEnabled(false);
            }
            refreshShotList();
            refreshLayerList();
            clearLayerProperties();
            spdlog::info("ShotComposer: Deleted shot '{}'", shotName.toStdString());
        }
    });

    // Shot search â†’ refresh shot list
    connect(m_shotSearchEdit, &QLineEdit::textChanged,
            this, [this]() { refreshShotList(); });

    return column;
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// Character filter thumbnail column â€” sits between rail and shots column
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

QWidget* ShotComposer::createCharFilterColumn()
{
    const auto& c = Theme::colors();

    constexpr int kFilterThumbSz = 90;

    auto* column = new QWidget;
    column->setObjectName("CharFilterColumn");
    column->setFixedWidth(kFilterThumbSz + 24);
    column->setStyleSheet(QStringLiteral(
        "#CharFilterColumn { background: %1; border-right: 1px solid %2; }")
        .arg(Theme::hex(c.surface0))
        .arg(Theme::hex(c.border)));

    auto* colLayout = new QVBoxLayout(column);
    colLayout->setContentsMargins(4, 8, 4, 4);
    colLayout->setSpacing(0);

    m_charFilterList = new QListWidget;
    m_charFilterList->setObjectName("CharFilterList");
    m_charFilterList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_charFilterList->setViewMode(QListView::IconMode);
    m_charFilterList->setFlow(QListView::TopToBottom);
    m_charFilterList->setWrapping(false);
    m_charFilterList->setMovement(QListView::Static);
    m_charFilterList->setResizeMode(QListView::Adjust);
    m_charFilterList->setIconSize(QSize(kFilterThumbSz, kFilterThumbSz));
    m_charFilterList->setGridSize(QSize(kFilterThumbSz + 8, kFilterThumbSz + 22));
    m_charFilterList->setSpacing(2);
    m_charFilterList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_charFilterList->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_charFilterList->setStyleSheet(QStringLiteral(
        "QListWidget { background: transparent; border: none; outline: none; }"
        "QListWidget::item { background: %1; border: 2px solid transparent;"
        "  border-radius: 6px; padding: 2px; }"
        "QListWidget::item:selected { border-color: %2; background: %3; }"
        "QListWidget::item:hover { background: %4; }")
        .arg(Theme::hex(c.surface1))
        .arg(Theme::hex(c.accent))
        .arg(Theme::hex(c.surface2))
        .arg(Theme::hex(c.surface2)));
    colLayout->addWidget(m_charFilterList, 1);

    // "ALL" item â€” will be populated with thumbnails in refreshShotList()
    auto* allItem = new QListWidgetItem(QStringLiteral("ALL"));
    allItem->setData(Qt::UserRole, QString());
    allItem->setTextAlignment(Qt::AlignHCenter | Qt::AlignBottom);
    allItem->setSizeHint(QSize(kFilterThumbSz + 8, 32));
    m_charFilterList->addItem(allItem);
    m_charFilterList->setCurrentRow(0);

    // Selecting a character filters the shots list
    connect(m_charFilterList, &QListWidget::currentItemChanged,
            this, [this]() { refreshShotList(); });

    return column;
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// UI Setup â€” B3-C layout: left=preview+library, right=properties+layers
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

void ShotComposer::setupUI()
{
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // Create the standalone shots column (will be reparented into CharacterShotPanel)
    m_shotsColumn = createShotsColumn();
    // Create the character filter thumbnail column (will be reparented into CharacterShotPanel)
    m_charFilterColumn = createCharFilterColumn();

    m_splitter = new QSplitter(Qt::Horizontal, this);

    m_splitter->addWidget(createLeftPanel());
    m_splitter->addWidget(createPropertiesPanel());

    m_splitter->setStretchFactor(0, 2);   // Preview + Library
    m_splitter->setStretchFactor(1, 1);   // Properties + Layers

    layout->addWidget(m_splitter);
}

// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Left panel: shot header bar + preview viewport + asset library tabs
// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

QWidget* ShotComposer::createLeftPanel()
{
    const auto& c = Theme::colors();
    const auto& m = Theme::metrics();

    auto* leftSplitter = new QSplitter(Qt::Vertical);

    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    // TOP: Shot Header + Preview Viewport
    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    auto* topSection = new QWidget;
    topSection->setObjectName("ShotTopSection");
    topSection->setStyleSheet(QStringLiteral(
        "QWidget#ShotTopSection { background: %1; }")
        .arg(Theme::hex(c.surface0)));
    auto* topLayout = new QVBoxLayout(topSection);
    topLayout->setContentsMargins(0, 0, 0, 0);
    topLayout->setSpacing(0);

    // â”€â”€ Shot Header Bar â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    auto* shotHeader = new QWidget;
    shotHeader->setObjectName("PreviewHeader");
    auto* shotHeaderLayout = new QHBoxLayout(shotHeader);
    shotHeaderLayout->setContentsMargins(m.spacingMd, m.spacingXs, m.spacingMd, m.spacingXs);
    shotHeaderLayout->setSpacing(m.spacingSm);

    auto* shotIcon = new QLabel(QStringLiteral("\xF0\x9F\x8E\xAC"));
    shotIcon->setFixedWidth(24);
    shotHeaderLayout->addWidget(shotIcon);

    auto* shotLabel = new QLabel("COMPOSE");
    shotLabel->setStyleSheet(QStringLiteral(
        "font-weight: bold; font-size: 13px; color: %1;")
        .arg(Theme::hex(c.textSecondary)));
    shotHeaderLayout->addWidget(shotLabel);

    shotHeaderLayout->addStretch();

    topLayout->addWidget(shotHeader);

    // â”€â”€ Preview Viewport â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    m_previewArea = new QWidget;
    m_previewArea->setObjectName("PreviewArea");

    auto* arContainer = new AspectRatioContainer(m_previewArea, 16.0 / 9.0);

    auto* previewInner = new QVBoxLayout(m_previewArea);
    previewInner->setContentsMargins(0, 0, 0, 0);
    previewInner->setSpacing(2);

#ifdef ROUNDTABLE_HAS_SPINE
    m_spinePreview = new SpinePreviewWidget(m_previewArea);
    m_spinePreview->setBackgroundColor(c.surface0);
    previewInner->addWidget(m_spinePreview);

    connect(m_spinePreview, &SpinePreviewWidget::dragStarted,
            this, &ShotComposer::pushUndoState);

    // When the user clicks a layer directly in the preview, sync the layer
    // list selection so spinboxes show the right layer's properties and
    // m_selectedLayer stays consistent.
    connect(m_spinePreview, &SpinePreviewWidget::layerClicked,
            this, [this](int layerIndex) {
        if (layerIndex < 0 || layerIndex >= m_layerList->count()) return;
        if (layerIndex == m_selectedLayer) return; // already synced
        m_layerList->blockSignals(true);
        m_layerList->setCurrentRow(layerIndex);
        m_layerList->blockSignals(false);
        m_selectedLayer = layerIndex;
        populateLayerProperties();
    });

    connect(m_spinePreview, &SpinePreviewWidget::layerTransformChanged,
            this, [this](int layerIndex, float posX, float posY, float scale) {
        if (m_updating) return;
        if (layerIndex < 0 || layerIndex >= m_currentShot.layerCount()) return;
        const auto& ref = m_currentShot.layerOrder()[static_cast<size_t>(layerIndex)];

        if (ref.type == LayerType::Character) {
            auto* ch = m_currentShot.character(ref.index);
            if (!ch) return;
            ch->posX  = posX;
            ch->posY  = posY;
            ch->scale = scale;
            if (layerIndex == m_selectedLayer) {
                m_updating = true;
                m_posXSpin->setValue(static_cast<double>(posX * 100.0));
                m_posYSpin->setValue(static_cast<double>(posY * 100.0));
                m_scaleSpin->setValue(static_cast<double>(scale * 100.0));
                m_updating = false;
            }
        } else if (ref.type == LayerType::Background) {
            auto* bg = m_currentShot.background(ref.index);
            if (!bg) return;
            bg->posX  = posX;
            bg->posY  = posY;
            bg->scale = scale;
            if (layerIndex == m_selectedLayer) {
                m_updating = true;
                m_bgPosXSpin->setValue(static_cast<double>(posX * 100.0));
                m_bgPosYSpin->setValue(static_cast<double>(posY * 100.0));
                m_bgScaleSpin->setValue(static_cast<double>(scale * 100.0));
                m_updating = false;
            }
        }

        emit shotChanged();
    });

    connect(m_spinePreview, &SpinePreviewWidget::layerCropChanged,
            this, [this](int layerIndex, float cropL, float cropR, float cropT, float cropB) {
        if (m_updating) return;
        if (layerIndex < 0 || layerIndex >= m_currentShot.layerCount()) return;
        const auto& ref = m_currentShot.layerOrder()[static_cast<size_t>(layerIndex)];

        if (ref.type == LayerType::Character) {
            auto* ch = m_currentShot.character(ref.index);
            if (!ch) return;
            ch->cropLeft   = cropL;
            ch->cropRight  = cropR;
            ch->cropTop    = cropT;
            ch->cropBottom = cropB;
            if (layerIndex == m_selectedLayer && m_cropLeftSpin) {
                m_updating = true;
                m_cropLeftSpin->setValue(static_cast<double>(cropL));
                m_cropRightSpin->setValue(static_cast<double>(cropR));
                m_cropTopSpin->setValue(static_cast<double>(cropT));
                m_cropBottomSpin->setValue(static_cast<double>(cropB));
                m_updating = false;
            }
        } else if (ref.type == LayerType::Background) {
            auto* bg = m_currentShot.background(ref.index);
            if (!bg) return;
            bg->cropLeft   = cropL;
            bg->cropRight  = cropR;
            bg->cropTop    = cropT;
            bg->cropBottom = cropB;
        }
        emit shotChanged();
    });
#else
    auto* label = new QLabel(QStringLiteral("\xF0\x9F\x8E\xAC Shot Preview\n(Spine not available)"));
    label->setAlignment(Qt::AlignCenter);
    label->setObjectName("PlaceholderLabel");
    previewInner->addWidget(label);
#endif

    topLayout->addWidget(arContainer, 1);

    // â”€â”€ Preview Toolbar â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    auto* previewToolbar = new QWidget;
    previewToolbar->setObjectName("PreviewToolbar");
    auto* tbLayout = new QHBoxLayout(previewToolbar);
    tbLayout->setContentsMargins(m.spacingXs, m.spacingXxs, m.spacingXs, m.spacingXxs);
    tbLayout->setSpacing(m.spacingSm);

    auto* resetViewBtn = new QPushButton("Reset View");
    resetViewBtn->setObjectName("ResetViewBtn");
    resetViewBtn->setToolTip("Reset viewport zoom and pan to 1:1 (Home)");
    tbLayout->addWidget(resetViewBtn);

#ifdef ROUNDTABLE_HAS_SPINE
    connect(resetViewBtn, &QPushButton::clicked, this, [this]() {
        if (m_spinePreview) {
            m_spinePreview->resetViewport();
            m_updating = true;
            m_cameraZoomSpin->setValue(100.0);
            m_cameraPanXSpin->setValue(0.0);
            m_cameraPanYSpin->setValue(0.0);
            m_updating = false;
            m_currentShot.setCameraZoom(1.0f);
            m_currentShot.setCameraX(0.0f);
            m_currentShot.setCameraY(0.0f);
            emit shotChanged();
        }
    });
#endif

    tbLayout->addStretch();

    auto* safeAreasBtn = new QPushButton(QStringLiteral("Safe Margins"));
    safeAreasBtn->setCheckable(true);
    safeAreasBtn->setChecked(false);
    safeAreasBtn->setFixedHeight(22);
    safeAreasBtn->setStyleSheet(
        QStringLiteral("QPushButton { background: %1; color: %2; border: 1px solid %3; "
                       "border-radius: 3px; padding: 0 8px; font-size: 12px; }"
                       "QPushButton:checked { background: %4; color: white; }")
        .arg(Theme::hex(c.surface1),
             Theme::hex(c.textSecondary),
             Theme::hex(c.border),
             Theme::hex(c.accent)));
    tbLayout->addWidget(safeAreasBtn);
#ifdef ROUNDTABLE_HAS_SPINE
    connect(safeAreasBtn, &QPushButton::toggled, this, [this](bool on) {
        if (m_spinePreview) m_spinePreview->setSafeAreasVisible(on);
    });
#endif

    topLayout->addWidget(previewToolbar);

    leftSplitter->addWidget(topSection);

    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    // BOTTOM: Asset Library Tabs
    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    auto* librarySection = new QWidget;
    auto* libraryLayout = new QVBoxLayout(librarySection);
    libraryLayout->setContentsMargins(m.spacingSm, 0, m.spacingSm, m.spacingSm);
    libraryLayout->setSpacing(m.spacingXs);

    // â”€â”€ Zoom slider row (controls thumbnail size) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    auto* zoomRow = new QHBoxLayout;
    zoomRow->setSpacing(4);

    auto* zoomMinus = new QLabel(QStringLiteral("\u2212"));
    zoomMinus->setStyleSheet(QStringLiteral("color: %1; font-size: 12px;")
        .arg(Theme::hex(c.textTertiary)));
    zoomRow->addWidget(zoomMinus);

    m_iconZoomSlider = new QSlider(Qt::Horizontal);
    m_iconZoomSlider->setRange(80, 280);
    m_iconZoomSlider->setValue(m_iconSize);
    m_iconZoomSlider->setFixedWidth(100);
    m_iconZoomSlider->setToolTip("Thumbnail Size");
    m_iconZoomSlider->setStyleSheet(QStringLiteral(
        "QSlider::groove:horizontal { background: %1; height: 4px; border-radius: 2px; }"
        "QSlider::handle:horizontal { background: %2; width: 10px; margin: -3px 0; border-radius: 5px; }"
        "QSlider::handle:horizontal:hover { background: %3; }")
        .arg(Theme::hex(c.border))
        .arg(Theme::hex(c.textSecondary))
        .arg(Theme::hex(c.textPrimary)));
    zoomRow->addWidget(m_iconZoomSlider);

    auto* zoomPlus = new QLabel(QStringLiteral("+"));
    zoomPlus->setStyleSheet(QStringLiteral("color: %1; font-size: 12px;")
        .arg(Theme::hex(c.textTertiary)));
    zoomRow->addWidget(zoomPlus);
    zoomRow->addStretch();

    libraryLayout->addLayout(zoomRow);

    // Helper: configure a QListWidget for icon-mode thumbnail display
    auto setupIconList = [&](QListWidget* list) {
        list->setObjectName("LibraryList");
        list->setSelectionMode(QAbstractItemView::SingleSelection);
        list->setViewMode(QListView::IconMode);
        list->setIconSize(QSize(m_iconSize, m_iconSize));
        list->setGridSize(QSize(m_iconSize + 8, m_iconSize + 22));
        list->setResizeMode(QListView::Adjust);
        list->setWordWrap(true);
        list->setSpacing(2);
        list->setUniformItemSizes(false);
        list->setMovement(QListView::Static);
        list->setWrapping(true);
        list->setFlow(QListView::LeftToRight);
        list->setStyleSheet(QStringLiteral(
            "QListWidget { background: %1; border: none; outline: none; }"
            "QListWidget::item { background: %2; border: 1px solid transparent;"
            "  border-radius: 4px; padding: 2px; }"
            "QListWidget::item:selected { border-color: %3; background: %4; }"
            "QListWidget::item:hover { background: %5; }")
            .arg(Theme::hex(c.surface0))
            .arg(Theme::hex(c.surface1))
            .arg(Theme::hex(c.accent))
            .arg(Theme::hex(c.accentDim))
            .arg(Theme::hex(c.surface2)));
    };

    m_libraryTabs = new QTabWidget;

    // â”€â”€ Characters tab â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    auto* charTab = new QWidget;
    auto* charLayout = new QVBoxLayout(charTab);
    charLayout->setContentsMargins(5, 5, 5, 5);

    m_charSearchEdit = new QLineEdit;
    m_charSearchEdit->setPlaceholderText(QStringLiteral("\xF0\x9F\x94\x8D Search characters..."));
    charLayout->addWidget(m_charSearchEdit);

    auto* filterRow = new QHBoxLayout;
    m_namedOnlyCheck = new QCheckBox("Named Only");
    m_namedOnlyCheck->setChecked(true);
    filterRow->addWidget(m_namedOnlyCheck);
    filterRow->addStretch();
    charLayout->addLayout(filterRow);

    m_characterLibrary = new DragAssetList;
    setupIconList(m_characterLibrary);
    m_characterLibrary->setDragEnabled(true);
    m_characterLibrary->setDragDropMode(QAbstractItemView::DragOnly);
    charLayout->addWidget(m_characterLibrary, 1);

    auto* btnAddChar = new QPushButton(QStringLiteral("Add to Shot \xE2\x86\x92"));
    charLayout->addWidget(btnAddChar);

    m_libraryTabs->addTab(charTab, "Characters");

    // â”€â”€ Backgrounds tab â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    auto* bgTab = new QWidget;
    auto* bgLayout = new QVBoxLayout(bgTab);
    bgLayout->setContentsMargins(5, 5, 5, 5);

    m_backgroundLibrary = new DragAssetList;
    setupIconList(m_backgroundLibrary);
    m_backgroundLibrary->setDragEnabled(true);
    m_backgroundLibrary->setDragDropMode(QAbstractItemView::DragOnly);
    m_backgroundLibrary->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_backgroundLibrary, &QWidget::customContextMenuRequested,
            this, [this](const QPoint& pos) {
        auto* item = m_backgroundLibrary->itemAt(pos);
        if (!item) return;
        const QString oldPath = item->data(Qt::UserRole).toString();
        if (oldPath.isEmpty()) return;
        QMenu menu(m_backgroundLibrary);
        QAction* relinkAct = menu.addAction(tr("Re-link…"));
        QAction* chosen = menu.exec(m_backgroundLibrary->viewport()->mapToGlobal(pos));
        if (chosen != relinkAct) return;
        const QFileInfo oldFi(oldPath);
        const QString startDir = oldFi.exists() ? oldFi.absolutePath() : QDir::homePath();
        // Premiere-style "Display Only Exact Name Matches" — default filter
        // shows only files whose name matches the offline asset.
        const QString exactLabel = tr("Exact name match (%1)").arg(oldFi.fileName());
        const QString filters = exactLabel
            + QStringLiteral(";;") + tr("Images (*.png *.jpg *.jpeg *.bmp *.webp)")
            + QStringLiteral(";;") + tr("All Files (*.*)");
        QFileDialog dlg(this, tr("Re-link: %1").arg(oldFi.fileName()), startDir);
        dlg.setNameFilter(filters);
        dlg.setFileMode(QFileDialog::ExistingFile);
        dlg.selectNameFilter(exactLabel);
        dlg.selectFile(oldFi.fileName());
        if (dlg.exec() != QDialog::Accepted) return;
        const QStringList sel = dlg.selectedFiles();
        if (sel.isEmpty()) return;
        const QString newPath = sel.first();
        if (newPath.isEmpty() || newPath == oldPath) return;
        // Update this composer's own preset library on disk.
        const int n = MediaRelinker::relinkPresetBackground(
            &m_presetManager, oldPath.toStdString(), newPath.toStdString());
        // Also patch the in-memory current shot if it matches.
        for (int i = 0; i < m_currentShot.backgroundCount(); ++i) {
            if (auto* bg = m_currentShot.background(i)) {
                if (bg->path == oldPath.toStdString())
                    bg->path = newPath.toStdString();
            }
        }
        // Let the host wire up timeline-side relinking.
        emit mediaRelinkRequested(oldPath, newPath);
        if (n > 0) emit shotChanged();
        refreshBackgroundLibrary();
    });
    bgLayout->addWidget(m_backgroundLibrary, 1);

    auto* btnAddBg = new QPushButton("Add Background");
    bgLayout->addWidget(btnAddBg);

    m_libraryTabs->addTab(bgTab, "Backgrounds");

    // â”€â”€ Videos tab â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    auto* videoTab = new QWidget;
    auto* videoLayout = new QVBoxLayout(videoTab);
    videoLayout->setContentsMargins(5, 5, 5, 5);

    m_videoLibrary = new DragAssetList;
    setupIconList(m_videoLibrary);
    m_videoLibrary->setDragEnabled(true);
    m_videoLibrary->setDragDropMode(QAbstractItemView::DragOnly);
    m_videoLibrary->setMouseTracking(true);  // for hover-scrub
    videoLayout->addWidget(m_videoLibrary, 1);

    auto* btnAddVideo = new QPushButton("Add Video");
    videoLayout->addWidget(btnAddVideo);

    m_libraryTabs->addTab(videoTab, "Videos");

    libraryLayout->addWidget(m_libraryTabs, 1);

    // â”€â”€ Zoom slider â†’ icon size â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    connect(m_iconZoomSlider, &QSlider::valueChanged,
            this, &ShotComposer::setLibraryIconSize);

    leftSplitter->addWidget(librarySection);

    // Splitter proportions: ~65% preview, ~35% library
    leftSplitter->setStretchFactor(0, 3);
    leftSplitter->setStretchFactor(1, 2);

    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    // Connections
    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

    // Character library
    connect(m_charSearchEdit, &QLineEdit::textChanged,
            this, [this]() { refreshCharacterLibrary(); });
    connect(m_namedOnlyCheck, &QCheckBox::toggled,
            this, [this]() { refreshCharacterLibrary(); });

    connect(m_characterLibrary, &QListWidget::itemDoubleClicked,
            this, [this](QListWidgetItem* item) {
        if (!item) return;
        if (item->data(Qt::UserRole).toString() == QStringLiteral("video")) {
            addCharacter(item->text().toStdString(),
                         item->data(Qt::UserRole + 1).toString().toStdString(),
                         item->data(Qt::UserRole + 2).toString().toStdString());
        } else {
            addCharacter(item->data(Qt::UserRole).toString().toStdString());
        }
    });
    connect(btnAddChar, &QPushButton::clicked, this, [this]() {
        auto items = m_characterLibrary->selectedItems();
        if (items.isEmpty()) return;
        auto* item = items.first();
        if (item->data(Qt::UserRole).toString() == QStringLiteral("video")) {
            addCharacter(item->text().toStdString(),
                         item->data(Qt::UserRole + 1).toString().toStdString(),
                         item->data(Qt::UserRole + 2).toString().toStdString());
        } else {
            addCharacter(item->data(Qt::UserRole).toString().toStdString());
        }
    });

    // Background library
    connect(m_backgroundLibrary, &QListWidget::itemDoubleClicked,
            this, [this](QListWidgetItem* item) {
        if (item) addBackground(item->text().toStdString());
    });
    connect(btnAddBg, &QPushButton::clicked, this, [this]() {
        auto items = m_backgroundLibrary->selectedItems();
        if (!items.isEmpty()) addBackground(items.first()->text().toStdString());
    });

    // Video library
    auto addVideoOrChar = [this](QListWidgetItem* item) {
        if (!item) return;
        std::string filename = item->text().toStdString();
        if (item->data(Qt::UserRole).toString() == QStringLiteral("videoChar")) {
            addCharacter(item->data(Qt::UserRole + 1).toString().toStdString(),
                         item->data(Qt::UserRole + 2).toString().toStdString(),
                         item->data(Qt::UserRole + 3).toString().toStdString());
        } else {
            addVideoLayer(filename);
        }
    };
    connect(m_videoLibrary, &QListWidget::itemDoubleClicked,
            this, addVideoOrChar);
    connect(btnAddVideo, &QPushButton::clicked, this, [this, addVideoOrChar]() {
        auto items = m_videoLibrary->selectedItems();
        if (!items.isEmpty()) addVideoOrChar(items.first());
    });

    return leftSplitter;
}

// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Right panel: properties (tabs) + camera + layers at bottom

} // namespace rt