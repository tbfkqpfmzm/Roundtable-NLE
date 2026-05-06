/*
 * test_monitors.cpp — Tests for Step 15: Source & Program Monitors
 *
 * Tests pure-logic aspects of:
 *   - MiniTimeline: coordinate mapping, clamping, in/out points, selected duration
 *   - Viewport: fit mode layout, coordinate mapping, frame display state
 *   - SourceMonitor: source region computation
 *   - ProgramMonitor: output resolution, composite callback
 *
 * These tests exercise the logic without needing Vulkan or a visible window,
 * but they DO require QApplication for widget instantiation.
 */

#include <gtest/gtest.h>

#include <QApplication>

// MiniTimeline and Viewport are Qt widgets, need headers
#include "widgets/MiniTimeline.h"
#include "viewport/Viewport.h"
#include "panels/monitors/SourceMonitor.h"
#include "panels/monitors/ProgramMonitor.h"
#include "media/PlaybackController.h"
#include "media/FrameCache.h"
#include "timeline/Timeline.h"

#include <cmath>
#include <memory>
#include <vector>

using namespace rt;

// ═════════════════════════════════════════════════════════════════════════════
//  QApplication fixture
// ═════════════════════════════════════════════════════════════════════════════

static QApplication* g_app = nullptr;

class MonitorTestEnv : public ::testing::Environment
{
public:
    void SetUp() override
    {
        if (!g_app)
        {
            static int    argc = 1;
            static char   arg0[] = "test_monitors";
            static char*  argv[] = { arg0, nullptr };
            g_app = new QApplication(argc, argv);
        }
    }
};

static auto* g_env = ::testing::AddGlobalTestEnvironment(new MonitorTestEnv);

// ═════════════════════════════════════════════════════════════════════════════
//  MiniTimeline tests
// ═════════════════════════════════════════════════════════════════════════════

class MiniTimelineTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        mt = std::make_unique<MiniTimeline>();
        // Give it a known width for predictable coordinate mapping
        mt->resize(408, MiniTimeline::kBarHeight);  // barWidth = 408 - 2*4 = 400
    }

    std::unique_ptr<MiniTimeline> mt;
};

TEST_F(MiniTimelineTest, DefaultState)
{
    EXPECT_EQ(mt->duration(), 0);
    EXPECT_EQ(mt->playhead(), 0);
    EXPECT_EQ(mt->inPoint(), -1);
    EXPECT_EQ(mt->outPoint(), -1);
    EXPECT_FALSE(mt->hasInPoint());
    EXPECT_FALSE(mt->hasOutPoint());
    EXPECT_DOUBLE_EQ(mt->frameRate(), 24.0);
}

TEST_F(MiniTimelineTest, SetDuration)
{
    mt->setDuration(48000);  // 1 second
    EXPECT_EQ(mt->duration(), 48000);

    // Negative duration clamped to 0
    mt->setDuration(-100);
    EXPECT_EQ(mt->duration(), 0);
}

TEST_F(MiniTimelineTest, SetPlayhead)
{
    mt->setDuration(48000);
    mt->setPlayhead(24000);
    EXPECT_EQ(mt->playhead(), 24000);

    // Clamped to duration
    mt->setPlayhead(96000);
    EXPECT_EQ(mt->playhead(), 48000);

    // Clamped to 0
    mt->setPlayhead(-100);
    EXPECT_EQ(mt->playhead(), 0);
}

TEST_F(MiniTimelineTest, ClampTick)
{
    mt->setDuration(48000);
    EXPECT_EQ(mt->clampTick(-100), 0);
    EXPECT_EQ(mt->clampTick(0), 0);
    EXPECT_EQ(mt->clampTick(24000), 24000);
    EXPECT_EQ(mt->clampTick(48000), 48000);
    EXPECT_EQ(mt->clampTick(96000), 48000);
}

TEST_F(MiniTimelineTest, ClampTickZeroDuration)
{
    mt->setDuration(0);
    EXPECT_EQ(mt->clampTick(-100), 0);
    EXPECT_EQ(mt->clampTick(0), 0);
    EXPECT_EQ(mt->clampTick(100), 0);
}

TEST_F(MiniTimelineTest, PositionToTick)
{
    mt->setDuration(48000);  // 1 second, barWidth=400

    // Left edge (margin=4) → tick 0
    EXPECT_EQ(mt->positionToTick(4.0), 0);

    // Right edge (4+400=404) → tick 48000
    EXPECT_EQ(mt->positionToTick(404.0), 48000);

    // Middle (4+200=204) → tick 24000
    EXPECT_EQ(mt->positionToTick(204.0), 24000);

    // Before left edge → clamped to 0
    EXPECT_EQ(mt->positionToTick(-10.0), 0);

    // After right edge → clamped to 48000
    EXPECT_EQ(mt->positionToTick(500.0), 48000);
}

TEST_F(MiniTimelineTest, TickToPosition)
{
    mt->setDuration(48000);  // barWidth=400

    // Tick 0 → left edge
    EXPECT_DOUBLE_EQ(mt->tickToPosition(0), 4.0);

    // Tick 48000 → right edge
    EXPECT_DOUBLE_EQ(mt->tickToPosition(48000), 404.0);

    // Tick 24000 → middle
    EXPECT_DOUBLE_EQ(mt->tickToPosition(24000), 204.0);
}

TEST_F(MiniTimelineTest, PositionToTickRoundtrip)
{
    mt->setDuration(96000);

    for (int64_t tick = 0; tick <= 96000; tick += 4800)
    {
        double pos = mt->tickToPosition(tick);
        int64_t recovered = mt->positionToTick(pos);
        // Allow 1 tick tolerance due to integer rounding
        EXPECT_NEAR(recovered, tick, 1) << "Roundtrip failed for tick=" << tick;
    }
}

TEST_F(MiniTimelineTest, InOutPoints)
{
    mt->setDuration(48000);

    mt->setInPoint(10000);
    EXPECT_TRUE(mt->hasInPoint());
    EXPECT_EQ(mt->inPoint(), 10000);

    mt->setOutPoint(40000);
    EXPECT_TRUE(mt->hasOutPoint());
    EXPECT_EQ(mt->outPoint(), 40000);

    // Clamped
    mt->setInPoint(-100);
    EXPECT_EQ(mt->inPoint(), 0);

    mt->setOutPoint(99000);
    EXPECT_EQ(mt->outPoint(), 48000);
}

TEST_F(MiniTimelineTest, ClearInOutPoints)
{
    mt->setDuration(48000);
    mt->setInPoint(10000);
    mt->setOutPoint(40000);

    mt->clearInOutPoints();
    EXPECT_FALSE(mt->hasInPoint());
    EXPECT_FALSE(mt->hasOutPoint());
    EXPECT_EQ(mt->inPoint(), -1);
    EXPECT_EQ(mt->outPoint(), -1);
}

TEST_F(MiniTimelineTest, SelectedDuration)
{
    mt->setDuration(48000);

    // No in/out → full duration
    EXPECT_EQ(mt->selectedDuration(), 48000);

    // With in/out
    mt->setInPoint(10000);
    mt->setOutPoint(40000);
    EXPECT_EQ(mt->selectedDuration(), 30000);

    // In > Out → returns full duration
    mt->setInPoint(40000);
    mt->setOutPoint(10000);
    EXPECT_EQ(mt->selectedDuration(), 48000);
}

TEST_F(MiniTimelineTest, FrameRate)
{
    mt->setFrameRate(30.0);
    EXPECT_DOUBLE_EQ(mt->frameRate(), 30.0);

    mt->setFrameRate(60.0);
    EXPECT_DOUBLE_EQ(mt->frameRate(), 60.0);

    // Invalid FPS → default to 24
    mt->setFrameRate(0.0);
    EXPECT_DOUBLE_EQ(mt->frameRate(), 24.0);

    mt->setFrameRate(-10.0);
    EXPECT_DOUBLE_EQ(mt->frameRate(), 24.0);
}

TEST_F(MiniTimelineTest, ZeroDurationMapping)
{
    mt->setDuration(0);

    // positionToTick should return 0 for any position
    EXPECT_EQ(mt->positionToTick(0.0), 0);
    EXPECT_EQ(mt->positionToTick(200.0), 0);

    // tickToPosition should return margin for any tick
    EXPECT_DOUBLE_EQ(mt->tickToPosition(0), MiniTimeline::kMarginH);
    EXPECT_DOUBLE_EQ(mt->tickToPosition(48000), MiniTimeline::kMarginH);
}

TEST_F(MiniTimelineTest, SizeHints)
{
    QSize sh = mt->sizeHint();
    EXPECT_GE(sh.width(), 100);
    EXPECT_EQ(sh.height(), MiniTimeline::kBarHeight);

    QSize msh = mt->minimumSizeHint();
    EXPECT_GE(msh.width(), 100);
    EXPECT_EQ(msh.height(), MiniTimeline::kBarHeight);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Viewport tests
// ═════════════════════════════════════════════════════════════════════════════

class ViewportTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        vp = std::make_unique<Viewport>();
        vp->resize(640, 360);
    }

    std::unique_ptr<Viewport> vp;
};

TEST_F(ViewportTest, DefaultState)
{
    EXPECT_FALSE(vp->hasFrame());
    EXPECT_EQ(vp->frameWidth(), 0u);
    EXPECT_EQ(vp->frameHeight(), 0u);
    EXPECT_EQ(vp->fitMode(), ViewportFitMode::Fit);
    EXPECT_FALSE(vp->safeAreasVisible());
}

TEST_F(ViewportTest, DisplayRawFrame)
{
    // Create a 4x4 BGRA test frame
    std::vector<uint8_t> pixels(4 * 4 * 4, 128);
    vp->displayRaw(pixels.data(), 4, 4);

    EXPECT_TRUE(vp->hasFrame());
    EXPECT_EQ(vp->frameWidth(), 4u);
    EXPECT_EQ(vp->frameHeight(), 4u);
}

TEST_F(ViewportTest, DisplayCachedFrame)
{
    CachedFrame frame;
    frame.width  = 8;
    frame.height = 6;
    frame.stride = 8 * 4;
    frame.pixels.resize(8 * 6 * 4, 200);

    vp->displayFrame(frame);

    EXPECT_TRUE(vp->hasFrame());
    EXPECT_EQ(vp->frameWidth(), 8u);
    EXPECT_EQ(vp->frameHeight(), 6u);
}

TEST_F(ViewportTest, ClearFrame)
{
    // Display a frame first
    std::vector<uint8_t> pixels(4 * 4 * 4, 128);
    vp->displayRaw(pixels.data(), 4, 4);
    EXPECT_TRUE(vp->hasFrame());

    // Clear
    vp->clearFrame();
    EXPECT_FALSE(vp->hasFrame());
    EXPECT_EQ(vp->frameWidth(), 0u);
    EXPECT_EQ(vp->frameHeight(), 0u);
}

TEST_F(ViewportTest, DisplayEmptyFrame)
{
    CachedFrame frame;
    frame.width  = 0;
    frame.height = 0;

    vp->displayFrame(frame);
    EXPECT_FALSE(vp->hasFrame());
}

TEST_F(ViewportTest, DisplayNullData)
{
    vp->displayRaw(nullptr, 100, 100);
    EXPECT_FALSE(vp->hasFrame());
}

TEST_F(ViewportTest, FitModeSwitch)
{
    vp->setFitMode(ViewportFitMode::Fill);
    EXPECT_EQ(vp->fitMode(), ViewportFitMode::Fill);

    vp->setFitMode(ViewportFitMode::Actual);
    EXPECT_EQ(vp->fitMode(), ViewportFitMode::Actual);

    vp->setFitMode(ViewportFitMode::Fit);
    EXPECT_EQ(vp->fitMode(), ViewportFitMode::Fit);
}

TEST_F(ViewportTest, FrameRectFitMode)
{
    // 640x360 widget, 1920x1080 frame (same aspect ratio 16:9)
    std::vector<uint8_t> pixels(1920 * 1080 * 4, 100);
    vp->displayRaw(pixels.data(), 1920, 1080);

    vp->setFitMode(ViewportFitMode::Fit);
    QRectF fr = vp->frameRect();

    // Should fill the entire widget since aspect ratio matches
    EXPECT_NEAR(fr.width(), 640.0, 1.0);
    EXPECT_NEAR(fr.height(), 360.0, 1.0);
    EXPECT_NEAR(fr.x(), 0.0, 1.0);
    EXPECT_NEAR(fr.y(), 0.0, 1.0);
}

TEST_F(ViewportTest, FrameRectFitModeLetterbox)
{
    vp->resize(640, 640);  // Square widget

    // 1920x1080 frame in square widget → letterbox (horizontal bars)
    std::vector<uint8_t> pixels(1920 * 1080 * 4, 100);
    vp->displayRaw(pixels.data(), 1920, 1080);

    vp->setFitMode(ViewportFitMode::Fit);
    QRectF fr = vp->frameRect();

    // Width fills 640, height = 640 * (1080/1920) = 360
    EXPECT_NEAR(fr.width(), 640.0, 1.0);
    EXPECT_NEAR(fr.height(), 360.0, 1.0);
    // Centered vertically: y = (640 - 360) / 2 = 140
    EXPECT_NEAR(fr.y(), 140.0, 1.0);
}

TEST_F(ViewportTest, FrameRectFitModePillarbox)
{
    vp->resize(640, 640);  // Square widget

    // 1080x1920 frame in square widget → pillarbox (vertical bars)
    std::vector<uint8_t> pixels(1080 * 1920 * 4, 100);
    vp->displayRaw(pixels.data(), 1080, 1920);

    vp->setFitMode(ViewportFitMode::Fit);
    QRectF fr = vp->frameRect();

    // Height fills 640, width = 640 * (1080/1920) = 360
    EXPECT_NEAR(fr.height(), 640.0, 1.0);
    EXPECT_NEAR(fr.width(), 360.0, 1.0);
    // Centered horizontally: x = (640 - 360) / 2 = 140
    EXPECT_NEAR(fr.x(), 140.0, 1.0);
}

TEST_F(ViewportTest, FrameRectFillMode)
{
    vp->resize(640, 640);  // Square widget

    // 1920x1080 frame in square widget with Fill → should more than fill
    std::vector<uint8_t> pixels(1920 * 1080 * 4, 100);
    vp->displayRaw(pixels.data(), 1920, 1080);

    vp->setFitMode(ViewportFitMode::Fill);
    QRectF fr = vp->frameRect();

    // Height fills 640, width = 640 * (1920/1080) ≈ 1137.8
    EXPECT_NEAR(fr.height(), 640.0, 1.0);
    EXPECT_GT(fr.width(), 640.0);
}

TEST_F(ViewportTest, FrameRectActualMode)
{
    vp->resize(640, 360);

    std::vector<uint8_t> pixels(320 * 240 * 4, 100);
    vp->displayRaw(pixels.data(), 320, 240);

    vp->setFitMode(ViewportFitMode::Actual);
    QRectF fr = vp->frameRect();

    // Frame should be at 1:1 pixel size, centered
    EXPECT_NEAR(fr.width(), 320.0, 0.1);
    EXPECT_NEAR(fr.height(), 240.0, 0.1);
    EXPECT_NEAR(fr.x(), (640.0 - 320.0) / 2.0, 0.1);
    EXPECT_NEAR(fr.y(), (360.0 - 240.0) / 2.0, 0.1);
}

TEST_F(ViewportTest, CoordinateMappingNoFrame)
{
    QPointF fp = vp->widgetToFrame(QPointF(100, 100));
    EXPECT_LT(fp.x(), 0);  // Should return (-1,-1)
    EXPECT_LT(fp.y(), 0);
}

TEST_F(ViewportTest, CoordinateMappingWithFrame)
{
    vp->resize(640, 360);

    // 640x360 frame in 640x360 widget → perfect fit
    std::vector<uint8_t> pixels(640 * 360 * 4, 100);
    vp->displayRaw(pixels.data(), 640, 360);

    vp->setFitMode(ViewportFitMode::Fit);

    // Center of widget → center of frame
    QPointF fp = vp->widgetToFrame(QPointF(320, 180));
    EXPECT_NEAR(fp.x(), 320.0, 2.0);
    EXPECT_NEAR(fp.y(), 180.0, 2.0);

    // Top-left → (0,0) in frame
    QPointF tl = vp->widgetToFrame(QPointF(0, 0));
    EXPECT_NEAR(tl.x(), 0.0, 2.0);
    EXPECT_NEAR(tl.y(), 0.0, 2.0);
}

TEST_F(ViewportTest, CoordinateRoundtrip)
{
    vp->resize(640, 360);

    std::vector<uint8_t> pixels(1920 * 1080 * 4, 100);
    vp->displayRaw(pixels.data(), 1920, 1080);

    vp->setFitMode(ViewportFitMode::Fit);

    QPointF framePos(960.0, 540.0);  // Center of frame
    QPointF widgetPos = vp->frameToWidget(framePos);
    QPointF recovered = vp->widgetToFrame(widgetPos);

    EXPECT_NEAR(recovered.x(), framePos.x(), 2.0);
    EXPECT_NEAR(recovered.y(), framePos.y(), 2.0);
}

TEST_F(ViewportTest, SafeAreas)
{
    EXPECT_FALSE(vp->safeAreasVisible());
    vp->setSafeAreasVisible(true);
    EXPECT_TRUE(vp->safeAreasVisible());
    vp->setSafeAreasVisible(false);
    EXPECT_FALSE(vp->safeAreasVisible());
}

TEST_F(ViewportTest, SizeHints)
{
    QSize sh = vp->sizeHint();
    EXPECT_GE(sh.width(), 160);
    EXPECT_GE(sh.height(), 90);

    QSize msh = vp->minimumSizeHint();
    EXPECT_EQ(msh.width(), 160);
    EXPECT_EQ(msh.height(), 90);
}

TEST_F(ViewportTest, FrameRectEmptyWhenNoFrame)
{
    QRectF fr = vp->frameRect();
    EXPECT_TRUE(fr.isEmpty() || fr.isNull());
}

TEST_F(ViewportTest, OverlayText)
{
    // Should not crash, overlay text is purely visual
    vp->setOverlayText(QStringLiteral("00:00:01:00"));
    // No assertion needed — just verify no crash
}

TEST_F(ViewportTest, DisplayWithCustomStride)
{
    // Stride > width * 4 (padding at end of each row)
    uint32_t w = 10, h = 10;
    uint32_t stride = 48; // 10*4=40, plus 8 padding
    std::vector<uint8_t> pixels(stride * h, 128);
    vp->displayRaw(pixels.data(), w, h, stride);

    EXPECT_TRUE(vp->hasFrame());
    EXPECT_EQ(vp->frameWidth(), 10u);
    EXPECT_EQ(vp->frameHeight(), 10u);
}

// ═════════════════════════════════════════════════════════════════════════════
//  SourceMonitor tests
// ═════════════════════════════════════════════════════════════════════════════

class SourceMonitorTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        sm = std::make_unique<SourceMonitor>();
    }

    std::unique_ptr<SourceMonitor> sm;
};

TEST_F(SourceMonitorTest, DefaultState)
{
    EXPECT_FALSE(sm->hasClip());
    EXPECT_EQ(sm->mediaHandle(), 0u);
    EXPECT_EQ(sm->currentTick(), 0);
    EXPECT_FALSE(sm->hasInPoint());
    EXPECT_FALSE(sm->hasOutPoint());
    EXPECT_EQ(sm->frameCount(), 0);
    EXPECT_EQ(sm->clipDuration(), 0);
}

TEST_F(SourceMonitorTest, ControllerExists)
{
    EXPECT_NE(sm->controller(), nullptr);
}

TEST_F(SourceMonitorTest, SourceRegionDefault)
{
    auto region = sm->sourceRegion();
    EXPECT_EQ(region.mediaHandle, 0u);
    EXPECT_EQ(region.sourceIn, 0);
    EXPECT_EQ(region.sourceOut, 0);
    EXPECT_EQ(region.duration, 0);
}

TEST_F(SourceMonitorTest, ClearClipResets)
{
    sm->clearClip();
    EXPECT_FALSE(sm->hasClip());
    EXPECT_EQ(sm->mediaHandle(), 0u);
    EXPECT_EQ(sm->frameCount(), 0);
    EXPECT_EQ(sm->clipDuration(), 0);
}

TEST_F(SourceMonitorTest, LoadClipNullPool)
{
    sm->loadClip(1, nullptr);
    EXPECT_FALSE(sm->hasClip());
}

TEST_F(SourceMonitorTest, SizeHint)
{
    QSize sh = sm->sizeHint();
    EXPECT_GE(sh.width(), 200);
    EXPECT_GE(sh.height(), 200);
}

// ═════════════════════════════════════════════════════════════════════════════
//  ProgramMonitor tests
// ═════════════════════════════════════════════════════════════════════════════

class ProgramMonitorTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        pm = std::make_unique<ProgramMonitor>();
    }

    std::unique_ptr<ProgramMonitor> pm;
};

TEST_F(ProgramMonitorTest, DefaultState)
{
    EXPECT_EQ(pm->controller(), nullptr);
    EXPECT_EQ(pm->timeline(), nullptr);
    EXPECT_EQ(pm->outputWidth(), 1920u);
    EXPECT_EQ(pm->outputHeight(), 1080u);
}

TEST_F(ProgramMonitorTest, SetOutputResolution)
{
    pm->setOutputResolution(3840, 2160);
    EXPECT_EQ(pm->outputWidth(), 3840u);
    EXPECT_EQ(pm->outputHeight(), 2160u);
}

TEST_F(ProgramMonitorTest, SetController)
{
    PlaybackController ctrl;
    pm->setController(&ctrl);
    EXPECT_EQ(pm->controller(), &ctrl);
}

TEST_F(ProgramMonitorTest, SetTimeline)
{
    Timeline tl;
    pm->setTimeline(&tl);
    EXPECT_EQ(pm->timeline(), &tl);
}

TEST_F(ProgramMonitorTest, ViewportAccess)
{
    EXPECT_NE(pm->viewport(), nullptr);
}

TEST_F(ProgramMonitorTest, MiniTimelineAccess)
{
    EXPECT_NE(pm->miniTimeline(), nullptr);
}

TEST_F(ProgramMonitorTest, SetCompositeCallback)
{
    bool called = false;
    pm->setCompositeCallback([&](int64_t /*tick*/, uint32_t /*w*/, uint32_t /*h*/, bool /*scrub*/) -> std::shared_ptr<CachedFrame> {
        called = true;
        return nullptr;
    });

    // The callback is stored but not called without a controller
    EXPECT_FALSE(called);
}

TEST_F(ProgramMonitorTest, RefreshWithCallbackAndController)
{
    PlaybackController ctrl;
    pm->setController(&ctrl);

    int64_t capturedTick = -1;
    pm->setCompositeCallback([&](int64_t tick, uint32_t /*w*/, uint32_t /*h*/, bool /*scrub*/) -> std::shared_ptr<CachedFrame> {
        capturedTick = tick;
        return nullptr;
    });

    pm->refresh();
    EXPECT_EQ(capturedTick, 0);  // Controller starts at tick 0
}

TEST_F(ProgramMonitorTest, RefreshWithFrameData)
{
    PlaybackController ctrl;
    pm->setController(&ctrl);

    // Create a test frame
    auto testFrame = std::make_shared<CachedFrame>();
    testFrame->width  = 320;
    testFrame->height = 240;
    testFrame->stride = 320 * 4;
    testFrame->pixels.resize(320 * 240 * 4, 128);

    pm->setCompositeCallback([&](int64_t /*tick*/, uint32_t /*w*/, uint32_t /*h*/, bool /*scrub*/) -> std::shared_ptr<CachedFrame> {
        return testFrame;
    });

    pm->refresh();

    // Viewport should now have a frame
    EXPECT_TRUE(pm->viewport()->hasFrame());
    EXPECT_EQ(pm->viewport()->frameWidth(), 320u);
    EXPECT_EQ(pm->viewport()->frameHeight(), 240u);
}

TEST_F(ProgramMonitorTest, RefreshSkipsSameTick)
{
    PlaybackController ctrl;
    pm->setController(&ctrl);

    int callCount = 0;
    pm->setCompositeCallback([&](int64_t /*tick*/, uint32_t /*w*/, uint32_t /*h*/, bool /*scrub*/) -> std::shared_ptr<CachedFrame> {
        callCount++;
        return nullptr;
    });

    pm->refresh();
    EXPECT_EQ(callCount, 1);

    // Second refresh at the same tick should be skipped (no change in position)
    // But refresh() forces a re-render by resetting lastRenderedTick
    pm->refresh();
    EXPECT_EQ(callCount, 2);
}

TEST_F(ProgramMonitorTest, SizeHint)
{
    QSize sh = pm->sizeHint();
    EXPECT_GE(sh.width(), 200);
    EXPECT_GE(sh.height(), 200);
}

TEST_F(ProgramMonitorTest, TimelineUpdatesMinitimeline)
{
    Timeline tl;
    (void)tl.addVideoTrack("V1");
    // (Can't add clips easily without full Clip creation, but duration should be 0 initially)
    pm->setTimeline(&tl);

    // Mini-timeline should reflect timeline duration
    EXPECT_EQ(pm->miniTimeline()->duration(), tl.duration());
}

TEST_F(ProgramMonitorTest, PollStartStop)
{
    // Should not crash
    pm->startPolling(32);
    pm->stopPolling();
}

// ═════════════════════════════════════════════════════════════════════════════
//  Integration tests
// ═════════════════════════════════════════════════════════════════════════════

TEST(MonitorIntegration, MiniTimelineVariousWidths)
{
    MiniTimeline mt;
    mt.setDuration(96000); // 2 seconds

    // Test at various widget widths
    int widths[] = { 100, 200, 400, 800, 1600 };
    for (int w : widths)
    {
        mt.resize(w, MiniTimeline::kBarHeight);

        // Tick 0 → position at margin
        double pos0 = mt.tickToPosition(0);
        EXPECT_DOUBLE_EQ(pos0, MiniTimeline::kMarginH)
            << "Width=" << w;

        // Full duration → position at widget_width - margin
        double posEnd = mt.tickToPosition(96000);
        EXPECT_NEAR(posEnd, w - MiniTimeline::kMarginH, 0.1)
            << "Width=" << w;

        // Roundtrip at mid-point
        double posMid = mt.tickToPosition(48000);
        int64_t tickMid = mt.positionToTick(posMid);
        EXPECT_NEAR(tickMid, 48000, 1) << "Width=" << w;
    }
}

TEST(MonitorIntegration, ViewportMultipleFrameSizes)
{
    Viewport vp;
    vp.resize(640, 360);

    struct Case { uint32_t w, h; };
    Case cases[] = {
        {1920, 1080}, {1280, 720}, {3840, 2160},
        {100, 100}, {4, 4}, {1, 1}
    };

    for (const auto& c : cases)
    {
        std::vector<uint8_t> px(c.w * c.h * 4, 100);
        vp.displayRaw(px.data(), c.w, c.h);

        EXPECT_TRUE(vp.hasFrame()) << c.w << "x" << c.h;
        EXPECT_EQ(vp.frameWidth(), c.w);
        EXPECT_EQ(vp.frameHeight(), c.h);

        QRectF fr = vp.frameRect();
        EXPECT_GT(fr.width(), 0) << c.w << "x" << c.h;
        EXPECT_GT(fr.height(), 0) << c.w << "x" << c.h;
    }
}

TEST(MonitorIntegration, ProgramMonitorWithTimelineInOut)
{
    ProgramMonitor pm;
    Timeline tl;

    tl.setInPoint(10000);
    tl.setOutPoint(40000);

    pm.setTimeline(&tl);

    // Timeline has no clips, so duration=0. The in/out points from the timeline
    // are clamped by MiniTimeline to [0, duration]. Since duration=0, they become 0.
    // This test verifies the wiring: setTimeline propagates in/out to MiniTimeline.
    // The clamping is correct behavior.
    EXPECT_EQ(pm.miniTimeline()->inPoint(), pm.miniTimeline()->clampTick(10000));
    EXPECT_EQ(pm.miniTimeline()->outPoint(), pm.miniTimeline()->clampTick(40000));
}
