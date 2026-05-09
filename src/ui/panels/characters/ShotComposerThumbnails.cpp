/*
 * ShotComposerThumbnails.cpp - Thumbnail, library, and property handler methods for ShotComposer.
 * Split from ShotComposer.cpp for maintainability.
 */

#include "panels/characters/ShotComposer.h"
#include "panels/characters/ShotComposerInternal.h"

#include "Theme.h"

#ifdef ROUNDTABLE_HAS_SPINE
#include "spine/ModelManager.h"
#include "spine/SpineEngine.h"
#include "spine/AnimationVideoCache.h"
#include "widgets/SpinePreviewWidget.h"
#endif

#include <QDir>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>

#include <spdlog/spdlog.h>

namespace rt {

void ShotComposer::refreshShotList()
{
    m_shotList->blockSignals(true);
    m_shotList->clear();
    auto names = m_presetManager.presetNames();
    std::sort(names.begin(), names.end());

    // Get filter criteria
    QString searchFilter;
    QString charFilter;
    if (m_shotSearchEdit)
        searchFilter = m_shotSearchEdit->text().trimmed().toLower();
    charFilter = activeCharFilter();

    // Build set of default shot names
    std::set<std::string> defaultShotNames;
    {
        for (const auto& [ch, shotName] : m_characterDefaults)
            defaultShotNames.insert(shotName);
    }

    // Scan all presets once to count per-character shot occurrences
    // and collect character names per shot for tag display.
    struct ShotInfo {
        std::string name;
        bool isDefault = false;
        QStringList charTags;
        int layerCount = 0;
    };
    std::vector<ShotInfo> allShotInfos;
    std::map<std::string, int> charShotCount; // character display name -> shot count
    int unassignedCount = 0;

    for (const auto& n : names) {
        auto preset = m_presetManager.load(n);
        if (!preset) continue;

        ShotInfo si;
        si.name = n;
        si.isDefault = defaultShotNames.count(n) > 0;
        si.layerCount = preset->layerCount();

        // Collect characters from this preset
        QSet<QString> seenChars;
        for (const auto& ch : preset->characters()) {
            std::string dn = m_modelManager
                ? m_modelManager->getDisplayName(ch.characterName)
                : ch.characterName;
            si.charTags << QString::fromStdString(dn);
            seenChars.insert(QString::fromStdString(dn));
        }

        // Count per-character shot occurrences
        if (si.charTags.isEmpty()) {
            ++unassignedCount;
        } else {
            for (const auto& tag : seenChars)
                charShotCount[tag.toStdString()]++;
        }

        allShotInfos.push_back(si);
    }

    int totalShots = static_cast<int>(allShotInfos.size());

    // --- Build character filter chip list (m_charFilterList) ---
    if (m_charFilterList) {
        // Preserve current selection BEFORE clearing
        QString prevFilter;
        if (auto* cur = m_charFilterList->currentItem())
            prevFilter = cur->data(Qt::UserRole).toString();

        m_charFilterList->blockSignals(true);
        m_charFilterList->clear();

        int restoreRow = -1;
        int row = 0;

        // Collect valid character names (downloaded + video + from user shots)
        QSet<QString> validNames;
#ifdef ROUNDTABLE_HAS_SPINE
        if (m_modelManager && m_modelManager->isScanned()) {
            for (const auto& name : m_modelManager->characterDisplayNames())
                validNames.insert(QString::fromStdString(name));
        }
#endif
        for (const auto& [filename, info] : videoCharacterFiles()) {
            (void)filename;
            validNames.insert(QString::fromStdString(info.charName));
        }
        // Also include any character that appears in user shots
        for (const auto& [cn, count] : charShotCount) {
            (void)count;
            validNames.insert(QString::fromStdString(cn));
        }

        // Apply search filter to character names
        QString filterSearchText;
        if (m_filterSearchEdit)
            filterSearchText = m_filterSearchEdit->text().trimmed().toLower();

        // ALL item
        {
            QString label = QString("ALL");
            auto* item = new QListWidgetItem(label);
            item->setData(Qt::UserRole, QString()); // empty = ALL
            item->setData(Qt::UserRole + 1, totalShots);
            item->setSizeHint(QSize(0, 56));
            QFont allFont = item->font();
            allFont.setPixelSize(18);
            allFont.setBold(true);
            item->setFont(allFont);
            item->setForeground(QColor(180, 220, 180));
            m_charFilterList->addItem(item);
            if (prevFilter.isEmpty()) restoreRow = row;
            ++row;
        }

        // UNASSIGNED item
        {
            QString label = QString("UNASSIGNED");
            auto* item = new QListWidgetItem(label);
            item->setData(Qt::UserRole, QStringLiteral("__UNASSIGNED__"));
            item->setData(Qt::UserRole + 1, unassignedCount);
            item->setSizeHint(QSize(0, 56));
            QFont uaFont = item->font();
            uaFont.setPixelSize(18);
            uaFont.setBold(true);
            item->setFont(uaFont);
            item->setForeground(QColor(210, 170, 80));
            m_charFilterList->addItem(item);
            if (prevFilter == QStringLiteral("__UNASSIGNED__")) restoreRow = row;
            ++row;
        }

        // Separator divider line (unselectable, thin grey like tab dividers)
        {
            auto* sep = new QListWidgetItem(QString());
            sep->setFlags(sep->flags() & ~Qt::ItemIsSelectable);
            sep->setSizeHint(QSize(0, 8));
            QColor sepColor(100, 100, 130, 50);
            sep->setBackground(sepColor);
            m_charFilterList->addItem(sep);
            ++row;
        }

        // Character items
        QStringList sortedChars = validNames.values();
        sortedChars.sort();
        for (const auto& cn : sortedChars) {
            // Apply search filter
            if (!filterSearchText.isEmpty() && !cn.toLower().contains(filterSearchText))
                continue;

            int count = 0;
            auto it = charShotCount.find(cn.toStdString());
            if (it != charShotCount.end())
                count = it->second;

            // Get thumbnail for this character
            std::string folderName = m_modelManager
                ? m_modelManager->getFolderName(cn.toStdString())
                : cn.toStdString();
            QPixmap thumb = makeCharacterThumbnail(folderName, 48);

            auto* item = new QListWidgetItem(QIcon(thumb), cn);
            item->setData(Qt::UserRole, cn);
            item->setData(Qt::UserRole + 1, count);
            item->setSizeHint(QSize(0, 60));
            QFont chFont = item->font();
            chFont.setPixelSize(16);
            item->setFont(chFont);
            item->setToolTip(QString("%1 - %2 shots").arg(cn).arg(count));
            m_charFilterList->addItem(item);
            if (cn == prevFilter) restoreRow = row;
            ++row;
        }

        if (restoreRow >= 0 && restoreRow < m_charFilterList->count())
            m_charFilterList->setCurrentRow(restoreRow);
        m_charFilterList->blockSignals(false);
    }

    // --- Build shot list (m_shotList) ---
    constexpr int kThumbW = 320;
    constexpr int kThumbH = 180;

    // Determine sort mode
    enum SortMode { SortAZ, SortFavorites, SortCharacter, SortRecent };
    SortMode sortMode = SortAZ;
    if (m_shotSortCombo) {
        int si = m_shotSortCombo->currentIndex();
        if (si == 1) sortMode = SortFavorites;
        else if (si == 2) sortMode = SortCharacter;
        else if (si == 3) sortMode = SortRecent;
    }

    // Build filtered shot list
    struct FilteredShot {
        std::string name;
        bool isDefault = false;
        QStringList charTags;
        int layerCount = 0;
        int sortKey = 0;
        QString firstCharTag; // for Character sort mode
    };
    std::vector<FilteredShot> filtered;

    for (const auto& si : allShotInfos) {
        // Apply name search filter
        if (!searchFilter.isEmpty()) {
            QString qn = QString::fromStdString(si.name).toLower();
            if (!qn.contains(searchFilter))
                continue;
        }

        // Apply character filter
        if (!charFilter.isEmpty()) {
            if (charFilter == QStringLiteral("__UNASSIGNED__")) {
                if (!si.charTags.isEmpty())
                    continue;
            } else {
                bool hasChar = false;
                for (const auto& tag : si.charTags) {
                    if (tag.compare(charFilter, Qt::CaseInsensitive) == 0) {
                        hasChar = true;
                        break;
                    }
                }
                if (!hasChar)
                    continue;
            }
        }

        FilteredShot fs;
        fs.name = si.name;
        fs.isDefault = si.isDefault;
        fs.charTags = si.charTags;
        fs.layerCount = si.layerCount;
        if (!si.charTags.isEmpty())
            fs.firstCharTag = si.charTags.first();
        filtered.push_back(fs);
    }

    // Sort
    if (sortMode == SortFavorites) {
        // Default shots first, then alphabetical
        std::stable_partition(filtered.begin(), filtered.end(),
            [](const FilteredShot& fs) { return fs.isDefault; });
        auto mid = std::stable_partition(filtered.begin(), filtered.end(),
            [](const FilteredShot& fs) { return fs.isDefault; });
        std::sort(filtered.begin(), mid,
            [](const FilteredShot& a, const FilteredShot& b) { return a.name < b.name; });
        std::sort(mid, filtered.end(),
            [](const FilteredShot& a, const FilteredShot& b) { return a.name < b.name; });
    } else if (sortMode == SortCharacter) {
        std::sort(filtered.begin(), filtered.end(),
            [](const FilteredShot& a, const FilteredShot& b) {
                if (a.isDefault != b.isDefault) return a.isDefault;
                if (a.firstCharTag != b.firstCharTag) return a.firstCharTag < b.firstCharTag;
                return a.name < b.name;
            });
    } else {
        // SortAZ (default alphabetical)
        std::sort(filtered.begin(), filtered.end(),
            [](const FilteredShot& a, const FilteredShot& b) {
                if (a.isDefault != b.isDefault) return a.isDefault;
                return a.name < b.name;
            });
    }

    int selectRow = -1;
    int visibleRow = 0;

    QString lastCharGroup; // for SortCharacter mode

    for (const auto& fs : filtered) {

        // For SortCharacter mode, insert character group headers
        if (sortMode == SortCharacter) {
            if (visibleRow == 0 || fs.firstCharTag != lastCharGroup) {
                lastCharGroup = fs.firstCharTag;
                QString groupLabel = fs.firstCharTag.isEmpty()
                    ? QStringLiteral("Other")
                    : fs.firstCharTag;
                auto* headerItem = new QListWidgetItem(QStringLiteral("\xf0\x9f\x93\x81 ") + groupLabel);
                headerItem->setFlags(headerItem->flags() & ~Qt::ItemIsSelectable);
                headerItem->setSizeHint(QSize(0, 24));
                QFont headerFont;
                headerFont.setBold(true);
                headerFont.setPixelSize(11);
                headerItem->setFont(headerFont);
                headerItem->setForeground(QColor(180, 180, 180));
                m_shotList->addItem(headerItem);
                ++visibleRow;
            }
        }

        // Build thumbnail
        QPixmap thumb;
        QString cachedPath = shotThumbnailPath(fs.name);
        if (!cachedPath.isEmpty() && QFileInfo::exists(cachedPath)) {
            thumb.load(cachedPath);
        }
        if (thumb.isNull()) {
            auto preset = m_presetManager.load(fs.name);
            if (preset)
                thumb = makeShotThumbnail(*preset, kThumbW, kThumbH);
            else
                thumb = QPixmap(kThumbW, kThumbH);
        }

        // Build shot item with data roles for custom delegate
        QString displayName = QString::fromStdString(fs.name);

        auto* item = new QListWidgetItem(QIcon(thumb), displayName);
        item->setData(Qt::UserRole, QString::fromStdString(fs.name));
        item->setData(Qt::UserRole + 1, fs.charTags);     // QStringList for tags
        item->setData(Qt::UserRole + 2, fs.layerCount);    // int layer count
        item->setData(Qt::UserRole + 3, fs.isDefault);     // bool is default
        if (fs.isDefault)
            item->setForeground(QColor(255, 200, 50));

        m_shotList->addItem(item);

        if (!m_currentShot.name().empty() && fs.name == m_currentShot.name())
            selectRow = visibleRow;
        ++visibleRow;
    }

    if (selectRow >= 0)
        m_shotList->setCurrentRow(selectRow);
    m_shotList->blockSignals(false);
}


void ShotComposer::refreshLayerList()
{
    const auto& m = Theme::metrics();
    m_layerList->blockSignals(true);
    m_layerList->clear();
    const auto& order = m_currentShot.layerOrder();
    for (size_t li = 0; li < order.size(); ++li) {
        const auto& ref = order[li];
        QString label;
        QString typeIcon;
        bool isVisible = true;
        float opacity = 1.0f;

        if (ref.type == LayerType::Background) {
            const auto* bg = m_currentShot.background(ref.index);
            if (bg) {
                auto fname = std::filesystem::path(bg->path).filename().string();
                typeIcon = bg->isVideo()
                    ? QStringLiteral("\xF0\x9F\x8E\xAC")   // Ã°Å¸Å½Â¬
                    : QStringLiteral("\xF0\x9F\x96\xBC");   // Ã°Å¸â€“Â¼
                label = QString::fromStdString(fname.empty() ? bg->path : fname);
                isVisible = bg->visible;
                opacity = bg->opacity;
            } else {
                typeIcon = QStringLiteral("\xF0\x9F\x96\xBC");
                label = QString("BG #%1").arg(ref.index);
            }
        } else {
            const auto* ch = m_currentShot.character(ref.index);
            if (ch) {
                typeIcon = QStringLiteral("\xF0\x9F\x91\xA4");  // 👤
                // Show display name (with colons) instead of folder name
                std::string dn = m_modelManager
                    ? m_modelManager->getDisplayName(ch->characterName)
                    : ch->characterName;
                label = QString::fromStdString(dn);
                isVisible = ch->visible;
                opacity = ch->opacity;
            } else {
                typeIcon = QStringLiteral("\xF0\x9F\x91\xA4");
                label = QString("Character #%1").arg(ref.index);
            }
        }

        // Ã¢â€â‚¬Ã¢â€â‚¬ Photoshop-style layer row Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
        auto* item = new QListWidgetItem(m_layerList);
        item->setSizeHint(QSize(0, 44));
        item->setFlags(item->flags() | Qt::ItemIsDragEnabled | Qt::ItemIsSelectable);

        auto* rowWidget = new QWidget;
        rowWidget->setStyleSheet("background: transparent;");
        auto* rowLayout = new QHBoxLayout(rowWidget);
        rowLayout->setContentsMargins(m.spacingSm, m.spacingXs, m.spacingMd, m.spacingXs);
        rowLayout->setSpacing(0);

        // Eye toggle button
        auto* eyeBtn = new QPushButton(
            isVisible ? QStringLiteral("\xF0\x9F\x91\x81")    // Ã°Å¸â€˜Â
                      : QStringLiteral("\xE2\x80\x94"));       // Ã¢â‚¬â€
        eyeBtn->setFixedSize(32, 32);
        eyeBtn->setFocusPolicy(Qt::NoFocus);
        eyeBtn->setToolTip("Toggle visibility");
        eyeBtn->setAttribute(Qt::WA_TransparentForMouseEvents, false);
        eyeBtn->setStyleSheet(
            isVisible
            ? QStringLiteral("QPushButton { background: transparent; border: none; font-size: 18px; "
              "color: %1; padding: 0; margin: 0; }"
              "QPushButton:hover { color: %2; background: rgba(255,255,255,0.1); border-radius: 3px; }")
              .arg(Theme::hex(Theme::colors().textSecondary), Theme::hex(Theme::colors().textBright))
            : QStringLiteral("QPushButton { background: transparent; border: none; font-size: 16px; "
              "color: %1; padding: 0; margin: 0; }"
              "QPushButton:hover { color: %2; background: rgba(255,255,255,0.05); border-radius: 3px; }")
              .arg(Theme::hex(Theme::colors().border), Theme::hex(Theme::colors().textDisabled))
        );

        int layerIdx = static_cast<int>(li);
        connect(eyeBtn, &QPushButton::clicked, this, [this, layerIdx]() {
            if (layerIdx < 0 || layerIdx >= m_currentShot.layerCount()) return;
            pushUndoState();
            const auto& lref = m_currentShot.layerOrder()[static_cast<size_t>(layerIdx)];
            if (lref.type == LayerType::Character) {
                auto* ch = m_currentShot.character(lref.index);
                if (ch) {
                    ch->visible = !ch->visible;
                    if (layerIdx == m_selectedLayer && m_visibleCheck)
                        m_visibleCheck->setChecked(ch->visible);
                }
            } else {
                auto* bg = m_currentShot.background(lref.index);
                if (bg) bg->visible = !bg->visible;
            }
            refreshLayerList();
            updatePreview();
            emit shotChanged();
        });
        rowLayout->addWidget(eyeBtn);

        // Thin vertical separator
        auto* sep = new QFrame;
        sep->setFrameShape(QFrame::VLine);
        sep->setFixedWidth(1);
        sep->setFixedHeight(32);
        sep->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::colors().border)));
        sep->setAttribute(Qt::WA_TransparentForMouseEvents);
        rowLayout->addSpacing(3);
        rowLayout->addWidget(sep);
        rowLayout->addSpacing(5);

        // Layer type icon
        auto* iconLabel = new QLabel(typeIcon);
        iconLabel->setFixedWidth(22);
        iconLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        iconLabel->setStyleSheet("QLabel { font-size: 18px; padding: 0; }");
        rowLayout->addWidget(iconLabel);
        rowLayout->addSpacing(m.spacingXs);

        // Layer name
        auto* nameLabel = new QLabel(label);
        nameLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        QString nameColor = isVisible ? Theme::hex(Theme::colors().textPrimary)
                                       : Theme::hex(Theme::colors().textDisabled);
        nameLabel->setStyleSheet(
            QStringLiteral("QLabel { color: %1; font-size: 14px; font-weight: 500; }")
            .arg(nameColor));
        rowLayout->addWidget(nameLabel, 1);

        // Opacity indicator (if not 100%)
        if (opacity < 0.99f) {
            auto* opLabel = new QLabel(QString("%1%").arg(static_cast<int>(opacity * 100)));
            opLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
            opLabel->setStyleSheet(QStringLiteral("QLabel { color: %1; font-size: 13px; padding-right: 6px; }")
                .arg(Theme::hex(Theme::colors().textDisabled)));
            opLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            rowLayout->addWidget(opLabel);
        }

        m_layerList->setItemWidget(item, rowWidget);
    }

    if (m_selectedLayer >= 0 && m_selectedLayer < m_layerList->count())
        m_layerList->setCurrentRow(m_selectedLayer);
    m_layerList->blockSignals(false);

    // Ã¢â€â‚¬Ã¢â€â‚¬ Refresh default-shot character dropdown Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    if (m_defaultCharCombo) {
        QString prev = m_defaultCharCombo->currentText();
        m_defaultCharCombo->blockSignals(true);
        m_defaultCharCombo->clear();
        for (const auto& ch : m_currentShot.characters()) {
            // Show display name in combo
            std::string dn = m_modelManager
                ? m_modelManager->getDisplayName(ch.characterName)
                : ch.characterName;
            m_defaultCharCombo->addItem(QString::fromStdString(dn));
        }
        // Restore previous selection if still present
        int prevIdx = m_defaultCharCombo->findText(prev);
        if (prevIdx >= 0)
            m_defaultCharCombo->setCurrentIndex(prevIdx);
        m_defaultCharCombo->blockSignals(false);
        bool hasChars = m_defaultCharCombo->count() > 0;
        m_defaultCharCombo->setEnabled(hasChars && !m_currentShot.name().empty());
        m_setDefaultBtn->setEnabled(hasChars && !m_currentShot.name().empty());
    }
}

// Ã¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢Â
// Thumbnail helpers
// Ã¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢Â

void ShotComposer::setLibraryIconSize(int sz)
{
    m_iconSize = sz;
    QSize icoSz(sz, sz);
    QSize gridSz(sz + 8, sz + 22);

    for (auto* list : {m_characterLibrary, m_backgroundLibrary, m_videoLibrary}) {
        if (!list) continue;
        list->setIconSize(icoSz);
        list->setGridSize(gridSz);
    }

    // Regenerate thumbnails at the new size
    refreshCharacterLibrary();
    refreshBackgroundLibrary();
    refreshVideoLibrary();
}


} // namespace rt
