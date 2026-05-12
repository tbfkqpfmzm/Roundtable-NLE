/*
 * ProjectPanelThumbnails.cpp — thumbnail management functions extracted
 * from ProjectPanel.cpp.
 */

#include "panels/project/ProjectPanel.h"

#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QImage>
#include <QString>

#include <spdlog/spdlog.h>

namespace rt {

// =============================================================================
// Thumbnail management
// =============================================================================

QString ProjectPanel::thumbnailPathForProject(const QString& projectName) const
{
    if (m_projectsDir.isEmpty()) return {};
    return m_projectsDir + "/thumbs/" + projectName + ".png";
}

void ProjectPanel::removeThumbnailForProject(const QString& projectName)
{
    if (m_projectsDir.isEmpty()) return;

    QString projFolder = m_projectsDir + "/" + projectName;
    bool removed = false;
    removed |= QFile::remove(projFolder + "/" + projectName + ".png");
    removed |= QFile::remove(projFolder + "/" + projectName + ".jpg");

    if (removed) {
        spdlog::info("ProjectPanel: Removed thumbnail for '{}'",
                     projectName.toStdString());
        rebuildTable();
    }
}

void ProjectPanel::setThumbnailForProject(const QString& projectName)
{
    QString path = QFileDialog::getOpenFileName(
        this, "Select Thumbnail Image", QString(),
        "Images (*.png *.jpg *.jpeg *.bmp *.webp)");

    if (path.isEmpty()) return;

    QImage img(path);
    if (img.isNull()) return;

    QString projFolder = m_projectsDir + "/" + projectName;
    QDir().mkpath(projFolder);

    if (img.width() > 480)
        img = img.scaledToWidth(480, Qt::SmoothTransformation);

    QString dstPath = projFolder + "/" + projectName + ".png";

    QFile::remove(projFolder + "/" + projectName + ".jpg");

    if (img.save(dstPath, "PNG")) {
        spdlog::info("ProjectPanel: Set thumbnail for '{}' from '{}'",
                     projectName.toStdString(), path.toStdString());
        rebuildTable();
    } else {
        spdlog::warn("ProjectPanel: Failed to save thumbnail to '{}'",
                     dstPath.toStdString());
    }
}

void ProjectPanel::setThumbnailFromPixels(const QString& projectName,
                                           const uint8_t* bgra,
                                           uint32_t width, uint32_t height)
{
    if (!bgra || width == 0 || height == 0) return;
    if (m_projectsDir.isEmpty()) return;

    QString projFolder = m_projectsDir + "/" + projectName;
    QDir().mkpath(projFolder);

    QImage img(bgra, static_cast<int>(width), static_cast<int>(height),
               static_cast<int>(width * 4), QImage::Format_ARGB32);

    if (img.width() > 480)
        img = img.scaledToWidth(480, Qt::SmoothTransformation);

    QString dstPath = projFolder + "/" + projectName + ".png";
    if (img.save(dstPath, "PNG")) {
        spdlog::info("ProjectPanel: Auto-saved thumbnail for '{}'  ({}x{})",
                     projectName.toStdString(), width, height);
        rebuildTable();
    } else {
        spdlog::warn("ProjectPanel: Failed to auto-save thumbnail to '{}'",
                     dstPath.toStdString());
    }
}

void ProjectPanel::setThumbnailFromImage(const QString& projectName, const QImage& image)
{
    if (image.isNull()) return;
    if (m_projectsDir.isEmpty()) return;

    QString projFolder = m_projectsDir + "/" + projectName;
    QDir().mkpath(projFolder);

    QImage img = image;
    if (img.width() > 480)
        img = img.scaledToWidth(480, Qt::SmoothTransformation);

    QString dstPath = projFolder + "/" + projectName + ".png";
    QFile::remove(projFolder + "/" + projectName + ".jpg");
    if (img.save(dstPath, "PNG")) {
        spdlog::info("ProjectPanel: Saved thumbnail for '{}' from viewport grab ({}x{})",
                     projectName.toStdString(), img.width(), img.height());
        rebuildTable();
    } else {
        spdlog::warn("ProjectPanel: Failed to save thumbnail to '{}'",
                     dstPath.toStdString());
    }
}

} // namespace rt
