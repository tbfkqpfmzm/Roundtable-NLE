/*
 * SplashScreen — Premiere Pro-style loading screen for ROUNDTABLE.
 *
 * Shown during startup while App::init() and App::createMainWindow()
 * are in progress.  Displays the app icon, a progress bar, and the
 * version number, then auto-closes when the main window is ready.
 */

#pragma once

#include <QWidget>
#include <QProgressBar>
#include <QPixmap>
#include <QString>

namespace rt {

class SplashScreen : public QWidget
{
    Q_OBJECT

public:
    explicit SplashScreen(const QString &iconPath, const QString &version,
                          QWidget *parent = nullptr);
    ~SplashScreen() override = default;

    /// Set the progress bar percentage (0–100).
    void setProgress(int percent);

    /// Set a status message shown below the progress bar.
    void setStatus(const QString &text);

    /// Smoothly close and delete the splash screen.
    void finish(QWidget *mainWindow);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    QPixmap m_logo;
    QString m_version;
    QProgressBar *m_progressBar;
    int m_progress{0};
};

} // namespace rt
