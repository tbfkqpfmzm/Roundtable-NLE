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

} // namespace rt