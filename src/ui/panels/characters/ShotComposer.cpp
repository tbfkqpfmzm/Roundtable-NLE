/*
 * ShotComposer.cpp â€” character shot composition panel.
 * Step 18: Shot Composer â€” matched to Python ShotPanel layout.
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

#ifdef ROUNDTABLE_HAS_FFMPEG
#include "media/VideoDecoder.h"
extern "C" {
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}
#endif

#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QMimeData>
#include <QPainterPath>
#include <QPixmap>
#include <QProcess>
#include <QResizeEvent>
#include <QScrollArea>
#include <QShortcut>
#include <QSlider>
#include <QStandardPaths>
#include <QStyledItemDelegate>
#include <QTabWidget>
#include <QJsonDocument>
#include <QJsonObject>
#include <QToolTip>

#ifdef _WIN32
#include <windows.h>
#endif

#include <map>
#include <set>
#include <tuple>

namespace rt {
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

ShotComposer::ShotComposer(QWidget* parent)
    : QWidget(parent)
{
    // Undo coalesce timer â€” resets after 600ms of no property changes
    m_undoCoalesceTimer = new QTimer(this);
    m_undoCoalesceTimer->setSingleShot(true);
    m_undoCoalesceTimer->setInterval(600);
    connect(m_undoCoalesceTimer, &QTimer::timeout, this, [this]() {
        m_undoPropertyPushed = false;
    });

    setupUI();
}

ShotComposer::~ShotComposer() = default;

QSize ShotComposer::sizeHint() const
{
    return {900, 600};
}

void ShotComposer::updateDropIndicatorLine()
{
    if (!m_dropIndicatorLine || !m_layerList) return;
    if (m_dropIndicatorIndex < 0) {
        m_dropIndicatorLine->hide();
        return;
    }
    int count = m_layerList->count();
    int y = 0;
    if (count == 0) {
        y = 0;
    } else if (m_dropIndicatorIndex < count) {
        auto* item = m_layerList->item(m_dropIndicatorIndex);
        QRect r = m_layerList->visualItemRect(item);
        y = r.top() - 1;
    } else {
        auto* item = m_layerList->item(count - 1);
        QRect r = m_layerList->visualItemRect(item);
        y = r.bottom();
    }
    m_dropIndicatorLine->setGeometry(0, y, m_layerList->viewport()->width(), 3);
    m_dropIndicatorLine->show();
    m_dropIndicatorLine->raise();
}

bool ShotComposer::eventFilter(QObject* obj, QEvent* event)
{
    static constexpr const char* kAssetMime = "application/x-roundtable-asset";

    // ── Drag-and-drop from asset library onto layer list or preview ──────
    bool isLayerList = m_layerList &&
                       (obj == m_layerList || obj == m_layerList->viewport());
    bool isPreview = false;
#ifdef ROUNDTABLE_HAS_SPINE
    if (obj == m_spinePreview) isPreview = true;
#endif
    bool isDropTarget = isLayerList || isPreview;

    if (isDropTarget) {
        if (event->type() == QEvent::DragEnter) {
            auto* de = static_cast<QDragEnterEvent*>(event);
            if (de->mimeData()->hasFormat(QString::fromLatin1(kAssetMime))) {
                // Build drag thumbnail for compose preview overlay
                if (m_dragThumb.isNull()) {
                    QByteArray payload = de->mimeData()->data(QString::fromLatin1(kAssetMime));
                    QDataStream ds(&payload, QIODevice::ReadOnly);
                    QString assetType;
                    ds >> assetType;
                    if (assetType == QStringLiteral("character") ||
                        assetType == QStringLiteral("videoChar")) {
                        QString name;
                        ds >> name;
                        m_dragThumb = makeCharacterThumbnail(name.toStdString(), 120);
                    } else if (assetType == QStringLiteral("background")) {
                        QString bgName;
                        ds >> bgName;
                        // Use the background library icon if available
                        for (int i = 0; i < m_backgroundLibrary->count(); ++i) {
                            auto* item = m_backgroundLibrary->item(i);
                            if (item->text() == bgName) {
                                m_dragThumb = item->icon().pixmap(120, 120);
                                break;
                            }
                        }
                    } else if (assetType == QStringLiteral("video")) {
                        QString filename;
                        ds >> filename;
                        for (int i = 0; i < m_videoLibrary->count(); ++i) {
                            auto* item = m_videoLibrary->item(i);
                            if (item->text() == filename) {
                                m_dragThumb = item->icon().pixmap(120, 120);
                                break;
                            }
                        }
                    }
                }
                de->acceptProposedAction();
                return true;
            }
        }
        if (event->type() == QEvent::DragMove) {
            auto* de = static_cast<QDragMoveEvent*>(event);
            if (de->mimeData()->hasFormat(QString::fromLatin1(kAssetMime))) {
                if (isLayerList) {
                    // Compute drop indicator position between layer rows
                    QPoint pos = de->position().toPoint();
                    int count = m_layerList->count();
                    int insertIdx = count; // default: append at end
                    for (int i = 0; i < count; ++i) {
                        auto* item = m_layerList->item(i);
                        QRect r = m_layerList->visualItemRect(item);
                        if (pos.y() < r.top() + r.height() / 2) {
                            insertIdx = i;
                            break;
                        }
                    }
                    if (m_dropIndicatorIndex != insertIdx) {
                        m_dropIndicatorIndex = insertIdx;
                        updateDropIndicatorLine();
                    }
                }
#ifdef ROUNDTABLE_HAS_SPINE
                if (isPreview) {
                    m_dragOverPreview = true;
                    m_dragPreviewPos = de->position().toPoint();
                    if (!m_dragThumb.isNull())
                        m_spinePreview->setDragOverlay(m_dragThumb, m_dragPreviewPos);
                }
#endif
                de->acceptProposedAction();
                return true;
            }
        }
        if (event->type() == QEvent::DragLeave) {
            if (isLayerList && m_dropIndicatorIndex >= 0) {
                m_dropIndicatorIndex = -1;
                updateDropIndicatorLine();
            }
#ifdef ROUNDTABLE_HAS_SPINE
            if (isPreview && m_dragOverPreview) {
                m_dragOverPreview = false;
                m_dragThumb = QPixmap();
                m_spinePreview->clearDragOverlay();
            }
#endif
            return false;
        }
        if (event->type() == QEvent::Drop) {
            auto* de = static_cast<QDropEvent*>(event);
            QByteArray payload = de->mimeData()->data(QString::fromLatin1(kAssetMime));
            if (!payload.isEmpty()) {
                QDataStream ds(&payload, QIODevice::ReadOnly);
                QString assetType;
                ds >> assetType;

                // Capture insertion index before adding (layer count may change)
                int insertAt = m_dropIndicatorIndex;

                int newLayerIdx = -1;
                if (assetType == QStringLiteral("character")) {
                    QString folderName;
                    ds >> folderName;
                    addCharacter(folderName.toStdString());
                    newLayerIdx = m_currentShot.layerCount() - 1;
                } else if (assetType == QStringLiteral("videoChar")) {
                    QString charName, mutePath, talkPath;
                    ds >> charName >> mutePath >> talkPath;
                    addCharacter(charName.toStdString(),
                                 mutePath.toStdString(),
                                 talkPath.toStdString());
                    newLayerIdx = m_currentShot.layerCount() - 1;
                } else if (assetType == QStringLiteral("background")) {
                    QString bgName;
                    ds >> bgName;
                    addBackground(bgName.toStdString());
                    newLayerIdx = m_currentShot.layerCount() - 1;
                } else if (assetType == QStringLiteral("video")) {
                    QString filename;
                    ds >> filename;
                    addVideoLayer(filename.toStdString());
                    newLayerIdx = m_currentShot.layerCount() - 1;
                }

                // Move to insertion position if dropped on layer list
                if (isLayerList && insertAt >= 0 && newLayerIdx >= 0 &&
                    insertAt != newLayerIdx) {
                    m_currentShot.moveLayerTo(newLayerIdx, insertAt);
                    refreshLayerList();
                    selectLayer(insertAt);
                }

                // Clear drag state
                m_dropIndicatorIndex = -1;
                m_dragOverPreview = false;
                m_dragThumb = QPixmap();
                updateDropIndicatorLine();
#ifdef ROUNDTABLE_HAS_SPINE
                if (m_spinePreview) m_spinePreview->clearDragOverlay();
#endif

                de->acceptProposedAction();
                return true;
            }
        }
    }

    // Intercept Ctrl+S on the preview widget so it saves the shot
    // instead of triggering MainWindow's project-save action.
    if (event->type() == QEvent::ShortcutOverride) {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (ke->matches(QKeySequence::Save) ||
            ke->matches(QKeySequence::Copy) ||
            ke->matches(QKeySequence::Paste)) {
            event->accept();
            return true;
        }
        // Ctrl+Shift+C / Ctrl+Shift+V for transform copy-paste
        if ((ke->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier)) &&
            (ke->key() == Qt::Key_C || ke->key() == Qt::Key_V)) {
            event->accept();
            return true;
        }
    }
    if (event->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (ke->matches(QKeySequence::Save)) {
            if (saveCurrentShot())
                QToolTip::showText(mapToGlobal(QPoint(width() / 2, 10)),
                                   "Shot saved", this, {}, 1500);
            return true;
        }
        // Ctrl+Shift+C/V â€” transform copy-paste (check before plain Copy/Paste)
        if (ke->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier)) {
            if (ke->key() == Qt::Key_C) {
                copyTransform();
                return true;
            }
            if (ke->key() == Qt::Key_V) {
                pasteTransform();
                return true;
            }
        }
        // Copy/Paste â€” the shot list and char filter list are reparented
        // outside ShotComposer, so QShortcuts with WidgetWithChildrenShortcut
        // don't reach them.  Handle via eventFilter instead.
        if (ke->matches(QKeySequence::Copy)) {
            copySelectedLayer();
            return true;
        }
        if (ke->matches(QKeySequence::Paste)) {
            pasteLayer();
            return true;
        }
    }
    return QWidget::eventFilter(obj, event);
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// Configuration
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

void ShotComposer::setModelManager(ModelManager* mgr)
{
    m_modelManager = mgr;
    refreshCharacterLibrary();
}

void ShotComposer::setAnimVideoCache(const AnimationVideoCache* cache)
{
    m_animVideoCache = cache;
    refreshCharacterLibrary();
}

void ShotComposer::setPresetsDirectory(const std::filesystem::path& dir)
{
    m_presetManager.scan(dir);
    loadDefaults();
    refreshShotList();
    refreshBackgroundLibrary();
    refreshVideoLibrary();
}

void ShotComposer::saveDefaults() const
{
    auto dir = m_presetManager.directory();
    if (dir.empty()) return;
    auto path = dir / "_defaults.json";
    QJsonObject obj;
    for (const auto& [charName, shotName] : m_characterDefaults)
        obj[QString::fromStdString(charName)] = QString::fromStdString(shotName);
    QJsonDocument doc(obj);
    std::ofstream f(path, std::ios::trunc);
    if (f.is_open()) {
        auto bytes = doc.toJson(QJsonDocument::Compact);
        f.write(bytes.constData(), bytes.size());
    }
}

void ShotComposer::loadDefaults()
{
    auto dir = m_presetManager.directory();
    if (dir.empty()) return;
    auto path = dir / "_defaults.json";
    std::ifstream f(path);
    if (!f.is_open()) return;
    std::string contents((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());
    auto doc = QJsonDocument::fromJson(QByteArray::fromStdString(contents));
    if (!doc.isObject()) return;
    m_characterDefaults.clear();
    auto obj = doc.object();
    for (auto it = obj.begin(); it != obj.end(); ++it)
        m_characterDefaults[it.key().toStdString()] = it.value().toString().toStdString();
}

void ShotComposer::ensureDefaultShotsForCharacters(const QStringList& characters)
{
    bool added = false;
    for (const auto& charName : characters) {
        std::string name = charName.toStdString();
        // Check if a default shot already exists for this character
        std::string defaultName = name + " (Default)";
        if (m_presetManager.hasPreset(defaultName))
            continue;

        // Also check upper-case variant
        std::string upperName = charName.toUpper().toStdString() + " (Default)";
        if (m_presetManager.hasPreset(upperName))
            continue;

        // Create a default shot preset for this character
        auto preset = ShotPreset::createDefault(name);
        preset.setName(defaultName);
        if (m_presetManager.save(preset)) {
            spdlog::info("ShotComposer: Created default shot for '{}'", name);
            added = true;
        }
    }
    if (added)
        refreshShotList();
}

QString ShotComposer::activeCharFilter() const
{
    // The character filter list now contains ALL (item 0, empty UserRole),
    // UNASSIGNED (item 1, "__UNASSIGNED__"), and character entries.
    if (m_charFilterList) {
        auto* curItem = m_charFilterList->currentItem();
        if (curItem) {
            QString val = curItem->data(Qt::UserRole).toString();
            if (!val.isEmpty())
                return val;
        }
    }
    return {};
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// Shot management
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

void ShotComposer::newShot()
{
    bool ok = false;
    QString name = QInputDialog::getText(this, "New Shot", "Shot Name:",
                                         QLineEdit::Normal, "", &ok);
    if (!ok || name.trimmed().isEmpty()) return;
    newShot(name.trimmed());
}

void ShotComposer::newShot(const QString& name)
{
    m_currentShot = ShotPreset(name.toStdString());
    m_selectedLayer = -1;
    m_lastSavedName = name.toStdString();

    // If a specific character filter is active, auto-add that character
    QString filterVal = activeCharFilter();
    if (!filterVal.isEmpty() && filterVal != QStringLiteral("__UNASSIGNED__")) {
        std::string folderName = m_modelManager
            ? m_modelManager->getFolderName(filterVal.toStdString())
            : filterVal.toStdString();

        CharacterState ch;
        ch.characterName = folderName;
        ch.outfit    = "default";
        ch.animation = "idle";
        ch.isTalking = true;
        ch.posX      = 0.5f;
        ch.posY      = 0.75f;
        ch.scale     = 1.0f;

        // Check for video character (e.g. "Wells")
        {
            QString qFolder  = QString::fromStdString(folderName).toLower();
            QString qDisplay = filterVal.toLower();
            for (const auto& [fn, info] : videoCharacterFiles()) {
                if (QString::fromStdString(fn).toLower() == qFolder ||
                    QString::fromStdString(info.charName).toLower() == qDisplay) {
                    ch.videoMutePath = info.mutePath;
                    ch.videoTalkPath = info.talkPath;
                    break;
                }
            }
        }

        m_currentShot.addCharacter(ch);
    }

    // Save the new shot immediately so it appears in the preset list
    m_presetManager.save(m_currentShot);

    m_updating = true;
    m_shotNameEdit->setText(name);
    m_shotNameEdit->setEnabled(true);
    m_defaultShotCheck->setEnabled(true);
    m_defaultCharCombo->setEnabled(true);
    m_setDefaultBtn->setEnabled(true);
    m_defaultCharCombo->clear();
    m_updating = false;

    refreshLayerList();
    clearLayerProperties();
    refreshShotList();
    updatePreview();
    emit shotChanged();
}

void ShotComposer::setCurrentShot(const ShotPreset& preset)
{
    m_currentShot = preset;
    m_selectedLayer = -1;
    m_lastSavedName = preset.name();  // Last saved name = this preset's name

    // â”€â”€ Migrate legacy video-character backgrounds â†’ proper CharacterState â”€â”€
    // Old shots stored Wells as a Background with layerType="video".
    // Convert them to CharacterState entries so they get character properties.
    {
        const auto& vcFiles = videoCharacterFiles();
        // Collect indices to migrate (reverse order so removal doesn't shift)
        struct MigrationInfo {
            int bgIndex;
            std::string charName, mutePath, talkPath;
            BackgroundState bg;
        };
        std::vector<MigrationInfo> migrations;

        for (int i = 0; i < m_currentShot.backgroundCount(); ++i) {
            const auto* bg = m_currentShot.background(i);
            if (!bg || !bg->isVideo()) continue;

            // Extract filename from path and lowercase it
            QString qpath = QString::fromStdString(bg->path);
            std::string filename = QFileInfo(qpath).fileName().toLower().toStdString();
            auto it = vcFiles.find(filename);
            if (it != vcFiles.end()) {
                const auto& [charName, mutePath, talkPath] = it->second;
                migrations.push_back({i, charName, mutePath, talkPath, *bg});
            }
        }

        // Apply migrations (reverse order to preserve indices)
        for (auto rit = migrations.rbegin(); rit != migrations.rend(); ++rit) {
            // Check if this video character was already added
            bool alreadyExists = false;
            for (const auto& ch : m_currentShot.characters()) {
                if (ch.characterName == rit->charName && ch.isVideoCharacter()) {
                    alreadyExists = true;
                    break;
                }
            }

            // Create character from the old background
            CharacterState ch;
            ch.characterName = rit->charName;
            ch.videoMutePath = rit->mutePath;
            ch.videoTalkPath = rit->talkPath;
            ch.posX      = rit->bg.posX;
            ch.posY      = rit->bg.posY;
            ch.scale     = rit->bg.scale;
            ch.opacity   = rit->bg.opacity;
            ch.visible   = rit->bg.visible;
            ch.isTalking = (rit->bg.path.find("TALK") != std::string::npos);

            if (!alreadyExists) {
                // Remove the old background first, then add as character
                // We need to update layerOrder: find the bg's position, remove it,
                // then insert the new character at the same z-position
                int layerPos = m_currentShot.findLayerIndex(
                    {LayerType::Background, rit->bgIndex});
                m_currentShot.removeBackground(rit->bgIndex);
                int chIdx = m_currentShot.addCharacter(ch);
                // Move the new character to the same visual position
                if (layerPos >= 0) {
                    // addCharacter inserts at front (index 0), move it to layerPos
                    int newLayerIdx = m_currentShot.findLayerIndex(
                        {LayerType::Character, chIdx});
                    // Shift it to the right position via layer moves
                    while (newLayerIdx >= 0 && newLayerIdx < layerPos &&
                           newLayerIdx + 1 < m_currentShot.layerCount()) {
                        m_currentShot.moveLayerDown(newLayerIdx);
                        newLayerIdx++;
                    }
                }
                spdlog::info("ShotComposer: Migrated '{}' from video background to video character",
                             rit->charName);
            } else {
                // Just remove the duplicate background
                m_currentShot.removeBackground(rit->bgIndex);
                spdlog::info("ShotComposer: Removed duplicate video bg for '{}'",
                             rit->charName);
            }
        }
    }

    // Sanitize layer order after migration â€” remove any stale references
    // that could cause out-of-bounds access in updatePreview
    m_currentShot.ensureLayerOrder();

    m_updating = true;
    m_shotNameEdit->setText(QString::fromStdString(preset.name()));
    m_shotNameEdit->setEnabled(true);
    m_defaultShotCheck->setEnabled(true);
    m_defaultCharCombo->setEnabled(true);
    m_setDefaultBtn->setEnabled(true);

    // Populate default-shot character dropdown with characters in this shot
    m_defaultCharCombo->clear();
    for (const auto& ch : preset.characters()) {
        m_defaultCharCombo->addItem(QString::fromStdString(ch.characterName));
    }

    m_cameraZoomSpin->setValue(preset.cameraZoom() * 100.0);
    m_cameraPanXSpin->setValue(static_cast<double>(preset.cameraX()) * 100.0);
    m_cameraPanYSpin->setValue(static_cast<double>(preset.cameraY()) * 100.0);
    m_updating = false;

    // Apply camera transform now (spins were set with m_updating=true so
    // onCameraPropertyChanged didn't fire)
#ifdef ROUNDTABLE_HAS_SPINE
    if (m_spinePreview)
        m_spinePreview->setCameraTransform(
            preset.cameraZoom(),
            preset.cameraX(),
            preset.cameraY());
#endif

    refreshLayerList();

    // Auto-select the first character layer (or first layer) for preview
    if (m_currentShot.layerCount() > 0) {
        int firstCharLayer = -1;
        for (int i = 0; i < m_currentShot.layerCount(); ++i) {
            const auto& ref = m_currentShot.layerOrder()[static_cast<size_t>(i)];
            if (ref.type == LayerType::Character) {
                firstCharLayer = i;
                break;
            }
        }
        int autoSelect = (firstCharLayer >= 0) ? firstCharLayer : 0;
        selectLayer(autoSelect);
    } else {
        clearLayerProperties();
        updatePreview();
    }

    emit shotChanged();
}

bool ShotComposer::saveCurrentShot()
{
    if (m_currentShot.name().empty())
        return false;

    // ── Clean up orphaned file on rename ────────────────────────────────
    // If the shot was previously saved under a different name, delete the
    // old file so it doesn't linger on disk and confuse things.
    if (!m_lastSavedName.empty() && m_lastSavedName != m_currentShot.name()) {
        m_presetManager.remove(m_lastSavedName);
    }

    bool ok = m_presetManager.save(m_currentShot);
    if (ok) {
        m_lastSavedName = m_currentShot.name();
        // Generate & persist a thumbnail PNG next to the preset JSON
        saveShotThumbnail(m_currentShot);
        refreshShotList();
        spdlog::info("ShotComposer: Saved shot '{}'", m_currentShot.name());
    }
    return ok;
}

void ShotComposer::duplicateCurrentShot()
{
    if (m_currentShot.name().empty())
        return;

    // Generate a unique name
    std::string baseName = m_currentShot.name() + " Copy";
    std::string newName  = baseName;
    int counter = 2;
    while (m_presetManager.hasPreset(newName)) {
        newName = baseName + " " + std::to_string(counter++);
    }

    bool ok = false;
    QString name = QInputDialog::getText(this, "Duplicate Shot",
        "Name for the duplicate:", QLineEdit::Normal,
        QString::fromStdString(newName), &ok);
    if (!ok || name.trimmed().isEmpty())
        return;

    ShotPreset dupe = m_currentShot;
    dupe.setName(name.trimmed().toStdString());
    m_presetManager.save(dupe);
    setCurrentShot(dupe);
    refreshShotList();

    spdlog::info("ShotComposer: Duplicated shot '{}' as '{}'",
        m_currentShot.name(), dupe.name());
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// Character / background management
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

int ShotComposer::addCharacter(const std::string& characterName,
                               const std::string& videoMutePath,
                               const std::string& videoTalkPath)
{
    pushUndoState();
    CharacterState ch;
    ch.characterName = characterName;
    ch.outfit    = "default";
    ch.posX      = 0.5f;
    ch.posY      = 0.75f;
    ch.scale     = 1.0f;
    ch.animation = "idle";
    ch.isTalking = true;
    ch.videoMutePath = videoMutePath;
    ch.videoTalkPath = videoTalkPath;

    int idx = m_currentShot.addCharacter(ch);
    if (!m_shotNameEdit->isEnabled())
        m_shotNameEdit->setEnabled(true);
    refreshLayerList();

    int layerIdx = m_currentShot.findLayerIndex({LayerType::Character, idx});
    if (layerIdx >= 0)
        selectLayer(layerIdx);

    emit shotChanged();
    return idx;
}

bool ShotComposer::removeCharacter(int index)
{
    if (!m_currentShot.removeCharacter(index))
        return false;

    m_selectedLayer = -1;
    refreshLayerList();
    clearLayerProperties();
    updatePreview();
    emit shotChanged();
    return true;
}

int ShotComposer::addBackground(const std::string& path)
{
    pushUndoState();
    BackgroundState bg;
    bg.path  = path;
    bg.posX  = 0.5f;
    bg.posY  = 0.5f;
    bg.scale = 1.0f;

    int idx = m_currentShot.addBackground(bg);
    if (!m_shotNameEdit->isEnabled())
        m_shotNameEdit->setEnabled(true);
    refreshLayerList();

    int layerIdx = m_currentShot.findLayerIndex({LayerType::Background, idx});
    if (layerIdx >= 0)
        selectLayer(layerIdx);

    emit shotChanged();
    return idx;
}

bool ShotComposer::removeBackground(int index)
{
    if (!m_currentShot.removeBackground(index))
        return false;

    m_selectedLayer = -1;
    refreshLayerList();
    clearLayerProperties();
    updatePreview();
    emit shotChanged();
    return true;
}

void ShotComposer::selectLayer(int index)
{
    if (index < 0 || index >= m_currentShot.layerCount()) {
        m_selectedLayer = -1;
        clearLayerProperties();
        return;
    }

    m_selectedLayer = index;
    m_layerList->setCurrentRow(index);
    populateLayerProperties();
#ifdef ROUNDTABLE_HAS_SPINE
    if (m_spinePreview)
        m_spinePreview->setSelectedLayer(index);
#endif
    emit layerSelected(index);
}

// â”€â”€ Layer ordering â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

void ShotComposer::pushUndoState()
{
    m_undoStack.push_back({m_currentShot, m_layerGroups});
    if (static_cast<int>(m_undoStack.size()) > MAX_UNDO)
        m_undoStack.pop_front();
    m_redoStack.clear();  // New action invalidates redo history
}

void ShotComposer::undo()
{
    if (m_undoStack.empty()) return;
    m_redoStack.push_back({m_currentShot, m_layerGroups});  // Save current state for redo
    auto& prev = m_undoStack.back();
    m_currentShot = prev.preset;
    m_layerGroups = prev.groups;
    m_undoStack.pop_back();
    // Clamp selection
    if (m_selectedLayer >= m_currentShot.layerCount())
        m_selectedLayer = m_currentShot.layerCount() - 1;
    refreshLayerList();
    populateLayerProperties();
    updatePreview();
    emit shotChanged();
}

void ShotComposer::redo()
{
    if (m_redoStack.empty()) return;
    m_undoStack.push_back({m_currentShot, m_layerGroups});  // Save current state for undo
    auto& next = m_redoStack.back();
    m_currentShot = next.preset;
    m_layerGroups = next.groups;
    m_redoStack.pop_back();
    // Clamp selection
    if (m_selectedLayer >= m_currentShot.layerCount())
        m_selectedLayer = m_currentShot.layerCount() - 1;
    refreshLayerList();
    populateLayerProperties();
    updatePreview();
    emit shotChanged();
}

void ShotComposer::moveSelectedLayerUp()
{
    if (m_selectedLayer < 0) return;
    spdlog::debug("ShotComposer::moveSelectedLayerUp â€” selected={}", m_selectedLayer);
    pushUndoState();
    if (m_currentShot.moveLayerUp(m_selectedLayer)) {
        --m_selectedLayer;
        spdlog::debug("  â†’ moved to index {}", m_selectedLayer);
        refreshLayerList();
        m_layerList->setCurrentRow(m_selectedLayer);
        updatePreview();
        emit shotChanged();
    } else {
        spdlog::debug("  â†’ moveLayerUp returned false (already at top?)");
    }
}

void ShotComposer::moveSelectedLayerDown()
{
    if (m_selectedLayer < 0) return;
    spdlog::debug("ShotComposer::moveSelectedLayerDown â€” selected={}", m_selectedLayer);
    pushUndoState();
    if (m_currentShot.moveLayerDown(m_selectedLayer)) {
        ++m_selectedLayer;
        spdlog::debug("  â†’ moved to index {}", m_selectedLayer);
        refreshLayerList();
        m_layerList->setCurrentRow(m_selectedLayer);
        updatePreview();
        emit shotChanged();
    } else {
        spdlog::debug("  â†’ moveLayerDown returned false (already at bottom?)");
    }
}

void ShotComposer::moveSelectedLayerToFront()
{
    if (m_selectedLayer < 0) return;
    pushUndoState();
    if (m_currentShot.moveLayerToFront(m_selectedLayer)) {
        m_selectedLayer = 0;
        refreshLayerList();
        m_layerList->setCurrentRow(0);
        updatePreview();
        emit shotChanged();
    }
}

void ShotComposer::moveSelectedLayerToBack()
{
    if (m_selectedLayer < 0) return;
    pushUndoState();
    if (m_currentShot.moveLayerToBack(m_selectedLayer)) {
        m_selectedLayer = m_currentShot.layerCount() - 1;
        refreshLayerList();
        m_layerList->setCurrentRow(m_selectedLayer);
        updatePreview();
        emit shotChanged();
    }
}

// ── Group operations (UI-only — does not affect ShotPreset data model) ────────

void ShotComposer::groupSelectedLayers()
{
    auto sel = m_layerList->selectionModel()->selectedRows();
    if (sel.size() < 1) return;

    pushUndoState();

    // Collect selected rows in order (sorted ascending)
    std::vector<int> rows;
    for (const auto& idx : sel)
        rows.push_back(idx.row());
    std::sort(rows.begin(), rows.end());

    // Build the group info
    LayerGroupInfo grp;
    grp.name = "Group";
    grp.expanded = true;
    grp.firstChild = rows.front();
    grp.lastChild  = rows.back();

    // Give the group a unique name if "Group" is already taken
    int suffix = 2;
    QString baseName = QString::fromStdString(grp.name);
    for (;;) {
        bool conflict = false;
        for (const auto& g : m_layerGroups) {
            if (g.name == grp.name) { conflict = true; break; }
        }
        if (!conflict) break;
        grp.name = QString("Group %1").arg(suffix++).toStdString();
    }

    m_layerGroups.push_back(grp);

    // Update selection to the group (it appears at the group insertion point in the list)
    m_selectedLayer = rows.front();
    refreshLayerList();
    m_layerList->setCurrentRow(m_selectedLayer);
    updatePreview();
    emit shotChanged();

    spdlog::info("ShotComposer: grouped {} layers into '{}'", rows.size(), grp.name);
}

void ShotComposer::ungroupSelectedGroup()
{
    if (m_selectedLayer < 0) return;

    // Find which group the selected layer belongs to
    int grpIdx = -1;
    for (int i = 0; i < static_cast<int>(m_layerGroups.size()); ++i) {
        const auto& g = m_layerGroups[i];
        if (m_selectedLayer >= g.firstChild && m_selectedLayer <= g.lastChild) {
            grpIdx = i;
            break;
        }
    }
    if (grpIdx < 0) return;

    pushUndoState();
    m_layerGroups.erase(m_layerGroups.begin() + grpIdx);
    m_selectedLayer = -1;
    refreshLayerList();
    clearLayerProperties();
    updatePreview();
    emit shotChanged();
}

void ShotComposer::addEmptyGroup()
{
    pushUndoState();
    LayerGroupInfo grp;
    grp.name = "Group";
    grp.expanded = true;
    // Empty group sits at the end of the layer list with no children.
    // We use firstChild = -1 to indicate it's an empty group folder.
    grp.firstChild = -1;
    grp.lastChild  = -1;

    // Make name unique
    int suffix = 2;
    for (;;) {
        bool conflict = false;
        for (const auto& g : m_layerGroups) {
            if (g.name == grp.name) { conflict = true; break; }
        }
        if (!conflict) break;
        grp.name = QString("Group %1").arg(suffix++).toStdString();
    }

    m_layerGroups.push_back(grp);

    // Select the last item in the layer list (or the group if empty shot)
    int selectIdx = m_currentShot.layerCount() > 0 ? m_currentShot.layerCount() - 1 : 0;
    if (selectIdx >= 0) {
        m_selectedLayer = selectIdx;
        refreshLayerList();
        m_layerList->setCurrentRow(m_selectedLayer);
    }
    emit shotChanged();
}

void ShotComposer::copySelectedLayer()
{
    // â”€â”€ Shot-level copy when the shot list has focus â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    if (m_shotList && (m_shotList->hasFocus() ||
        (m_charFilterList && m_charFilterList->hasFocus())) && m_shotList->currentItem()) {
        auto name = m_shotList->currentItem()->data(Qt::UserRole).toString().toStdString();
        auto preset = m_presetManager.load(name);
        if (preset) {
            m_shotClipboard = *preset;
            spdlog::info("ShotComposer: copied shot '{}' to clipboard", name);
            QToolTip::showText(mapToGlobal(QPoint(width() / 2, 10)),
                QString("Shot \"%1\" copied").arg(QString::fromStdString(name)), this, {}, 1200);
        }
        return;
    }

    // â”€â”€ Layer-level copy â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    // Copy all selected layers (multi-select supported)
    const auto rows = m_layerList->selectionModel()->selectedRows();
    if (rows.isEmpty()) return;

    m_layerClipboard.clear();
    for (const auto& idx : rows) {
        int row = idx.row();
        if (row < 0 || row >= m_currentShot.layerCount()) continue;

        const auto& ref = m_currentShot.layerOrder()[row];
        LayerClipboardEntry entry;
        entry.type = ref.type;

        if (ref.type == LayerType::Character) {
            const auto* ch = m_currentShot.character(ref.index);
            if (!ch) continue;
            entry.character = *ch;
        } else {
            const auto* bg = m_currentShot.background(ref.index);
            if (!bg) continue;
            entry.background = *bg;
        }
        m_layerClipboard.push_back(entry);
    }

    spdlog::info("ShotComposer: copied {} layer(s) to clipboard", m_layerClipboard.size());
    QToolTip::showText(mapToGlobal(QPoint(width() / 2, 10)),
        QString("%1 layer(s) copied").arg(m_layerClipboard.size()), this, {}, 1200);
}

void ShotComposer::pasteLayer()
{
    // â”€â”€ Shot-level paste when the shot list or char filter has focus â”€â”€â”€â”€
    if (m_shotClipboard && m_shotList &&
        (m_shotList->hasFocus() ||
         (m_charFilterList && m_charFilterList->hasFocus()) ||
         !m_layerList->hasFocus())) {
        // Determine target character from character filter
        QString charFilter = activeCharFilter();
        // UNASSIGNED filter behaves like ALL for paste purposes
        if (charFilter == QStringLiteral("__UNASSIGNED__"))
            charFilter.clear();

        // Build a new name: replace source character prefix with target character
        std::string srcName = m_shotClipboard->name();
        std::string newName;
        if (!charFilter.isEmpty()) {
            // Try to replace source character prefix with the target character
            // Shot names typically start with "CharName_..."
            auto srcPos = srcName.find('_');
            if (srcPos != std::string::npos)
                newName = charFilter.toStdString() + srcName.substr(srcPos);
            else
                newName = charFilter.toStdString() + "_" + srcName;
        } else {
            newName = srcName + " Copy";
        }

        // Ensure unique name
        std::string baseName = newName;
        int counter = 2;
        while (m_presetManager.hasPreset(newName))
            newName = baseName + " " + std::to_string(counter++);

        bool ok = false;
        QString name = QInputDialog::getText(this, "Paste Shot",
            "Name for the pasted shot:", QLineEdit::Normal,
            QString::fromStdString(newName), &ok);
        if (!ok || name.trimmed().isEmpty()) return;

        ShotPreset pasted = *m_shotClipboard;
        pasted.setName(name.trimmed().toStdString());
        m_presetManager.save(pasted);
        saveShotThumbnail(pasted);
        setCurrentShot(pasted);
        refreshShotList();

        spdlog::info("ShotComposer: pasted shot '{}' as '{}'", srcName, pasted.name());
        QToolTip::showText(mapToGlobal(QPoint(width() / 2, 10)),
            QString("Shot \"%1\" pasted").arg(name.trimmed()), this, {}, 1200);
        return;
    }

    // â”€â”€ Layer-level paste â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    if (m_layerClipboard.empty()) {
        QToolTip::showText(mapToGlobal(QPoint(width() / 2, 10)), "Nothing to paste", this, {}, 1200);
        return;
    }

    pushUndoState();

    int lastIdx = -1;
    for (const auto& entry : m_layerClipboard) {
        if (entry.type == LayerType::Character) {
            lastIdx = m_currentShot.addCharacter(entry.character);
            spdlog::info("ShotComposer: pasted character '{}' as index {}",
                         entry.character.characterName, lastIdx);
        } else {
            lastIdx = m_currentShot.addBackground(entry.background);
            spdlog::info("ShotComposer: pasted background '{}' as index {}",
                         entry.background.path, lastIdx);
        }
    }

    refreshLayerList();
    // Select the last pasted layer
    m_selectedLayer = m_currentShot.layerCount() - 1;
    m_layerList->setCurrentRow(m_selectedLayer);
    populateLayerProperties();
    updatePreview();
    emit shotChanged();
    QToolTip::showText(mapToGlobal(QPoint(width() / 2, 10)),
        QString("%1 layer(s) pasted").arg(m_layerClipboard.size()), this, {}, 1200);
}

// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// copyTransform / pasteTransform â€” Ctrl+Shift+C / Ctrl+Shift+V for transform
//                                  attributes between any layer types
// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

void ShotComposer::copyTransform()
{
    if (m_selectedLayer < 0 || m_selectedLayer >= m_currentShot.layerCount()) return;

    const auto& ref = m_currentShot.layerOrder()[static_cast<size_t>(m_selectedLayer)];
    TransformClipboard tc;

    if (ref.type == LayerType::Character) {
        const auto* ch = m_currentShot.character(ref.index);
        if (!ch) return;
        tc.posX       = ch->posX;
        tc.posY       = ch->posY;
        tc.scale      = ch->scale;
        tc.rotation   = ch->rotation;
        tc.opacity    = ch->opacity;
        tc.flipX      = ch->flipX;
        tc.visible    = ch->visible;
        tc.cropLeft   = ch->cropLeft;
        tc.cropRight  = ch->cropRight;
        tc.cropTop    = ch->cropTop;
        tc.cropBottom = ch->cropBottom;
        tc.blur       = ch->blur;
    } else {
        const auto* bg = m_currentShot.background(ref.index);
        if (!bg) return;
        tc.posX       = bg->posX;
        tc.posY       = bg->posY;
        tc.scale      = bg->scale;
        tc.rotation   = 0.0f;
        tc.opacity    = bg->opacity;
        tc.flipX      = false;
        tc.visible    = bg->visible;
        tc.cropLeft   = bg->cropLeft;
        tc.cropRight  = bg->cropRight;
        tc.cropTop    = bg->cropTop;
        tc.cropBottom = bg->cropBottom;
        tc.blur       = bg->blur;
    }

    m_transformClipboard = tc;
    spdlog::info("ShotComposer: copied transform from layer {}", m_selectedLayer);
    QToolTip::showText(mapToGlobal(QPoint(width() / 2, 10)),
        "Transform copied", this, {}, 1200);
}

void ShotComposer::pasteTransform()
{
    if (!m_transformClipboard) {
        QToolTip::showText(mapToGlobal(QPoint(width() / 2, 10)),
            "No transform to paste", this, {}, 1200);
        return;
    }

    const auto rows = m_layerList->selectionModel()->selectedRows();
    if (rows.isEmpty()) return;

    pushUndoState();
    const auto& tc = *m_transformClipboard;

    for (const auto& idx : rows) {
        int row = idx.row();
        if (row < 0 || row >= m_currentShot.layerCount()) continue;

        const auto& ref = m_currentShot.layerOrder()[static_cast<size_t>(row)];
        if (ref.type == LayerType::Character) {
            auto* ch = m_currentShot.character(ref.index);
            if (!ch) continue;
            ch->posX       = tc.posX;
            ch->posY       = tc.posY;
            ch->scale      = tc.scale;
            ch->rotation   = tc.rotation;
            ch->opacity    = tc.opacity;
            ch->flipX      = tc.flipX;
            ch->visible    = tc.visible;
            ch->cropLeft   = tc.cropLeft;
            ch->cropRight  = tc.cropRight;
            ch->cropTop    = tc.cropTop;
            ch->cropBottom = tc.cropBottom;
            ch->blur       = tc.blur;
        } else {
            auto* bg = m_currentShot.background(ref.index);
            if (!bg) continue;
            bg->posX       = tc.posX;
            bg->posY       = tc.posY;
            bg->scale      = tc.scale;
            bg->opacity    = tc.opacity;
            bg->visible    = tc.visible;
            bg->cropLeft   = tc.cropLeft;
            bg->cropRight  = tc.cropRight;
            bg->cropTop    = tc.cropTop;
            bg->cropBottom = tc.cropBottom;
            bg->blur       = tc.blur;
        }
    }

    refreshLayerList();
    populateLayerProperties();
    updatePreview();
    emit shotChanged();

    int count = static_cast<int>(rows.size());
    spdlog::info("ShotComposer: pasted transform onto {} layer(s)", count);
    QToolTip::showText(mapToGlobal(QPoint(width() / 2, 10)),
        QString("Transform pasted to %1 layer(s)").arg(count), this, {}, 1200);
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// Video thumbnail extraction â€” uses ffmpeg to grab first frame
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•


void ShotComposer::updatePreview()
{
#ifdef ROUNDTABLE_HAS_SPINE
    if (!m_spinePreview || !m_modelManager)
        return;

    m_spinePreview->stopAnimation();
    m_spinePreview->clearBackgroundImage();   // BG is now in the layer stack

    // â”€â”€ Resize engine pools to match character count â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    size_t needed = static_cast<size_t>(m_currentShot.characterCount());
    while (m_layerEngines.size() < needed) {
        m_layerEngines.push_back(std::make_unique<SpineEngine>());
        m_layerTextures.emplace_back();
    }
    if (m_layerEngines.size() > needed) {
        m_layerEngines.resize(needed);
        m_layerTextures.resize(needed);
    }

    // â”€â”€ Build unified layer list â€” backgrounds + characters in z-order â”€â”€
    // layerOrder[0] = FRONT (top of UI list, drawn on top)
    // We iterate back-to-front so previewLayers[0] = BACK, previewLayers[last] = FRONT
    std::vector<PreviewCharLayer> previewLayers;
    int selectedCharIdx = -1;
    std::vector<AnimationInfo> selectedAnims;

    spdlog::debug("ShotComposer::updatePreview â€” {} layers in order:", m_currentShot.layerCount());
    for (int dbg = 0; dbg < m_currentShot.layerCount(); ++dbg) {
        const auto& r = m_currentShot.layerOrder()[static_cast<size_t>(dbg)];
        if (r.type == LayerType::Background) {
            const auto* bg = m_currentShot.background(r.index);
            spdlog::debug("  [{}] BG idx={} path='{}' visible={}",
                dbg, r.index, bg ? bg->path : "?", bg ? bg->visible : false);
        } else {
            const auto* ch = m_currentShot.character(r.index);
            spdlog::debug("  [{}] CH idx={} name='{}' visible={}",
                dbg, r.index, ch ? ch->characterName : "?", ch ? ch->visible : false);
        }
    }

    for (int i = m_currentShot.layerCount() - 1; i >= 0; --i) {
        const auto& ref = m_currentShot.layerOrder()[static_cast<size_t>(i)];

        if (ref.type == LayerType::Background) {
            // â”€â”€ Background / video layer â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
            const auto* bg = m_currentShot.background(ref.index);
            if (!bg) continue;

            PreviewCharLayer layer;
            layer.isBackground = true;
            layer.visible      = bg->visible;
            layer.opacity      = bg->opacity;
            layer.posX         = bg->posX;
            layer.posY         = bg->posY;
            layer.scale        = bg->scale;
            layer.layerIndex   = i;
            layer.cropLeft     = bg->cropLeft;
            layer.cropRight    = bg->cropRight;
            layer.cropTop      = bg->cropTop;
            layer.cropBottom   = bg->cropBottom;
            layer.blur         = bg->blur;

            if (!bg->path.empty()) {
                if (bg->isVideo()) {
                    // Set up looping video playback via VideoDecoder
                    auto player = getOrCreateVideoPlayer(bg->path);
                    if (player) {
                        // Set initial frame
                        if (!player->lastFrame.isNull())
                            layer.backgroundImage = player->lastFrame;
                        else {
                            // Fall back to thumbnail for initial display
                            QImage thumb = extractVideoThumbnail(bg->path);
                            if (!thumb.isNull())
                                layer.backgroundImage = thumb;
                        }
                        // Install video frame provider callback (looping)
                        layer.videoFrameProvider = [player](float vdt) -> QImage {
                            return ShotComposer::advanceVideoPlayer(player, vdt);
                        };
                    } else {
                        // Fallback to static thumbnail
                        QImage frame = extractVideoThumbnail(bg->path);
                        if (!frame.isNull())
                            layer.backgroundImage = frame;
                    }
                } else {
                    // Static background image â€” use cache to avoid disk I/O every frame
                    auto cacheIt = m_bgImageCache.find(bg->path);
                    if (cacheIt != m_bgImageCache.end()) {
                        layer.backgroundImage = cacheIt->second;
                    } else {
                        QImage bgImg(QString::fromStdString(bg->path));
                        if (bgImg.isNull()) {
                            bgImg = QImage(QString("assets/backgrounds/%1")
                                .arg(QString::fromStdString(bg->path)));
                        }
                        if (bgImg.isNull()) {
                            // Try from app root
                            QString altPath = QApplication::applicationDirPath()
                                + "/../../../" + QString::fromStdString(bg->path);
                            if (QFileInfo::exists(altPath))
                                bgImg = QImage(altPath);
                        }
                        if (!bgImg.isNull()) {
                            bgImg = bgImg.convertToFormat(QImage::Format_ARGB32);
                            m_bgImageCache[bg->path] = bgImg;
                            layer.backgroundImage = bgImg;
                        }
                    }
                }
            }

            previewLayers.push_back(std::move(layer));
            continue;
        }

        // â”€â”€ Character (Spine) layer â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        const auto* ch = m_currentShot.character(ref.index);
        if (!ch) continue;

        if (i == m_selectedLayer)
            selectedCharIdx = ref.index;

        // â”€â”€ Video character (e.g. Wells) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        if (ch->isVideoCharacter()) {
            const std::string& videoPath = ch->activeVideoPath();
            PreviewCharLayer layer;
            layer.isBackground = true;       // render as image layer
            layer.isVideoCharacter = true;   // use character-style sizing
            layer.visible      = ch->visible;
            layer.opacity      = ch->opacity;
            layer.posX         = ch->posX;
            layer.posY         = ch->posY;
            layer.scale        = ch->scale;
            layer.layerIndex   = i;
            layer.cropLeft     = ch->cropLeft;
            layer.cropRight    = ch->cropRight;
            layer.cropTop      = ch->cropTop;
            layer.cropBottom   = ch->cropBottom;
            layer.flipX        = ch->flipX;
            layer.rotation     = ch->rotation;
            layer.blur         = ch->blur;

            if (!videoPath.empty()) {
                // Helper to detect and unpack packed-alpha frames
                // for the initial frame / thumbnail only. The worker thread
                // now unpacks every decoded frame before storing it.
                auto maybeUnpack = [](QImage img) -> QImage {
                    if (img.isNull()) return img;
                    if (img.height() > img.width() && (img.height() % 2 == 0) &&
                        img.height() >= img.width() * 1.8) {
                        return unpackPackedAlpha(img.bits(),
                            static_cast<uint32_t>(img.width()),
                            static_cast<uint32_t>(img.height()));
                    }
                    return img;
                };

                auto player = getOrCreateVideoPlayer(videoPath);
                if (player) {
                    if (!player->lastFrame.isNull())
                        layer.backgroundImage = player->lastFrame; // already unpacked at open
                    else {
                        QImage thumb = extractVideoThumbnail(videoPath);
                        if (!thumb.isNull())
                            layer.backgroundImage = maybeUnpack(thumb);
                    }
                    // Frames from advanceVideoPlayer are already unpacked
                    // by the worker thread â€” no per-frame pixel processing
                    // on the UI thread.
                    layer.videoFrameProvider = [player](float vdt) -> QImage {
                        return ShotComposer::advanceVideoPlayer(player, vdt);
                    };
                } else {
                    QImage frame = extractVideoThumbnail(videoPath);
                    if (!frame.isNull())
                        layer.backgroundImage = maybeUnpack(frame);
                }
            }

            previewLayers.push_back(std::move(layer));
            continue;
        }

        // â”€â”€ Normal Spine character â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        size_t ei = static_cast<size_t>(ref.index);
        if (ei >= m_layerEngines.size()) {
            spdlog::error("ShotComposer::updatePreview: character index {} >= engine pool size {} â€” skipping",
                          ei, m_layerEngines.size());
            continue;
        }
        auto& engine = m_layerEngines[ei];

        std::string outfitKey = ch->outfit.empty() ? "default" : ch->outfit;
        CharacterStance stance = ch->stance;

        const auto* variant = m_modelManager->findVariant(ch->characterName, outfitKey, stance);
        if (!variant || variant->skelPath.empty() || variant->atlasPath.empty())
            continue;

        // Clear cached textures if the skeleton/atlas is about to change
        if (engine->loadedSkelPath() != variant->skelPath ||
            engine->loadedAtlasPath() != variant->atlasPath) {
            m_layerTextures[ei].clear();
        }

        // loadSkeleton now skips if same files already loaded (fast path)
        if (!engine->loadSkeleton(variant->skelPath, variant->atlasPath, 0.5f)) {
            spdlog::warn("ShotComposer: Failed to load skeleton for '{}': {}",
                         ch->characterName, variant->skelPath);
            continue;
        }

        std::string animName = ch->animation.empty() ? "idle" : ch->animation;
        auto anims = engine->animation().listAnimations();
        bool foundAnim = false;
        for (const auto& a : anims) {
            if (a.name == animName) { foundAnim = true; break; }
        }
        if (foundAnim) {
            engine->animation().setBodyAnimation(animName, true);
        } else if (!anims.empty()) {
            engine->animation().setBodyAnimation(anims[0].name, true);
        }

        if (ch->isTalking)
            engine->animation().startTalking();
        else
            engine->animation().stopTalking();

        if (ref.index == selectedCharIdx)
            selectedAnims = anims;

        // Only reload textures if engine was actually reloaded (atlas changed)
        auto& textures = m_layerTextures[ei];
        if (textures.empty()) {
            const auto& pages = engine->atlas().pages();
            const auto& atlasDir = engine->atlas().directory();
            for (const auto& page : pages) {
                std::string fullPath = atlasDir + "/" + page.texturePath;
                QImage img(QString::fromStdString(fullPath));
                if (img.isNull()) {
                    textures.emplace_back();
                } else {
                    img = img.convertToFormat(QImage::Format_ARGB32);
                    // PMA atlas: un-premultiply so the rasteriser doesn't
                    // double-premultiply, which causes dark edge borders.
                    if (page.pma) {
                        uint8_t* px = img.bits();
                        const int total = img.width() * img.height();
                        for (int p = 0; p < total; ++p) {
                            const uint8_t a = px[p * 4 + 3];
                            if (a > 0 && a < 255) {
                                px[p * 4 + 0] = static_cast<uint8_t>(std::min(255, px[p * 4 + 0] * 255 / a));
                                px[p * 4 + 1] = static_cast<uint8_t>(std::min(255, px[p * 4 + 1] * 255 / a));
                                px[p * 4 + 2] = static_cast<uint8_t>(std::min(255, px[p * 4 + 2] * 255 / a));
                            } else if (a == 0) {
                                px[p * 4 + 0] = 0; px[p * 4 + 1] = 0; px[p * 4 + 2] = 0;
                            }
                        }
                    }
                    textures.push_back(std::move(img));
                }
            }
        }

        PreviewCharLayer layer;
        layer.engine      = engine.get();
        layer.textures    = textures;
        layer.posX        = ch->posX;
        layer.posY        = ch->posY;
        layer.scale       = ch->scale;
        layer.rotation    = ch->rotation;
        layer.flipX       = ch->flipX;
        layer.opacity     = ch->opacity;
        layer.visible     = ch->visible;
        layer.layerIndex  = i;
        layer.cropLeft    = ch->cropLeft;
        layer.cropRight   = ch->cropRight;
        layer.cropTop     = ch->cropTop;
        layer.cropBottom  = ch->cropBottom;
        layer.blur        = ch->blur;
        previewLayers.push_back(std::move(layer));
    }

    spdlog::debug("ShotComposer::updatePreview â€” built {} preview layers (back-to-front)",
        previewLayers.size());

    // previewLayers is already ordered back-to-front (we iterated in reverse)
    // Pass all layers to the preview widget
    m_spinePreview->setCharacterLayers(std::move(previewLayers));
    m_spinePreview->setSelectedLayer(m_selectedLayer);

    // Keep SpinePreviewWidget's multi-select set up-to-date after rebuilding layers
    {
        QSet<int> sel;
        const auto rows = m_layerList->selectionModel()->selectedRows();
        for (const auto& idx : rows)
            sel.insert(idx.row());
        if (sel.isEmpty() && m_selectedLayer >= 0)
            sel.insert(m_selectedLayer);
        m_spinePreview->setSelectedLayers(sel);
    }

    // NOTE: camera transform is NOT applied here â€” it is set once when the
    // shot is loaded (setCurrentShot) or when the user changes camera spins
    // (onCameraPropertyChanged).  Applying it here would reset the user's
    // interactive viewport zoom/pan every time a layer is clicked.

    // Start animation timer for any scene with layers (characters OR videos)
    bool hasAnimatedContent = !m_currentShot.characters().empty();
    // Check if any video layers exist (they need the timer for playback)
    for (int i = 0; i < m_currentShot.backgroundCount(); ++i) {
        const auto* bg = m_currentShot.background(i);
        if (bg && bg->isVideo()) { hasAnimatedContent = true; break; }
    }
    if (hasAnimatedContent)
        m_spinePreview->startAnimation();
    else
        m_spinePreview->update(); // Just repaint for static BG-only scenes

    // Populate animation dropdown for the SELECTED character layer only.
    // Each character has its own unique set of animation names â€” filter
    // out internal bookend animations (talk_start / talk_end) so only
    // user-meaningful body animations are shown.
    if (selectedCharIdx >= 0 && !selectedAnims.empty()) {
        m_updating = true;
        QString currentAnim = m_animCombo->currentText();
        m_animCombo->clear();
        for (const auto& a : selectedAnims) {
            if (a.name == "talk_start" || a.name == "talk_end") continue;
            if (a.duration <= 0.0f) continue;  // skip zero-length markers
            m_animCombo->addItem(QString::fromStdString(a.name));
        }
        int idx = m_animCombo->findText(currentAnim);
        if (idx >= 0) {
            m_animCombo->setCurrentIndex(idx);
        } else if (m_animCombo->count() > 0) {
            int idleIdx = m_animCombo->findText("idle");
            m_animCombo->setCurrentIndex(idleIdx >= 0 ? idleIdx : 0);
        }
        m_updating = false;
    }
#endif
}

} // namespace rt

