#pragma once
#include "replay_model.hpp"
#include <QWidget>
#include <QElapsedTimer>
#include <functional>
#include <array>
#include <utility>

namespace cc::ui {
// GUI-only projection of one stable model/index. The model must outlive the
// card; rebuild cards before replacing the channel list. set_view caches chart
// inputs in milliseconds, while tick_motion changes only the visual reveal.
// The owner manages hover lifetime and routes callbacks to details/history.
class channel_card final : public QWidget {
public:
    channel_card(const replay_model& model, std::size_t index, QWidget* parent);
    void set_view(bool total, double maximum, bool show_picks, bool animate = false);
    bool tick_motion(bool allowed = true);
    std::function<void(std::size_t)> open_details;
    std::function<void(std::size_t)> show_hover;
    std::function<void()> leave_hover, dismiss_hover;

protected:
    void paintEvent(QPaintEvent*) override;
    void keyPressEvent(QKeyEvent* event) override;
    void enterEvent(QEnterEvent*) override;
    void leaveEvent(QEvent*) override;
    void focusInEvent(QFocusEvent*) override;
    void focusOutEvent(QFocusEvent*) override;

private:
    const replay_model& model_;
    std::size_t index_;
    bool total_{true}, show_picks_{true};
    // Recompute geometry inputs when observations or display mode change,
    // not on each frame of the short visual reveal.
    std::vector<cc::observation> ordered_;
    std::array<std::optional<std::pair<double, double>>, 2> bands_;
    QElapsedTimer reveal_;
    int newest_round_{-1};
    std::optional<double> timing(const cc::observation&, bool reply) const;
    double maximum_{10};
};
}
