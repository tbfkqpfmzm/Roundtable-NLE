/*
 * VUMeter — Audio level meter widget.
 *
 * Step 19: GPU Waveform Renderer
 *
 * Vertical bar meter displaying real-time audio levels with:
 *   - Peak hold with decay
 *   - Gradient coloring (green → yellow → red by level)
 *   - Stereo support (left/right bars)
 *   - dB scale markings
 *   - Clipping indicator
 *
 * Layout:
 *   ┌────┐
 *   │ ▓▓ │  ← clip indicator (red when >0 dB)
 *   │ ░░ │
 *   │ ▒▒ │  ← yellow zone (-12 to -6 dB)
 *   │ ▓▓ │
 *   │ ██ │  ← green zone (< -12 dB)
 *   │ ██ │
 *   │ ██ │
 *   └────┘
 */

#pragma once

#include <QColor>
#include <QWidget>

#include <cstdint>
#include <vector>

namespace rt {

class VUMeter : public QWidget
{
    Q_OBJECT

public:
    /// Orientation of the meter bars.
    enum class Orientation : uint8_t
    {
        Vertical,    ///< Bars grow upward
        Horizontal   ///< Bars grow rightward
    };

    explicit VUMeter(QWidget* parent = nullptr);
    ~VUMeter() override;

    // ── Level input ─────────────────────────────────────────────────────

    /// Set the current level for a single channel (0 = left, 1 = right).
    /// Level is in normalized linear scale [0, 1].
    void setLevel(int channel, float level);

    /// Set levels for all channels at once.
    void setLevels(const std::vector<float>& levels);

    /// Get the current level for a channel.
    [[nodiscard]] float level(int channel) const;

    /// Reset all levels and peak hold to zero.
    void reset();

    // ── Configuration ───────────────────────────────────────────────────

    /// Set the number of channels (default 2 = stereo).
    void setChannelCount(int channels);
    [[nodiscard]] int channelCount() const noexcept { return m_channelCount; }

    /// Set the meter orientation.
    void setOrientation(Orientation orient);
    [[nodiscard]] Orientation orientation() const noexcept { return m_orientation; }

    /// Enable/disable peak hold indicator.
    void setPeakHoldEnabled(bool enabled);
    [[nodiscard]] bool isPeakHoldEnabled() const noexcept { return m_peakHoldEnabled; }

    /// Set peak hold decay time in milliseconds.
    void setPeakHoldDecayMs(int ms);
    [[nodiscard]] int peakHoldDecayMs() const noexcept { return m_peakHoldDecayMs; }

    /// Enable/disable dB scale markings.
    void setScaleVisible(bool visible);
    [[nodiscard]] bool isScaleVisible() const noexcept { return m_scaleVisible; }

    /// Set the minimum dB value displayed (default -60).
    void setMinDb(float db);
    [[nodiscard]] float minDb() const noexcept { return m_minDb; }

    // ── Appearance ──────────────────────────────────────────────────────

    /// Set the background color.
    void setBackgroundColor(const QColor& color);
    [[nodiscard]] QColor backgroundColor() const noexcept { return m_bgColor; }

    /// Set gradient zone colors.
    void setGradientColors(const QColor& low, const QColor& mid, const QColor& high);

    /// Set the clip indicator color.
    void setClipColor(const QColor& color);
    [[nodiscard]] QColor clipColor() const noexcept { return m_clipColor; }

    /// Set the peak hold indicator color.
    void setPeakHoldColor(const QColor& color);
    [[nodiscard]] QColor peakHoldColor() const noexcept { return m_peakHoldColor; }

    // ── Size hints ──────────────────────────────────────────────────────
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

    // ── Utility ─────────────────────────────────────────────────────────

    /// Convert linear level [0,1] to dB.
    [[nodiscard]] static float linearToDb(float level);

    /// Convert dB to linear level [0,1].
    [[nodiscard]] static float dbToLinear(float db);

    /// Convert dB to normalized position [0,1] for the given dB range.
    [[nodiscard]] float dbToPosition(float db) const;

signals:
    /// Emitted when any channel clips (level >= 1.0).
    void clipping(int channel);

protected:
    void paintEvent(QPaintEvent* event) override;
    void timerEvent(QTimerEvent* event) override;

private:
    void paintVertical(QPainter& painter);
    void paintHorizontal(QPainter& painter);
    void paintScale(QPainter& painter);

    QColor gradientColor(float normalizedPosition) const;

    // ── Per-channel state ───────────────────────────────────────────────
    struct ChannelState
    {
        float currentLevel{0.0f};    ///< Current level (linear)
        float peakLevel{0.0f};       ///< Peak hold level (linear)
        int   peakHoldTimer{0};      ///< Frames since peak
        bool  clipping{false};       ///< Currently clipping
    };

    std::vector<ChannelState> m_channels;

    // ── Configuration ───────────────────────────────────────────────────
    int         m_channelCount{2};
    Orientation m_orientation{Orientation::Vertical};
    bool        m_peakHoldEnabled{true};
    int         m_peakHoldDecayMs{1500};
    bool        m_scaleVisible{true};
    float       m_minDb{-60.0f};

    // ── Appearance ──────────────────────────────────────────────────────
    QColor m_bgColor{20, 20, 25};
    QColor m_colorLow{0, 200, 0};
    QColor m_colorMid{230, 230, 0};
    QColor m_colorHigh{255, 50, 0};
    QColor m_clipColor{255, 0, 0};
    QColor m_peakHoldColor{255, 255, 255};

    // ── Timer ───────────────────────────────────────────────────────────
    int m_timerId{0};
};

} // namespace rt
