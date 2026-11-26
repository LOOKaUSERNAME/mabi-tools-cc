#include "channel_card.hpp"
#include <QKeyEvent>
#include <QPainter>
#include <algorithm>
#include <cmath>

namespace cc::ui {
channel_card::channel_card(const replay_model& model, std::size_t index, QWidget* parent)
    : QWidget(parent), model_(model), index_(index) {
    setFocusPolicy(Qt::StrongFocus);
    const auto& stats = model_.summary.channels[index];
    const auto text =
        QString(
            "Ch. %1 · Connection %2 ms; Reply wait %3 ms; Total time %4 ms. %5 successful checks out of %6 attempted checks in this scan so far; warm-up excluded. Jitter %7. Press Enter for details and saved history.")
            .arg(model.channels[index].number)
            .arg(milliseconds(stats.average_connection_ms, 0),
                 milliseconds(stats.average_reply_ms, 0), milliseconds(stats.average_total_ms, 0))
            .arg(stats.successful)
            .arg(stats.attempted)
            .arg(variation(stats.total_jitter_ms));
    setAccessibleName(text);
    setObjectName(QString("Channel%1").arg(model.channels[index].number));
}

std::optional<double> channel_card::timing(const cc::observation& sample, bool reply) const {
    if (!sample.total_ms || sample.has_failure)
        return {};
    if (reply)
        return sample.connection_ms
                   ? std::optional<double>(*sample.total_ms - *sample.connection_ms)
                   : std::nullopt;
    return total_ ? sample.total_ms : sample.connection_ms;
}

bool channel_card::tick_motion(bool allowed) {
    if (!reveal_.isValid())
        return false;
    if (!allowed || reveal_.elapsed() >= 100)
        reveal_.invalidate();
    update(15, 33, width() - 30, 27);
    return reveal_.isValid();
}

void channel_card::set_view(bool total, double maximum, bool show_picks, bool animate) {
    const bool mode_changed = total_ != total;
    total_ = total;
    maximum_ = maximum;
    show_picks_ = show_picks;
    ordered_ = model_.channels[index_].samples;
    std::ranges::sort(ordered_, {}, &cc::observation::round);
    const int last = ordered_.empty() ? -1 : ordered_.back().round;
    if (!animate || mode_changed || last < newest_round_)
        reveal_.invalidate();
    else if (last > newest_round_) {
        reveal_.invalidate();
        if (ordered_.size() >= 2) {
            const auto& a = ordered_[ordered_.size() - 2];
            const auto& b = ordered_.back();
            if (a.round + 1 == b.round && timing(a, false) && timing(b, false))
                reveal_.start();
        }
    }
    newest_round_ = last;
    for (std::size_t i = 0; i < bands_.size(); ++i) {
        bands_[i].reset();
        std::vector<double> values;
        for (const auto& sample : ordered_)
            if (auto measured = timing(sample, i == 1))
                values.push_back(*measured);
        if (values.size() < 5)
            continue;
        std::ranges::sort(values);
        const auto middle = values.size() / 2;
        const double baseline =
            values.size() % 2 ? values[middle] : values[middle - 1] / 2 + values[middle] / 2;
        const double rise = std::max(10., baseline * .15);
        if (values.back() > baseline + rise)
            bands_[i] = std::pair{baseline + rise, baseline + 2 * rise};
    }
    const auto& stats = model_.summary.channels[index_];
    const auto& picks = model_.summary.picks;
    const int index = static_cast<int>(index_);
    QString highlight;
    if (show_picks) {
        highlight = model_.suggests(index_) ? "Suggested channel"
                    : picks.fastest == index && picks.consistent == index
                        ? "Fastest connection and most consistent"
                    : picks.fastest == index    ? "Fastest connection"
                    : picks.consistent == index ? "Most consistent"
                                                : "";
    }
    auto timing = [](std::optional<double> value) {
        return milliseconds(value, 0) + (value ? " ms" : "");
    };
    auto jitter = stats.total_jitter_ms ? "Jitter, " +
                                              variation(stats.total_jitter_ms)
                                                  .replace("<", "less than ")
                                                  .replace(" ms", " milliseconds") +
                                              "."
                                        : QString("Jitter, unavailable.");
    setAccessibleName(
        QString(
            "Ch. %1%2 · Connection %3; Reply wait %4; Total time %5. %6 successful checks out of %7 attempted checks in this scan so far; warm-up excluded. %8 Press Enter for details and saved history.")
            .arg(model_.channels[index_].number)
            .arg(highlight.isEmpty() ? "" : " · " + highlight, timing(stats.average_connection_ms),
                 timing(stats.average_reply_ms), timing(stats.average_total_ms))
            .arg(stats.successful)
            .arg(stats.attempted)
            .arg(jitter));
    update();
}

void channel_card::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter ||
        event->key() == Qt::Key_Space) {
        if (open_details)
            open_details(index_);
        event->accept();
    } else if (event->key() == Qt::Key_Escape) {
        if (dismiss_hover)
            dismiss_hover();
        event->accept();
    } else
        QWidget::keyPressEvent(event);
}

void channel_card::enterEvent(QEnterEvent* event) {
    QWidget::enterEvent(event);
    if (show_hover)
        show_hover(index_);
}

void channel_card::leaveEvent(QEvent* event) {
    QWidget::leaveEvent(event);
    if (leave_hover)
        leave_hover();
}

void channel_card::focusInEvent(QFocusEvent* event) {
    QWidget::focusInEvent(event);
    if (show_hover)
        show_hover(index_);
    update();
}

void channel_card::focusOutEvent(QFocusEvent* event) {
    QWidget::focusOutEvent(event);
    if (leave_hover)
        leave_hover();
    update();
}

void channel_card::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    QLinearGradient background(0, 0, width(), height());
    background.setColorAt(0, QColor("#1B282D"));
    background.setColorAt(1, QColor("#131B20"));
    QString border = "#35464A";
    double border_width = 1.;
    const auto& picks = model_.summary.picks;
    if (show_picks_) {
        if (picks.fastest == static_cast<int>(index_))
            border = "#3E716B";
        if (picks.consistent == static_cast<int>(index_))
            border = "#4F718A";
        if (model_.suggests(index_))
            border = "#9B7A4A";
        if (border != "#35464A")
            border_width = 1.5;
    }
    painter.setBrush(background);
    painter.setPen(QPen(QColor(border), border_width));
    painter.drawRoundedRect(QRectF(.75, .75, width() - 1.5, height() - 1.5), 9, 9);
    if (hasFocus()) {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor("#A7D8D0"), 2));
        painter.drawRoundedRect(QRectF(3, 3, width() - 6, height() - 6), 7, 7);
    }
    auto font = this->font();
    font.setPixelSize(13);
    font.setWeight(QFont::DemiBold);
    painter.setFont(font);
    painter.setPen(QColor("#F2F4F7"));
    painter.drawText(QRectF(14, 8, 70, 20), Qt::AlignVCenter,
                     QString("Ch. %1").arg(model_.channels[index_].number));
    const auto& stats = model_.summary.channels[index_];
    font.setPixelSize(14);
    painter.setFont(font);
    painter.setPen(QColor("#A7D8D0"));
    const auto timing_label =
        total_
            ? milliseconds(stats.average_total_ms, 0) + (stats.average_total_ms ? " ms" : "")
            : milliseconds(stats.average_connection_ms, 0) + " / " +
                  milliseconds(stats.average_reply_ms, 0) + (stats.average_reply_ms ? " ms" : "");
    if (total_)
        painter.drawText(QRectF(67, 8, width() - 81, 20), Qt::AlignRight | Qt::AlignVCenter,
                         timing_label);
    else {
        const auto connection = milliseconds(stats.average_connection_ms, 0);
        const auto reply =
            milliseconds(stats.average_reply_ms, 0) + (stats.average_reply_ms ? " ms" : "");
        const auto metrics = painter.fontMetrics();
        const auto reply_width = metrics.horizontalAdvance(reply),
                   separator_width = metrics.horizontalAdvance(" / ");
        const double reply_x = width() - 14. - reply_width;
        painter.setPen(QColor("#A7D8D0"));
        painter.drawText(QRectF(0, 8, reply_x - separator_width, 20),
                         Qt::AlignRight | Qt::AlignVCenter, connection);
        painter.setPen(QColor("#A8B0BB"));
        painter.drawText(QRectF(reply_x - separator_width, 8, separator_width, 20), Qt::AlignCenter,
                         " / ");
        painter.setPen(QColor("#A4BED9"));
        painter.drawText(QRectF(reply_x, 8, reply_width, 20), Qt::AlignRight | Qt::AlignVCenter,
                         reply);
    }
    const QRectF chart(17, 35, width() - 34, 22);
    painter.setPen(QPen(QColor("#263B3B"), 1));
    for (double fraction : {0., .5, 1.})
        painter.drawLine(QPointF(chart.left(), chart.top() + chart.height() * fraction),
                         QPointF(chart.right(), chart.top() + chart.height() * fraction));
    const double shown =
        reveal_.isValid() ? std::clamp(static_cast<double>(reveal_.elapsed()) / 100., 0., 1.) : 1.;
    auto trace = [&](bool reply) {
        QColor color(reply ? "#A4BED9" : "#91CCC1");
        QBrush brush(color);
        if (const auto& band = bands_[reply ? 1 : 0]) {
            QLinearGradient gradient(0, chart.top() + chart.height() * (1 - band->first / maximum_),
                                     0,
                                     chart.top() + chart.height() * (1 - band->second / maximum_));
            gradient.setColorAt(0, color);
            gradient.setColorAt(1, QColor("#D8B476"));
            brush = gradient;
        }
        QPen line(brush, 1.7);
        if (reply)
            line.setDashPattern({2., 2.});
        std::optional<QPointF> previous;
        int previous_round = -1;
        std::optional<QPointF> last;
        bool failed = false;
        for (const auto& sample : ordered_) {
            const double x = chart.left() +
                             chart.width() * std::clamp((sample.round - 1.) /
                                                            std::max(1, model_.target_samples - 1),
                                                        0., 1.);
            if (auto measured = timing(sample, reply)) {
                QPointF point(x, chart.top() + chart.height() *
                                                   (1 - std::clamp(*measured / maximum_, 0., 1.)));
                painter.setPen(line);
                painter.setBrush(brush);
                if (previous && previous_round == sample.round - 1) {
                    if (sample.round == newest_round_ && shown < 1.)
                        point = *previous + (point - *previous) * shown;
                    painter.drawLine(*previous, point);
                } else {
                    painter.setPen(Qt::NoPen);
                    painter.drawEllipse(point, 1.5, 1.5);
                }
                previous = last = point;
                previous_round = sample.round;
                failed = false;
            } else {
                if (!reply) {
                    painter.setPen(QPen(QColor("#EF7D7D"), 1.5));
                    painter.drawLine(QPointF(x - 2, chart.bottom() - 4),
                                     QPointF(x + 2, chart.bottom()));
                    painter.drawLine(QPointF(x - 2, chart.bottom()),
                                     QPointF(x + 2, chart.bottom() - 4));
                }
                previous.reset();
                last = QPointF(x, chart.bottom() - 2);
                failed = true;
            }
        }
        if (last && (!failed || !reply)) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(failed ? QBrush(QColor("#EF7D7D")) : brush);
            painter.drawEllipse(*last, 2.7, 2.7);
        }
    };
    if (!total_)
        trace(true);
    trace(false);
    font.setPixelSize(11);
    font.setWeight(QFont::Normal);
    painter.setFont(font);
    painter.setPen(QColor(stats.failed ? "#F1BD78" : "#A8B0BB"));
    painter.drawText(QRectF(14, 62, width() - 28, 17), Qt::AlignVCenter,
                     QString("%1/%2").arg(stats.successful).arg(stats.attempted));
    painter.setPen(QColor("#A8B0BB"));
    painter.drawText(QRectF(14, 62, width() - 28, 17), Qt::AlignRight | Qt::AlignVCenter,
                     QStringLiteral("Δ ") + variation(stats.total_jitter_ms));
}
}
