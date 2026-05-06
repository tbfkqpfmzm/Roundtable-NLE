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
    m_shotCombo->blockSignals(true);
    m_shotList->clear();
    m_shotCombo->clear();
    auto names = m_presetManager.presetNames();
    std::sort(names.begin(), names.end());

    // Build set of shot names that are currently set as a default
    std::set<std::string> defaultShotNames;
    {
        // If a character filter is active, only consider the default for that character;
        // otherwise ("ALL"), consider defaults for every character.
        QString charFilter;
        if (m_charFilterList && m_charFilterList->currentRow() > 0) {
            auto* curItem = m_charFilterList->currentItem();
            if (curItem)
                charFilter = curItem->data(Qt::UserRole).toString();
        }
        for (const auto& [ch, shotName] : m_characterDefaults) {
            if (charFilter.isEmpty() ||
                QString::fromStdString(ch).toLower() == charFilter.toLower())
                defaultShotNames.insert(shotName);
        }
    }

    // Sort default shots to the top
    std::stable_partition(names.begin(), names.end(),
        [&](const std::string& n) {
            return defaultShotNames.count(n) > 0;
        });

    // Get filter criteria
    QString searchFilter;
    QString charFilter;
    if (m_shotSearchEdit)
        searchFilter = m_shotSearchEdit->text().trimmed().toLower();
    // Use the character filter thumbnail list (row 0 = "ALL")
    if (m_charFilterList && m_charFilterList->currentRow() > 0) {
        auto* curItem = m_charFilterList->currentItem();
        if (curItem)
            charFilter = curItem->data(Qt::UserRole).toString();
    }

    constexpr int kThumbW = 100;
    constexpr int kThumbH = 56;

    // Collect unique character names across all shots (for the filter combo)
    QSet<QString> allCharNames;

    int selectRow = -1;
    int visibleRow = 0;
    for (size_t i = 0; i < names.size(); ++i) {
        const auto& n = names[i];

        // Collect characters from this preset for the filter combo
        auto preset = m_presetManager.load(n);
        if (preset) {
            for (const auto& ch : preset->characters()) {
                // Use display name for UI
                std::string dn = m_modelManager
                    ? m_modelManager->getDisplayName(ch.characterName)
                    : ch.characterName;
                allCharNames.insert(QString::fromStdString(dn));
            }
        }

        // Apply name search filter
        if (!searchFilter.isEmpty()) {
            QString qn = QString::fromStdString(n).toLower();
            if (!qn.contains(searchFilter))
                continue;
        }

        // Apply character filter — only show shots that contain the selected character
        if (!charFilter.isEmpty() && preset) {
            bool hasChar = false;
            for (const auto& ch : preset->characters()) {
                std::string dn = m_modelManager
                    ? m_modelManager->getDisplayName(ch.characterName)
                    : ch.characterName;
                if (QString::fromStdString(dn).compare(charFilter, Qt::CaseInsensitive) == 0) {
                    hasChar = true;
                    break;
                }
            }
            if (!hasChar)
                continue;
        }

        // Determine if this is a default shot (set via SET DEFAULT button)
        bool isDefault = defaultShotNames.count(n) > 0;

        QPixmap thumb;
        // Try to load a persisted thumbnail from disk first
        QString cachedPath = shotThumbnailPath(n);
        if (!cachedPath.isEmpty() && QFileInfo::exists(cachedPath)) {
            thumb.load(cachedPath);
        }
        // Fall back to generating one on the fly
        if (thumb.isNull()) {
            if (preset)
                thumb = makeShotThumbnail(*preset, kThumbW, kThumbH);
            else
                thumb = QPixmap(kThumbW, kThumbH);
        }

        // Display text: append gold star for default shots
        QString displayName = QString::fromStdString(n);
        if (isDefault)
            displayName += QStringLiteral("  \u2B50");   // Ã¢Â­Â

        auto* item = new QListWidgetItem(QIcon(thumb), displayName);
        item->setData(Qt::UserRole, QString::fromStdString(n));  // real name without star
        item->setSizeHint(QSize(kThumbW + 8, kThumbH + 20));
        if (isDefault)
            item->setForeground(QColor(255, 200, 50));  // gold tint for default items
        m_shotList->addItem(item);

        // Also populate the dropdown combo (use real name for combo)
        m_shotCombo->addItem(QString::fromStdString(n), QString::fromStdString(n));

        if (!m_currentShot.name().empty() && n == m_currentShot.name())
            selectRow = visibleRow;
        ++visibleRow;
    }

    if (selectRow >= 0) {
        m_shotList->setCurrentRow(selectRow);
        m_shotCombo->setCurrentIndex(selectRow);
    }
    m_shotList->blockSignals(false);
    m_shotCombo->blockSignals(false);

    // Update the character filter thumbnail column
    if (m_charFilterList) {
        // Only show characters that actually exist as either a Spine model
        // on disk OR a known video character.  Filter out stale preset
        // references to characters that no longer exist, but keep video
        // characters (e.g. Wells) that have no Spine folder.
        QSet<QString> validNames;
#ifdef ROUNDTABLE_HAS_SPINE
        if (m_modelManager && m_modelManager->isScanned()) {
            for (const auto& name : m_modelManager->characterDisplayNames())
                validNames.insert(QString::fromStdString(name));
        }
#endif
        // Add video characters from the hardcoded lookup table.  Each
        // entry's charName is the canonical display name.
        for (const auto& [filename, info] : videoCharacterFiles()) {
            (void)filename;
            validNames.insert(QString::fromStdString(info.charName));
        }
        // Intersect: keep only names that appear in both the preset-derived
        // allCharNames AND the validity set.  This drops stale preset chars
        // while preserving real video characters.
        QSet<QString> filtered;
        for (const auto& n : allCharNames)
            if (validNames.contains(n)) filtered.insert(n);
        allCharNames = filtered;

        // Preserve current selection
        QString prevFilter;
        if (auto* cur = m_charFilterList->currentItem())
            prevFilter = cur->data(Qt::UserRole).toString();

        m_charFilterList->blockSignals(true);
        m_charFilterList->clear();

        // "ALL" item at top
        constexpr int kFilterThumbSz = 90;
        auto* allItem = new QListWidgetItem(QStringLiteral("ALL"));
        allItem->setData(Qt::UserRole, QString());
        allItem->setTextAlignment(Qt::AlignHCenter | Qt::AlignBottom);
        allItem->setSizeHint(QSize(kFilterThumbSz + 8, 32));
        m_charFilterList->addItem(allItem);

        QStringList charNames = allCharNames.values();
        charNames.sort();
        int restoreRow = 0;
        for (const auto& cn : charNames) {
            // Resolve folder name for thumbnail generation (filesystem)
            std::string folderName = m_modelManager
                ? m_modelManager->getFolderName(cn.toStdString())
                : cn.toStdString();
            QPixmap thumb = makeCharacterThumbnail(folderName, kFilterThumbSz);
            auto* item = new QListWidgetItem(QIcon(thumb), cn);
            item->setData(Qt::UserRole, cn);
            item->setSizeHint(QSize(kFilterThumbSz + 8, kFilterThumbSz + 22));
            item->setToolTip(cn);
            m_charFilterList->addItem(item);

            if (cn == prevFilter)
                restoreRow = m_charFilterList->count() - 1;
        }

        m_charFilterList->setCurrentRow(restoreRow);
        m_charFilterList->blockSignals(false);
    }

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
