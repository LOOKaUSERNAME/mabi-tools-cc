#pragma once
#include <QElapsedTimer>
#include <QProgressBar>
#include <QStyleOptionProgressBar>
#include <QStylePainter>
#include <algorithm>
#include <cmath>

namespace cc::ui {
// Keep the real QProgressBar value available to assistive technology.
// Only the style's paint option receives the interpolated fill.
class scan_progress_bar final : public QProgressBar {
public:
    using QProgressBar::QProgressBar;

    void set_measured(double percent, bool animate) {
        const double next = std::clamp(percent, 0., 100.);
        setValue(static_cast<int>(std::floor(next)));
        if (!animate || next < target_) {
            target_ = shown_ = next;
            clock_.invalidate();
        } else if (next != target_) {
            if (clock_.isValid())
                shown_ = at(clock_.elapsed());
            from_ = shown_;
            target_ = next;
            clock_.start();
        }
        update();
    }

    bool advance_fill(bool animate) {
        if (!clock_.isValid())
            return false;
        const auto elapsed = clock_.elapsed();
        shown_ = animate ? at(elapsed) : target_;
        if (!animate || elapsed >= duration_ms)
            clock_.invalidate();
        update();
        return clock_.isValid();
    }

    bool fill_in_motion() const { return clock_.isValid(); }

protected:
    void paintEvent(QPaintEvent*) override {
        QStyleOptionProgressBar options;
        initStyleOption(&options);
        options.maximum = 10'000;
        options.progress = static_cast<int>(std::lround(shown_ * 100.));
        QStylePainter painter(this);
        painter.drawControl(QStyle::CE_ProgressBar, options);
    }

private:
    static constexpr qint64 duration_ms = 100;
    double shown_{}, from_{}, target_{};
    QElapsedTimer clock_;

    double at(qint64 elapsed) const {
        const double fraction =
            std::clamp(static_cast<double>(elapsed) / static_cast<double>(duration_ms), 0., 1.);
        return std::lerp(from_, target_, fraction);
    }
};
}
