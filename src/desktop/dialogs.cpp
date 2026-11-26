#include "dialogs.hpp"
#include "main_window.hpp"
#include "cc_history.hpp"
#include "cc_endpoints.hpp"
#include <QFile>
#include <QGridLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QScrollBar>
#include <QScreen>
#include <QStyleOptionButton>
#include <QTextDocument>
#include <QTextBlock>
#include <QAbstractTextDocumentLayout>
#include <QVBoxLayout>
#include <cmath>
#include <windows.h>
#include <dwmapi.h>

namespace cc::ui {
namespace {
QDialog* dialog(QWidget* owner, const QString& title, QSize size, QSize minimum) {
    auto result = new QDialog(owner);
    result->setWindowTitle("Channel Checker — " + title);
    result->setWindowIcon(owner->windowIcon());
    result->resize(size);
    result->setMinimumSize(minimum);
    result->setAttribute(Qt::WA_DeleteOnClose);
    result->setModal(false);
    BOOL dark = TRUE;
    DwmSetWindowAttribute(reinterpret_cast<HWND>(result->winId()), 20, &dark, sizeof(dark));
    return result;
}

QWidget* surface(QWidget* owner, const QString& background = "#171C20") {
    auto result = new QWidget(owner);
    result->setObjectName("surface");
    result->setStyleSheet(
        QString("QWidget#surface {background:%1; border:1px solid #343A3F; border-radius:9px;}")
            .arg(background));
    return result;
}

QLabel* paragraph(QWidget* owner, const QString& text, int size = 12) {
    auto result = label(owner, text, size, "#A8B0BB");
    result->setWordWrap(true);
    return result;
}

QPushButton* close_button(QDialog* owner) {
    auto close = button(owner, "Close", "CloseDialog");
    close->setFixedSize(100, 38);
    QObject::connect(close, &QPushButton::clicked, owner, &QDialog::close);
    return close;
}

void size_channel_action(QPushButton* control) {
    control->setFixedHeight(38);
    control->ensurePolished();
    QStyleOptionButton focused;
    focused.initFrom(control);
    focused.state |= QStyle::State_HasFocus;
    if (control->autoDefault())
        focused.features |= QStyleOptionButton::AutoDefaultButton;
    const auto text = control->fontMetrics().size(Qt::TextShowMnemonic, control->text());
    const auto focused_size =
        control->style()->sizeFromContents(QStyle::CT_PushButton, &focused, text, control);
    // The focus border also needs room. QPushButton caches its ordinary size
    // hint, so a fixed width or that hint alone can clip the first glyph.
    control->setMinimumWidth(qMax(control->sizeHint().width(), focused_size.width()));
}

// QTextDocument is Qt's rich-text layout, not a browser or web view. Fixed line
// height matches WPF's paragraphs while preserving inline emphasis and wrapping.
class help_paragraph final : public QWidget {
public:
    help_paragraph(const QJsonObject& data, QWidget* owner) : QWidget(owner) {
        auto font = this->font();
        font.setPixelSize(data["FontSize"].toInt());
        font.setWeight(data["Bold"].toBool() ? QFont::Bold : QFont::Normal);
        document_.setDefaultFont(font);
        document_.setDocumentMargin(0);
        document_.setHtml(data["Html"].toString());
        auto cursor = QTextCursor(&document_);
        cursor.select(QTextCursor::Document);
        QTextBlockFormat format;
        const auto line_height = data["LineHeight"].toDouble();
        if (line_height > 0)
            format.setLineHeight(line_height, QTextBlockFormat::FixedHeight);
        cursor.mergeBlockFormat(format);
        setAccessibleName(document_.toPlainText());
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    }

    bool hasHeightForWidth() const override { return true; }

    int heightForWidth(int width) const override {
        document_.setTextWidth(width);
        return static_cast<int>(std::ceil(document_.size().height()));
    }

    QSize sizeHint() const override { return {300, heightForWidth(300)}; }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        document_.setTextWidth(width());
        QAbstractTextDocumentLayout::PaintContext context;
        context.palette.setColor(QPalette::Text, QColor("#F2F4F7"));
        document_.documentLayout()->draw(&p, context);
    }

private:
    mutable QTextDocument document_;
};

class history_scatter final : public QWidget {
public:
    history_scatter(const history_replay& history, QWidget* owner)
        : QWidget(owner), history_(history) {
        const auto& summary = history_.summary;
        const bool missing = summary.timing_scans != static_cast<int>(summary.indices.size());
        setFixedHeight(missing ? 124 : summary.indices.size() == 1 ? 76 : 102);
        setAccessibleName(
            QString(
                "Saved scan observations: %1 scans; %2 with timings. Each point is a scan mean; amber diamonds include failed checks; crosses in the No timing lane have no successful timing. No continuous monitoring.")
                .arg(summary.indices.size())
                .arg(summary.timing_scans));
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QFont font("Segoe UI");
        font.setPixelSize(11);
        p.setFont(font);
        const auto& summary = history_.summary;
        if (summary.indices.empty())
            return;
        double maximum = 10;
        for (auto index : summary.indices)
            maximum = std::max(
                maximum,
                std::ceil(history_.rows[static_cast<std::size_t>(index)].mean_total_ms.value_or(0) /
                          10) *
                    10);
        const bool missing = summary.timing_scans != static_cast<int>(summary.indices.size());
        const double left = 40, top = 8, plot_width = width() - 48.,
                     bottom = height() - (missing ? 48. : 26.), plot_height = bottom - top;
        for (double fraction : {0., .5, 1.}) {
            double y = bottom - plot_height * fraction;
            p.setPen(QColor("#343A3F"));
            p.drawLine(QPointF(left, y), QPointF(left + plot_width, y));
            p.setPen(QColor("#A8B0BB"));
            p.drawText(QRectF(0, y - 8, 33, 16), Qt::AlignRight | Qt::AlignVCenter,
                       compact(maximum * fraction));
        }
        const auto
            first =
                history_.rows[static_cast<std::size_t>(summary.indices.front())].completed_ticks,
            last = history_.rows[static_cast<std::size_t>(summary.indices.back())].completed_ticks;
        const auto date_format =
            history_replay::date(first).date() == history_replay::date(last).date() ? "HH:mm"
                                                                                    : "d MMM";
        p.drawText(QRectF(first == last ? left + plot_width / 2 - 16 : left, bottom + 4, 90, 16),
                   history_replay::date(first).toString(date_format));
        if (last != first)
            p.drawText(QRectF(left + plot_width - 90, bottom + 4, 90, 16), Qt::AlignRight,
                       history_replay::date(last).toString(date_format));
        if (missing)
            p.drawText(QRectF(0, height() - 17, 80, 16), "No timing");
        for (auto index : summary.indices) {
            const auto& row = history_.rows[static_cast<std::size_t>(index)];
            double x = left + plot_width * (last == first
                                                ? .5
                                                : static_cast<double>(row.completed_ticks - first) /
                                                      static_cast<double>(last - first));
            if (!row.mean_total_ms) {
                p.setPen(QPen(QColor("#F1BD78"), 1.4));
                p.drawLine(QPointF(x - 3, height() - 14), QPointF(x + 3, height() - 8));
                p.drawLine(QPointF(x - 3, height() - 8), QPointF(x + 3, height() - 14));
                continue;
            }
            const double y = bottom - plot_height * *row.mean_total_ms / maximum;
            p.setPen(QPen(QColor(row.failed ? "#F1BD78" : "#A7D8D0"), 1.4));
            p.setBrush(row.failed ? Qt::NoBrush : QBrush(QColor("#A7D8D0")));
            if (row.failed)
                p.drawPolygon(QPolygonF{QPointF(x, y - 4), QPointF(x + 4, y), QPointF(x, y + 4),
                                        QPointF(x - 4, y)});
            else
                p.drawEllipse(QPointF(x, y), 3, 3);
        }
    }

private:
    const history_replay& history_;
};
}

QDialog* help_dialog(QWidget* owner) {
    auto window = dialog(owner, "How it works", {624, 811}, {434, 341});
    auto layout = new QVBoxLayout(window);
    layout->setContentsMargins(22, 18, 22, 18);
    layout->setSpacing(16);
    auto title = label(window, "How Channel Checker works", 25, "#F2F4F7", true);
    title->setStyleSheet("font-size:25px; font-weight:700;");
    title->setFixedHeight(34);
    layout->addWidget(title);
    auto card = surface(window);
    auto card_layout = new QVBoxLayout(card);
    card_layout->setContentsMargins(16, 16, 16, 16);
    // Let QScrollArea own viewport geometry. Expanding its viewport from a
    // resize callback fights its height-for-width layout at scrollbar thresholds
    // and can trap the UI thread in repeated text layout. The normal scrollbar
    // already reserves the 16-pixel right gutter; do not subtract it twice.
    auto scroll = new QScrollArea(card);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet("QScrollArea {background:transparent; border:0;}");
    auto body = new QWidget;
    auto body_layout = new QVBoxLayout(body);
    body_layout->setContentsMargins(0, 0, 0, 0);
    body_layout->setSpacing(0);
    QFile source(":/help-content.json");
    if (!source.open(QIODevice::ReadOnly))
        throw std::runtime_error("Missing Help copy");
    int index = 0;
    for (const auto& item : QJsonDocument::fromJson(source.readAll()).array()) {
        auto data = item.toObject();
        body_layout->addSpacing(data["Top"].toInt());
        body_layout->addWidget(new help_paragraph(data, body));
        body_layout->addSpacing(data["Bottom"].toInt());
        if (index++ == 0) {
            body_layout->addSpacing(6);
            auto line = new QWidget(body);
            line->setFixedHeight(1);
            line->setStyleSheet("background:#343A3F;");
            body_layout->addWidget(line);
            body_layout->addSpacing(18);
        }
    }
    body_layout->addStretch();
    scroll->setWidget(body);
    body->setAutoFillBackground(false);
    scroll->viewport()->setAutoFillBackground(false);
    card_layout->addWidget(scroll);
    layout->addWidget(card, 1);
    layout->addWidget(close_button(window), 0, Qt::AlignRight);
    return window;
}

QDialog* channel_dialog(
    QWidget* owner, const replay_model& model,
    const std::function<void(std::vector<cc::scanning::endpoint>)>& apply_channels,
    bool fixture_backed, const QStringList& default_channels) {
#if defined(CC_PROFILE_HISTORY) && defined(CC_BUNDLED_COMPARISON)
    const auto restored_message = QStringLiteral(
        "Bundled NA Erinn channels restored to the draft. Choose Apply to use them.");
#else
    const auto restored_message =
        QStringLiteral("Loopback defaults restored to the draft. Choose Apply to save.");
#endif
    auto window = dialog(owner,
                         !default_channels.isEmpty() ? "Channel list"
                         : fixture_backed            ? "Channel list — disposable settings fixture"
                                                     : "Channel list — session only",
                         {484, 641}, {484, 381});
#if defined(CC_PROFILE_HISTORY) && defined(CC_BUNDLED_COMPARISON)
    window->setWindowTitle("Channel Checker — Channel list");
#endif
    auto layout = new QVBoxLayout(window);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(0);
    layout->addWidget(label(window, "Channel list", 21, "#F2F4F7", true));
    layout->addSpacing(8);
#if defined(CC_PROFILE_HISTORY) && defined(CC_BUNDLED_COMPARISON)
    layout->addWidget(paragraph(
        window,
        "Keep or reorder bundled NA Erinn rows. New addresses are not enabled in this build.", 13));
#else
    layout->addWidget(
        paragraph(window, "One channel per line: channel number, IP address, port.", 13));
#endif
    layout->addSpacing(16);

    auto input = new QPlainTextEdit(window);
    input->setAccessibleName("Channel addresses");
    input->setObjectName("ChannelListText");
    input->setTabChangesFocus(true);
    input->setStyleSheet(
        "QPlainTextEdit {background:#11161A; border:1px solid #454C52; padding:11px 15px; font-family:Consolas; font-size:16px;}");
    input->document()->setDocumentMargin(0);
    input->setLineWrapMode(QPlainTextEdit::NoWrap);
    QStringList rows;
    for (std::size_t i = 0; i < model.channels.size(); ++i)
        rows << model.endpoint(i);
    input->setPlainText(rows.join('\n'));
    layout->addWidget(input, 1);
    layout->addSpacing(12);

    auto footer = new QGridLayout;
    footer->setHorizontalSpacing(16);
    footer->setVerticalSpacing(8);
    auto status = paragraph(window, "");
    status->setObjectName("ChannelListError");
    status->hide();
    footer->addWidget(status, 0, 0, 2, 1, Qt::AlignVCenter);
    footer->setColumnStretch(0, 1);

    auto restore = button(window, "Restore default list", "RestoreChannels");
    size_channel_action(restore);
    footer->addWidget(restore, 0, 1, Qt::AlignRight);
    restore->setToolTip(default_channels.isEmpty()
                            ? QStringLiteral("Restore the bundled NA Erinn list to the draft.")
                            : restored_message);

    auto actions = new QHBoxLayout;
    actions->setSpacing(8);
    actions->addStretch();
    auto cancel = button(window, "Cancel", "CancelChannelEdit");
    auto apply = button(window, "Apply", "SaveChannels");
    apply->setStyleSheet("background:#362F23; border-color:#D99032;");
    size_channel_action(cancel);
    size_channel_action(apply);
    actions->addWidget(cancel);
    actions->addWidget(apply);
    footer->addLayout(actions, 1, 1);
    layout->addLayout(footer);

    QObject::connect(cancel, &QPushButton::clicked, window, &QDialog::reject);
    QObject::connect(apply, &QPushButton::clicked, window, [window, input, status, apply_channels] {
        try {
            auto channels = cc::catalog::parse(input->toPlainText().toUtf8().toStdString());
            // Validation is independent of connection permission. Apply updates
            // the selected channels; Check channels still enforces the endpoint policy.
            apply_channels(std::move(channels));
            window->accept();
        } catch (const std::exception& error) {
            status->setText(QString::fromUtf8(error.what()));
            status->setStyleSheet("color:#EF7D7D;");
            status->show();
        }
    });
    QObject::connect(
        restore, &QPushButton::clicked, window,
        [input, status, normal_style = status->styleSheet(), default_channels, restored_message] {
            if (!default_channels.isEmpty()) {
                input->setPlainText(default_channels.join('\n'));
                status->setStyleSheet(normal_style);
                status->setText(restored_message);
                status->show();
                return;
            }
            QFile source(":/erinn-na.csv");
            if (source.open(QIODevice::ReadOnly)) {
                QStringList lines;
                for (auto line : QString::fromUtf8(source.readAll()).split('\n'))
                    if (!line.trimmed().isEmpty() && !line.trimmed().startsWith('#'))
                        lines << line.trimmed();
                input->setPlainText(lines.join('\n'));
                status->setStyleSheet(normal_style);
                status->setText("Default list restored. Choose Apply to use it.");
                status->show();
            }
        });
    return window;
}

QDialog* detail_dialog(QWidget* owner, const replay_model& model, std::size_t index,
                       bool expanded) {
    auto window =
        dialog(owner, QString("Ch. %1").arg(model.channels[index].number), {464, 721}, {364, 361});
    auto layout = new QVBoxLayout(window);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(12);
    auto card = surface(window);
    auto outer = new QVBoxLayout(card);
    outer->setContentsMargins(17, 17, 17, 17);
    auto scroll = new QScrollArea(card);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet("QScrollArea {background:transparent; border:0;}");
    // WPF reserves a small right gutter inside the rich panel. This is separate
    // from the compact summary ToolTip and its much smaller padding.
    auto body = new QWidget;
    auto content = new QVBoxLayout(body);
    content->setContentsMargins(0, 0, 16, 0);
    content->setSpacing(0);
    auto row = [&](const QString& name, const QString& value, bool heading = false,
                   bool warning = false, bool subdued = false) {
        auto line = new QHBoxLayout;
        line->setContentsMargins(0, heading ? 0 : 3, 0, 3);
        line->setSpacing(12);
        line->addWidget(label(card, name, 13, heading ? "#F2F4F7" : "#A8B0BB", heading));
        line->addStretch();
        line->addWidget(label(card, value, heading ? 12 : 13,
                              warning              ? "#F1BD78"
                              : heading || subdued ? "#A8B0BB"
                                                   : "#F2F4F7"));
        content->addLayout(line);
    };
    auto divider = [&] {
        auto line = new QWidget(card);
        line->setFixedHeight(1);
        line->setStyleSheet("background:#343A3F;");
        content->addSpacing(10);
        content->addWidget(line);
        content->addSpacing(12);
    };
    row(QString("Ch. %1 · This scan").arg(model.channels[index].number), "Times in ms", true);
    const auto summary = cc::summarize_history_channel(model.channels[index].samples);
    content->addSpacing(9);
    auto timing = new QGridLayout;
    timing->setContentsMargins(0, 5, 0, 8);
    timing->setVerticalSpacing(10);
    timing->setColumnStretch(0, 1);
    timing->setColumnMinimumWidth(1, 90);
    timing->setColumnMinimumWidth(2, 120);
    const QStringList headers{"Timing", "Average", "Range"};
    for (int col = 0; col < 3; ++col)
        timing->addWidget(label(card, headers[col], 12, "#A8B0BB"), 0, col,
                          col == 0 ? Qt::AlignLeft : Qt::AlignRight);
    const cc::timing_summary values[]{summary.connection, summary.reply, summary.total};
    const QStringList names{"Connection", "Reply wait", "Total"};
    for (int i = 0; i < 3; ++i) {
        timing->addWidget(label(card, names[i]), i + 1, 0);
        timing->addWidget(label(card, values[i].mean ? compact(*values[i].mean) : "—"), i + 1, 1,
                          Qt::AlignRight);
        timing->addWidget(label(card, values[i].minimum ? compact(*values[i].minimum) + "–" +
                                                              compact(*values[i].maximum)
                                                        : "—"),
                          i + 1, 2, Qt::AlignRight);
    }
    content->addLayout(timing);
    divider();
    row("Jitter", variation(summary.total_variation));
    row("Successful checks", QString("%1/%2").arg(summary.successful).arg(summary.attempted));
    row("Failed checks",
        QString::number(summary.failed) + " · " +
            (summary.attempted ? compact(100. * summary.failed / summary.attempted) + "%" : "—"),
        false, summary.failed > 0, summary.failed == 0);
    divider();
    const bool has_history = model.saved_history &&
                             model.endpoint(index) == model.saved_history->endpoint_csv &&
                             !model.saved_history->summary.indices.empty();
    const auto history_period = model.saved_history && model.saved_history->summary.period ==
                                                           cc::history_period::maintenance
                                    ? "Since last confirmed maintenance"
                                    : "Past seven days";
    if (has_history) {
        const auto& history = *model.saved_history;
        const auto& saved = history.summary;
        row("History",
            QString("%1 scans · %2 with timings").arg(saved.indices.size()).arg(saved.timing_scans),
            true);
        content->addWidget(paragraph(card, history_period));
        content->addSpacing(10);
        content->addWidget(paragraph(card, "Mean total (ms)"));
        content->addSpacing(2);
        content->addWidget(new history_scatter(history, card));
        content->addSpacing(4);
        content->addWidget(paragraph(card, "● Saved scan     ◇ Includes failed checks"));
        content->addSpacing(10);
        row("Typical total",
            saved.typical_total_ms ? compact(*saved.typical_total_ms) + " ms" : "—");
        row("Typical jitter", variation(saved.typical_variation_ms));
        row("Observed checks", QString("%1/%2").arg(saved.successful).arg(saved.attempted));
        row("Failed checks",
            QString::number(saved.failed) + " · " +
                compact(std::round(1000. * static_cast<double>(saved.failed) /
                                   static_cast<double>(saved.attempted)) /
                        10) +
                "%",
            false, saved.failed > 0, saved.failed == 0);
        const auto last = history_replay::date(
            history.rows[static_cast<std::size_t>(saved.indices.back())].completed_ticks);
        const int offset = last.offsetFromUtc();
        const auto zone = QString(" %1%2:%3")
                              .arg(offset < 0 ? "-" : "+")
                              .arg(std::abs(offset) / 3600, 2, 10, QChar('0'))
                              .arg(std::abs(offset) % 3600 / 60, 2, 10, QChar('0'));
        row("Last checked",
            expanded ? last.toString("yyyy-MM-dd HH:mm:ss") + zone : last.toString("d MMM, HH:mm"));
        content->addWidget(paragraph(card, QString("%1 with jitter").arg(saved.variation_scans)));
    } else {
        row("History", "No history yet", true);
        content->addWidget(paragraph(card, history_period));
    }
    if (expanded) {
        content->addSpacing(12);
        content->addWidget(
            paragraph(card, model.endpoint(index).section(',', 1).replace(',', ':')));
    }
    content->addStretch();
    scroll->setWidget(body);
    body->setAutoFillBackground(false);
    scroll->viewport()->setAutoFillBackground(false);
    outer->addWidget(scroll);
    layout->addWidget(card, 1);
    if (expanded)
        layout->addWidget(close_button(window), 0, Qt::AlignRight);
    else {
        window->setWindowFlags(Qt::ToolTip | Qt::FramelessWindowHint);
        window->setAttribute(Qt::WA_ShowWithoutActivating);
        layout->setContentsMargins(0, 0, 0, 0);
        window->setMinimumSize(280, 240);
        // Fit the actual content rather than leaving a fixed blank tail. Keep
        // scrolling available when the work area cannot hold the whole panel.
        const auto available = owner->screen()->availableGeometry();
        const int width = std::min(430, std::max(280, available.width() - 32));
        const int content_height = content->hasHeightForWidth()
                                       ? content->totalHeightForWidth(width - 34)
                                       : content->sizeHint().height();
        window->resize(width,
                       std::min(content_height + 34, std::max(240, available.height() - 66)));
    }
    return window;
}
}
