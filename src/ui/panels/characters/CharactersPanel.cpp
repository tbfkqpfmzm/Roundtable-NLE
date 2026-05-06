/*
 * CharactersPanel.cpp — lists downloaded characters and their cached
 * ProRes animations, draggable to the timeline or project bin.
 */

#include "panels/characters/CharactersPanel.h"
#include "Theme.h"
#include "widgets/MediaDragTreeWidget.h"

#ifdef ROUNDTABLE_HAS_SPINE
#include "spine/ModelManager.h"
#include "spine/AnimationVideoCache.h"
#endif
#include "media/MediaPool.h"

#include <QDir>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>
#include <set>

namespace rt {

// ─────────────────────────────────────────────────────────────────────────────

CharactersPanel::CharactersPanel(QWidget* parent)
    : QWidget(parent)
{
    buildUI();
}

QTreeWidget* CharactersPanel::treeWidget() const noexcept
{
    return m_tree;
}

// ─────────────────────────────────────────────────────────────────────────────

void CharactersPanel::buildUI()
{
    const auto& tc = Theme::colors();
    const auto& m  = Theme::metrics();

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    // ── Toolbar ──────────────────────────────────────────────────────────
    auto* toolbar = new QWidget(this);
    toolbar->setFixedHeight(28);
    toolbar->setStyleSheet(QStringLiteral(
        "QWidget { background: %1; border-bottom: 1px solid %2; }")
        .arg(Theme::hex(tc.surface2), Theme::hex(tc.panelBorder)));
    auto* toolbarLayout = new QHBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(m.spacingXs, m.spacingXxs, m.spacingXs, m.spacingXxs);
    toolbarLayout->setSpacing(m.spacingXs);

    // Search field with magnifying glass prefix
    m_searchEdit = new QLineEdit(toolbar);
    m_searchEdit->setPlaceholderText(QStringLiteral("\U0001F50D Search characters\u2026"));
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->setFixedHeight(22);
    m_searchEdit->setStyleSheet(
        QString("QLineEdit { background: %1; color: %2; border: 1px solid %3; "
                "border-radius: %4px; padding: 2px 4px; font-size: 12px; }"
                "QLineEdit:focus { border: 1px solid %5; }")
            .arg(Theme::hex(tc.inputBg), Theme::hex(tc.text),
                 Theme::hex(tc.controlBorder),
                 QString::number(m.radiusSm),
                 Theme::hex(tc.accent)));
    toolbarLayout->addWidget(m_searchEdit, 1);

    // Debounced search — 200ms after last keystroke
    m_searchDebounce = new QTimer(this);
    m_searchDebounce->setSingleShot(true);
    m_searchDebounce->setInterval(200);
    connect(m_searchDebounce, &QTimer::timeout, this, [this]() { refresh(); });
    connect(m_searchEdit, &QLineEdit::textChanged,
            this, [this]() { m_searchDebounce->start(); });

    // Small toolbar button style
    QString tbStyle = QStringLiteral(
        "QToolButton { background: transparent; border: none; color: %1; "
        "font-size: 13px; padding: 2px; border-radius: 3px; }"
        "QToolButton:hover { background: %2; color: %3; }")
        .arg(Theme::hex(tc.textSecondary),
             Theme::hex(tc.controlBgHover),
             Theme::hex(tc.text));

    // Sort A→Z toggle
    m_btnSortAZ = new QToolButton(toolbar);
    m_btnSortAZ->setText(QStringLiteral("A\u2193Z"));
    m_btnSortAZ->setToolTip(tr("Sort alphabetically"));
    m_btnSortAZ->setFixedSize(28, 22);
    m_btnSortAZ->setCheckable(true);
    m_btnSortAZ->setChecked(true);
    m_btnSortAZ->setStyleSheet(tbStyle +
        QStringLiteral("\nQToolButton:checked { color: %1; }")
            .arg(Theme::hex(tc.accent)));
    toolbarLayout->addWidget(m_btnSortAZ);
    connect(m_btnSortAZ, &QToolButton::toggled,
            this, [this]() { refresh(); });

    // Refresh button
    m_btnRefresh = new QToolButton(toolbar);
    m_btnRefresh->setText(QStringLiteral("\U0001F504"));
    m_btnRefresh->setToolTip(tr("Refresh character list"));
    m_btnRefresh->setFixedSize(22, 22);
    m_btnRefresh->setStyleSheet(tbStyle);
    toolbarLayout->addWidget(m_btnRefresh);
    connect(m_btnRefresh, &QToolButton::clicked,
            this, [this]() { refresh(); });

    lay->addWidget(toolbar);

    // ── Tree widget ──────────────────────────────────────────────────────
    m_tree = new MediaDragTreeWidget;
    m_tree->setHeaderHidden(true);
    m_tree->setRootIsDecorated(true);
    m_tree->setIndentation(16);
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->setDragEnabled(true);
    m_tree->setDragDropMode(QAbstractItemView::DragOnly);
    m_tree->setIconSize(QSize(16, 16));
    m_tree->setStyleSheet(
        QString("QTreeWidget { background: %1; color: %2; border: none; }"
                "QTreeWidget::item { padding: 2px 0; }"
                "QTreeWidget::item:hover { background: %3; }"
                "QTreeWidget::item:selected { background: %4; }")
            .arg(Theme::hex(tc.surface0), Theme::hex(tc.text),
                 Theme::hex(tc.controlBgHover),
                 Theme::hex(tc.accent)));
    lay->addWidget(m_tree, 1);

    // ── Empty state label (shown when tree is empty) ─────────────────────
    m_emptyLabel = new QLabel(tr("No characters found"), this);
    m_emptyLabel->setAlignment(Qt::AlignCenter);
    m_emptyLabel->setStyleSheet(
        QString("QLabel { color: %1; font-size: 13px; background: %2; border: none; }")
            .arg(Theme::hex(tc.textDisabled), Theme::hex(tc.surface0)));
    m_emptyLabel->hide();
    lay->addWidget(m_emptyLabel, 1);

    // ── Status bar ───────────────────────────────────────────────────────
    auto* statusBar = new QWidget(this);
    statusBar->setFixedHeight(22);
    statusBar->setStyleSheet(QStringLiteral(
        "QWidget { background: %1; border-top: 1px solid %2; }")
        .arg(Theme::hex(tc.surface2), Theme::hex(tc.panelBorder)));
    auto* statusLayout = new QHBoxLayout(statusBar);
    statusLayout->setContentsMargins(m.spacingMd, 0, m.spacingMd, 0);
    statusLayout->setSpacing(0);

    m_statusLabel = new QLabel(QStringLiteral("0 items"), statusBar);
    m_statusLabel->setStyleSheet(
        QString("QLabel { color: %1; font-size: 12px; background: transparent; border: none; }")
            .arg(Theme::hex(tc.textSecondary)));
    statusLayout->addWidget(m_statusLabel);
    statusLayout->addStretch();

    lay->addWidget(statusBar);
}

// ─────────────────────────────────────────────────────────────────────────────

void CharactersPanel::refresh()
{
    m_tree->clear();

    const auto& tc = Theme::colors();
    QString searchTerm = m_searchEdit ? m_searchEdit->text().trimmed().toLower() : QString();

#ifdef ROUNDTABLE_HAS_SPINE
    spdlog::info("CharactersPanel::refresh — mgr={}, scanned={}, cache={}",
                 static_cast<const void*>(m_modelManager),
                 m_modelManager ? m_modelManager->isScanned() : false,
                 static_cast<const void*>(m_animVideoCache));

    if (m_modelManager && m_modelManager->isScanned() && m_animVideoCache) {

    // Collect character names that have at least one cached video
    auto charNames = m_modelManager->characterNames();
    bool ascending = !m_btnSortAZ || m_btnSortAZ->isChecked();
    if (ascending)
        std::sort(charNames.begin(), charNames.end());
    else
        std::sort(charNames.begin(), charNames.end(), std::greater<>());

    for (const auto& charName : charNames) {
        bool hasCache = m_animVideoCache->hasAnyForCharacter(charName);

        // Use display name for UI
        std::string dispName = m_modelManager->getDisplayName(charName);
        QString qName = QString::fromStdString(dispName);
        if (!searchTerm.isEmpty() && !qName.toLower().contains(searchTerm))
            continue;

        // Parent item for this character
        auto* charItem = new QTreeWidgetItem;
        if (hasCache) {
            size_t count = m_animVideoCache->countForCharacter(charName);
            charItem->setText(0, QString("%1  (%2)").arg(qName).arg(count));
            charItem->setForeground(0, tc.text);
        } else {
            charItem->setText(0, qName);
            charItem->setForeground(0, tc.textDisabled);
        }
        // Will be made draggable below if cached animations are found
        charItem->setFlags(charItem->flags() & ~Qt::ItemIsDragEnabled);

        // Track the first available anim as a fallback for drag data
        bool charDragDataSet = false;

        // Iterate outfits
        const auto* entry = m_modelManager->findByName(charName);
        if (!entry) continue;

        for (const auto& outfit : entry->outfits) {
            size_t outfitCount = m_animVideoCache->countForCharacterOutfit(
                charName, outfit.name);
            if (outfitCount == 0) continue;

            // If multiple outfits have cached anims, add an outfit sub-level;
            // otherwise flatten directly into the character node.
            QTreeWidgetItem* parentForAnims = charItem;
            bool multiOutfit = (entry->outfits.size() > 1);
            if (multiOutfit) {
                auto* outfitItem = new QTreeWidgetItem(charItem);
                QString oName = outfit.displayName.empty()
                    ? QString::fromStdString(outfit.name)
                    : QString::fromStdString(outfit.displayName);
                outfitItem->setText(0, oName);
                outfitItem->setForeground(0, tc.textSecondary);
                outfitItem->setFlags(outfitItem->flags() & ~Qt::ItemIsDragEnabled);
                parentForAnims = outfitItem;
            }

            // Add each cached animation as a draggable leaf
            // We iterate the cache entries directly since AnimationVideoCache
            // doesn't expose per-outfit iteration — filter from full list
            // by checking getEntry for known animation names from skeleton.
            // Instead, enumerate using the skeleton's animation list.
            // ModelManager doesn't expose anim names, but we can rely on
            // the cache's entry count. Let's iterate all known entries.
            // We'll collect entries that match this character+outfit.
            // AnimationVideoCache stores entries keyed by "char|outfit|anim"
            // but doesn't expose iteration. We need to get specific entries.
            // Alternative: use getEntry with animation names from the outfit's
            // skeleton. But we can just list the cache directory.
            //
            // Simplest approach: ask the cache for specific standard animation names,
            // or list the directory on disk.
            
            // List the cache directory for this character/outfit
            std::filesystem::path outfitDir = std::filesystem::path("assets/cache/animations")
                / charName / outfit.name;
            if (!std::filesystem::exists(outfitDir))
                continue;

            std::vector<std::string> animFiles;
            for (const auto& dirEntry : std::filesystem::directory_iterator(outfitDir)) {
                if (!dirEntry.is_regular_file()) continue;
                auto ext = dirEntry.path().extension().string();
                // Accept .mov (ProRes), .mp4, .webm
                if (ext == ".mov" || ext == ".mp4" || ext == ".webm") {
                    animFiles.push_back(dirEntry.path().stem().string());
                }
            }
            std::sort(animFiles.begin(), animFiles.end());

            for (const auto& animName : animFiles) {
                const auto* cacheEntry = m_animVideoCache->getEntry(
                    charName, outfit.name, animName);
                if (!cacheEntry) continue;

                auto* animItem = new QTreeWidgetItem(parentForAnims);
                animItem->setText(0, QString::fromStdString(animName));
                animItem->setForeground(0, tc.success);

                // Store file path and media handle for timeline drag-drop
                QString videoPath = QString::fromStdString(
                    cacheEntry->videoPath.string());
                animItem->setData(0, Qt::UserRole, videoPath);

                // Open media handle lazily — the timeline wiring will open it
                // on drop if needed. Store 0 for now; getMediaHandle is called
                // when the drag actually starts (in case cache changed).
                animItem->setData(0, Qt::UserRole + 1,
                    QVariant::fromValue(static_cast<quint64>(cacheEntry->mediaHandle)));

                // Not a bin item
                animItem->setData(0, Qt::UserRole + 2, false);

                animItem->setFlags(animItem->flags() | Qt::ItemIsDragEnabled);

                // Track drag data for the character header item.
                // Prefer "default", otherwise fall back to the first anim found.
                if (!charDragDataSet || animName == "default") {
                    charItem->setData(0, Qt::UserRole, videoPath);
                    charItem->setData(0, Qt::UserRole + 1,
                        QVariant::fromValue(static_cast<quint64>(cacheEntry->mediaHandle)));
                    charItem->setData(0, Qt::UserRole + 2, false);
                    charItem->setFlags(charItem->flags() | Qt::ItemIsDragEnabled);
                    charDragDataSet = true;
                }
            }
        }

        // For characters with no cached animations at all, add a muted
        // child item indicating they have no pre-rendered videos.
        if (charItem->childCount() == 0 && !hasCache) {
            auto* noCacheItem = new QTreeWidgetItem(charItem);
            noCacheItem->setText(0, QStringLiteral("(no cached animations)"));
            noCacheItem->setForeground(0, tc.textDisabled);
            noCacheItem->setFlags(noCacheItem->flags() & ~Qt::ItemIsDragEnabled);
        }

        if (charItem->childCount() > 0)
            m_tree->addTopLevelItem(charItem);
        else
            delete charItem;
    }

    spdlog::info("CharactersPanel::refresh — {} characters scanned, {} tree items",
                 charNames.size(), m_tree->topLevelItemCount());
    } // end if (m_modelManager && ...)
#endif

    // ── Video-only characters (from shot presets) ──────────────────────
    {
        // Collect names already shown as Spine characters
        std::set<QString> spineNames;
#ifdef ROUNDTABLE_HAS_SPINE
        if (m_modelManager && m_modelManager->isScanned()) {
            for (const auto& n : m_modelManager->characterNames())
                spineNames.insert(QString::fromStdString(n));
        }
#endif
        struct VCEntry { QString name; QString mutePath; QString talkPath; };
        std::vector<VCEntry> videoChars;

        QDir presetsDir(QStringLiteral("assets/presets/shots"));
        if (presetsDir.exists()) {
            for (const auto& fname : presetsDir.entryList({QStringLiteral("*.json")}, QDir::Files)) {
                QFile f(presetsDir.filePath(fname));
                if (!f.open(QIODevice::ReadOnly)) continue;
                QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
                f.close();
                if (!doc.isObject()) continue;
                QJsonArray chars = doc.object().value(QStringLiteral("characters")).toArray();
                for (const auto& cv : chars) {
                    QJsonObject ch = cv.toObject();
                    QString mute = ch.value(QStringLiteral("videoMutePath")).toString();
                    if (mute.isEmpty()) continue;
                    QString charName = ch.value(QStringLiteral("characterName")).toString();
                    if (charName.isEmpty() || spineNames.contains(charName)) continue;
                    // Deduplicate
                    bool already = false;
                    for (const auto& vc : videoChars)
                        if (vc.name == charName) { already = true; break; }
                    if (already) continue;
                    QString talk = ch.value(QStringLiteral("videoTalkPath")).toString();
                    videoChars.push_back({charName, mute, talk});
                }
            }
        }

        bool ascending = !m_btnSortAZ || m_btnSortAZ->isChecked();
        if (ascending)
            std::sort(videoChars.begin(), videoChars.end(),
                      [](const VCEntry& a, const VCEntry& b) { return a.name < b.name; });
        else
            std::sort(videoChars.begin(), videoChars.end(),
                      [](const VCEntry& a, const VCEntry& b) { return a.name > b.name; });

        for (const auto& vc : videoChars) {
            if (!searchTerm.isEmpty() && !vc.name.toLower().contains(searchTerm))
                continue;

            auto* charItem = new QTreeWidgetItem;
            charItem->setText(0, QStringLiteral("\xF0\x9F\x8E\xAC  ") + vc.name);
            charItem->setForeground(0, QColor(200, 170, 255));

            // Mute variant
            if (!vc.mutePath.isEmpty() && std::filesystem::exists(vc.mutePath.toStdString())) {
                auto* muteItem = new QTreeWidgetItem(charItem);
                muteItem->setText(0, QStringLiteral("Mute"));
                muteItem->setForeground(0, tc.success);
                muteItem->setData(0, Qt::UserRole, vc.mutePath);
                muteItem->setData(0, Qt::UserRole + 1, QVariant::fromValue(static_cast<quint64>(0)));
                muteItem->setData(0, Qt::UserRole + 2, false);
                muteItem->setFlags(muteItem->flags() | Qt::ItemIsDragEnabled);

                // Default drag data for the character header = mute variant
                charItem->setData(0, Qt::UserRole, vc.mutePath);
                charItem->setData(0, Qt::UserRole + 1, QVariant::fromValue(static_cast<quint64>(0)));
                charItem->setData(0, Qt::UserRole + 2, false);
                charItem->setFlags(charItem->flags() | Qt::ItemIsDragEnabled);
            }

            // Talk variant
            if (!vc.talkPath.isEmpty() && std::filesystem::exists(vc.talkPath.toStdString())) {
                auto* talkItem = new QTreeWidgetItem(charItem);
                talkItem->setText(0, QStringLiteral("Talk"));
                talkItem->setForeground(0, tc.success);
                talkItem->setData(0, Qt::UserRole, vc.talkPath);
                talkItem->setData(0, Qt::UserRole + 1, QVariant::fromValue(static_cast<quint64>(0)));
                talkItem->setData(0, Qt::UserRole + 2, false);
                talkItem->setFlags(talkItem->flags() | Qt::ItemIsDragEnabled);
            }

            if (charItem->childCount() > 0)
                m_tree->addTopLevelItem(charItem);
            else
                delete charItem;
        }
    }

    // ── Update empty state + status bar ─────────────────────────────────
    int totalItems = m_tree->topLevelItemCount();
    bool empty = (totalItems == 0);
    m_tree->setVisible(!empty);
    m_emptyLabel->setVisible(empty);
    m_statusLabel->setText(empty ? tr("0 items")
        : (totalItems == 1 ? tr("1 item")
                           : tr("%1 items").arg(totalItems)));
}

} // namespace rt
