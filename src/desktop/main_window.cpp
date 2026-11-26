#include "main_window.hpp"
#ifdef CC_WINDOW_SERVICES
#include "service_flow.hpp"
#endif
#include "dialogs.hpp"
#include "channel_settings.hpp"
#include "profile_paths.hpp"
#include <QApplication>
#include <QClipboard>
#include <QCheckBox>
#include <QSignalBlocker>
#include <QCloseEvent>
#include <QDialog>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#ifdef CC_PROFILE_HISTORY
#include "wpf_migration.hpp"
#include <QFileInfo>
#endif
#include <QSaveFile>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QScreen>
#include <QHeaderView>
#include <QGraphicsOpacityEffect>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMessageBox>
#include <QPainter>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QScrollBar>
#include <QTimer>
#include <QToolTip>
#include <QTextLayout>
#include <QVBoxLayout>
#include <QWindow>
#include <algorithm>
#include <cmath>
#include <windows.h>
#include <dwmapi.h>

namespace cc::ui {
QString stylesheet() {
    return R"(
QWidget { color:#F2F4F7; font-family:'Segoe UI Variable'; font-size:13px; }
QWidget#page { background:qlineargradient(x1:0,y1:0,x2:1,y2:1,stop:0 #0F1314,stop:0.52 #111719,stop:1 #0F1314); }
QDialog { background:#0F1314; }
QLabel { background:transparent; border:0; }
QPushButton { background:#11161A; border:1px solid #454C52; border-radius:8px; padding:0px 15px; }
QPushButton:hover:active { background:#20272B; }
QPushButton:pressed:active { background:#233532; }
QPushButton:disabled { color:#7A8084; border-color:#2C3338; background:#14191D; }
QPushButton:focus, QComboBox:focus { border:2px solid #A7D8D0; }
QPushButton[primary="true"] { background:#233532; border-color:#6FAEA3; }
QPushButton[chosen="true"] { background:#233532; border-color:#6FAEA3; color:#A7D8D0; border-radius:5px; }
QComboBox { background:#11161A; border:1px solid #454C52; border-radius:5px; padding:0px 8px; }
QComboBox::drop-down { border:0; width:27px; }
QComboBox QAbstractItemView { background:#171C20; selection-background-color:#233532; }
QScrollArea { border:0; background:#0F1314; }
QScrollBar:vertical { background:#171C20; width:16px; margin:0; }
QScrollBar::handle:vertical { background:#454C52; border-radius:6px; margin:2px; min-height:24px; }
QScrollBar:horizontal { background:#171C20; height:16px; margin:0; }
QScrollBar::handle:horizontal { background:#454C52; border-radius:6px; margin:2px; min-width:24px; }
QScrollBar::add-line, QScrollBar::sub-line { width:0; height:0; }
QScrollBar::add-page, QScrollBar::sub-page { background:transparent; }
QTableWidget { background:#11161A; border:1px solid #343A3F; }
QTableWidget QScrollBar:vertical { margin-top:56px; }
QTableWidget::item:selected { background:#362F23; }
/* Qt adds its own label margin to these insets. Together they match WPF's
   compact 10x7 padding; copying those values here would count it twice. */
QToolTip { color:#F2F4F7; background:#171C20; border:1px solid #454C52; border-radius:6px; padding:1px 4px; font-size:12px; font-weight:400; }
)";
}

QLabel* label(QWidget* parent, const QString& text, int size, const QString& color, bool bold) {
    auto result = new QLabel(text, parent);
    // Scope label typography to the label itself. An unqualified local style
    // also reaches Qt's tooltip child, making summary help inherit 22px picks.
    result->setProperty("presentationLabel", true);
    result->setStyleSheet(
        QString("QLabel[presentationLabel=\"true\"] {color:%1; font-size:%2px; font-weight:%3;}")
            .arg(color)
            .arg(size)
            .arg(bold ? 600 : 400));
    result->setTextFormat(Qt::PlainText);
    return result;
}

QPushButton* button(QWidget* parent, const QString& text, const QString& name) {
    auto result = new QPushButton(text, parent);
    result->setObjectName(name);
    result->setAccessibleName(text);
    result->setFocusPolicy(Qt::StrongFocus);
    return result;
}

static QWidget* panel(QWidget* parent, const QString& color = "#171C20",
                      const QString& border = "#343A3F", int radius = 9) {
    auto result = new QWidget(parent);
    result->setObjectName("panel");
    result->setStyleSheet(
        QString("QWidget#panel { background:%1; border:1px solid %2; border-radius:%3px; }")
            .arg(color, border)
            .arg(radius));
    return result;
}

class sample_choice final : public QComboBox {
public:
    using QComboBox::QComboBox;

protected:
    void paintEvent(QPaintEvent* event) override {
        QComboBox::paintEvent(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(isEnabled() ? QColor("#F2F4F7") : QColor("#788599"));
        painter.drawPolygon(QPolygonF{QPointF(width() - 20, 16), QPointF(width() - 12, 16),
                                      QPointF(width() - 16, 21)});
    }
};

// The WPF table wraps headings instead of eliding their units. Match the
// 12-DIP cell inset, including after horizontal scrolling at narrow widths.
class measurement_header final : public QHeaderView {
public:
    explicit measurement_header(QWidget* owner) : QHeaderView(Qt::Horizontal, owner) {
        setFixedHeight(56);
        setSectionResizeMode(QHeaderView::Fixed);
    }

protected:
    void paintSection(QPainter* painter, const QRect& rect, int index) const override {
        painter->save();
        painter->fillRect(rect, QColor("#171C20"));
        painter->setPen(QColor("#343A3F"));
        painter->drawLine(rect.bottomLeft(), rect.bottomRight());
        auto heading_font = font();
        heading_font.setWeight(QFont::DemiBold);
        painter->setFont(heading_font);
        painter->setPen(QColor("#A8B0BB"));
        painter->drawText(rect.adjusted(12, 8, -12, -8),
                          Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap,
                          model()->headerData(index, Qt::Horizontal).toString());
        painter->restore();
    }
};

class measurement_cell final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& supplied,
               const QModelIndex& index) const override {
        auto option = supplied;
        initStyleOption(&option, index);
        painter->save();
        painter->fillRect(option.rect,
                          QColor(option.state & QStyle::State_Selected ? "#362F23" : "#11161A"));
        painter->setFont(option.font);
        painter->setPen(QColor("#F2F4F7"));
        painter->drawText(option.rect.adjusted(12, 8, -12, -8), Qt::AlignLeft | Qt::AlignTop,
                          option.text);
        if (option.state & QStyle::State_HasFocus) {
            painter->setPen(QPen(QColor("#A7D8D0"), 2));
            painter->drawRect(option.rect.adjusted(1, 1, -1, -1));
        }
        painter->restore();
    }
};

main_window::main_window(replay_model model, std::optional<scan_config> configuration,
                         QString channel_settings_root, QString settings_warning,
                         QString fixture_history_root, QString profile_root)
    : model_(configuration ? replay_model::ready_scan(*configuration) : std::move(model)),
      scan_config_(std::move(configuration)),
      channel_settings_root_(std::move(channel_settings_root)),
      profile_root_(std::move(profile_root)) {
    if (scan_config_ && !channel_settings_root_.isEmpty())
        throw std::invalid_argument("Settings fixture and scan modes cannot be combined.");
    if (!profile_root_.isEmpty() &&
        (!fixture_history_root.isEmpty() || !channel_settings_root_.isEmpty()))
        throw std::invalid_argument(
            "Profile storage cannot be combined with disposable fixture modes.");
    if (!fixture_history_root.isEmpty()) {
        if (!scan_config_ || !channel_settings_root_.isEmpty())
            throw std::invalid_argument(
                "Disposable history requires an explicit scan configuration.");
        history_ = std::make_unique<history_store>(fixture_history_root);
    }
    if (!profile_root_.isEmpty()) {
        profile_root_ = checked_profile_root(profile_root_);
        authorized_scan_ = scan_config_;
        default_channels_ = scan_config_
                                ? scan_config_->targets
                                : std::vector<cc::scanning::endpoint>{{1, "127.0.0.1", 40123}};
        auto selected = default_channels_;
        try {
            auto saved = channel_settings::load_profile(profile_root_);
            if (!saved.empty()) {
                validate_profile_channels(saved, authorized_scan_);
#if defined(CC_PROFILE_HISTORY) && defined(CC_BUNDLED_COMPARISON)
                // Only the exact old preset expands. Respect an edited subset or different order.
                const bool old_preset =
                    saved.size() == 13 && default_channels_.size() == 16 &&
                    std::equal(saved.begin(), saved.end(), default_channels_.begin(),
                               [](const auto& row, const auto& bundled) {
                                   return row.number == bundled.number &&
                                          row.host == bundled.host && row.port == bundled.port;
                               });
                if (!old_preset)
                    selected = std::move(saved);
#else
                selected = std::move(saved);
#endif
            }
        } catch (const std::exception& error) {
            settings_warning =
                "Saved channel list was kept but not loaded. " + QString::fromUtf8(error.what());
        }
        if (scan_config_)
            scan_config_->targets = selected;
        model_ = replay_model::ready_channels(selected, scan_config_ ? scan_config_->settings
                                                                     : cc::scanning::options{});
        history_ = std::make_unique<history_store>(profile_root_, true);
    }
    qApp->installEventFilter(this);
    setWindowTitle("Channel Checker — replay fixture");
    if (scan_config_)
        setWindowTitle("Channel Checker — loopback test");
    if (!channel_settings_root_.isEmpty())
        setWindowTitle("Channel Checker — settings fixture");
    if (!profile_root_.isEmpty())
        setWindowTitle("Channel Checker");
    // Use the executable resource directly: the Windows shell and Qt windows
    // share the original multi-size artwork without an image-format plugin.
    QIcon applicationIcon;
    for (const int size : {16, 20, 24, 32, 40, 48, 64, 128, 256}) {
        const auto icon = static_cast<HICON>(
            LoadImageW(GetModuleHandleW(nullptr), L"IDI_ICON1", IMAGE_ICON, size, size, 0));
        if (icon) {
            applicationIcon.addPixmap(QPixmap::fromImage(QImage::fromHICON(icon)));
            DestroyIcon(icon);
        }
    }
    if (applicationIcon.isNull())
        applicationIcon = QIcon(":/Assets/channel-checker.ico");
    setWindowIcon(applicationIcon);
    setMinimumSize(624, 441);
    resize(1144, 901);
    scroll_ = new QScrollArea(this);
    scroll_->setWidgetResizable(false);
    scroll_->setFocusPolicy(Qt::NoFocus);
    page_ = new QWidget;
    page_->setObjectName("page");
    scroll_->setWidget(page_);
    logo_ = new QLabel(page_);
    logo_->setPixmap(QPixmap(":/Assets/mabitools.png")
                         .scaled(48, 48, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    logo_->setAccessibleName("MabiTools");
    title_ = label(page_, "MabiTools - Channel Checker", 25, "#F2F4F7", true);
    title_->setObjectName("AppTitle");
    title_->setWordWrap(true);
    title_->setStyleSheet("font-family:'Segoe UI Variable'; font-size:25px; font-weight:700;");
    help_ = button(page_, "How it works", "HowItWorks");
    updates_ = button(page_, "Check for updates", "CheckForUpdates");
    settings_ = button(page_, "Settings", "Settings");
    settings_->setEnabled(history_ != nullptr);
    // Paint the same Lucide settings-2 paths into a Qt icon; notice remains in licenses/.
    QPixmap settings_icon(24, 24);
    settings_icon.fill(Qt::transparent);
    {
        QPainter p(&settings_icon);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(QColor("#F2F4F7"), 2, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(14, 17, 5, 17);
        p.drawLine(19, 7, 10, 7);
        p.drawEllipse(QPointF(17, 17), 3, 3);
        p.drawEllipse(QPointF(7, 7), 3, 3);
    }
    settings_->setIcon(QIcon(settings_icon));
    settings_->setIconSize({16, 16});
    options_ = panel(page_);
    results_ = panel(page_);
    edit_ = button(options_, "Channel list", "EditChannels");
    auto sample_label = label(options_, "Samples / channel", 13, "#A8B0BB");
    sample_label->setObjectName("SamplesLabel");
    sample_label->setGeometry(139, 17, 113, 34);
    samples_ = new sample_choice(options_);
    samples_->addItems({"10", "20", "40", "60", "120"});
    samples_->setCurrentText("40");
    samples_->setAccessibleName("Samples per channel");
    samples_->setObjectName("Samples");
    samples_->setGeometry(255, 15, 78, 38);
    samples_->setToolTip(
        "Choose 10, 20, 40, 60 or 120 samples per channel. More samples observe changes over a longer period.");
    sample_label->setToolTip(samples_->toolTip());
    start_ = button(options_, "Check channels", "StartScan");
    start_->setProperty("primary", true);
    cancel_ = button(options_, "Cancel", "CancelScan");
    auto result_heading = label(results_, "Results", 14, "#F2F4F7", true);
    result_heading->setGeometry(17, 12, 100, 22);
    recommendation_ = label(results_, "Ready", 20, "#A8B0BB", true);
    recommendation_->setObjectName("RecommendationStatusText");
    recommendation_->setStyleSheet(
        "background:#222628; border:1px solid #42484B; border-radius:8px; padding-left:14px; color:#A8B0BB; font-size:20px; font-weight:600;");
    const QStringList colors{"#A7D8D0", "#B7D0E2", "#F1BD78"};
    const QStringList borders{"#3E716B", "#4F718A", "#725332"};
    const QStringList headings{"Fastest connection", "Most consistent", "Suggested channel"};
    for (int i = 0; i < 3; ++i) {
        recommendations_[i] = panel(results_, i == 2 ? "#241F18" : "#11161A", borders[i], 8);
        auto heading = label(recommendations_[i], headings[i], 13, i == 2 ? colors[i] : "#A8B0BB");
        heading->setGeometry(15, 10, 260, 19);
        pick_[i] = label(recommendations_[i], "—", 22, colors[i], true);
        pick_[i]->setGeometry(15, 35, 200, 30);
        note_[i] = label(recommendations_[i], "", 12, "#A8B0BB");
        note_[i]->setGeometry(15, 68, 300, 17);
    }
    auto heading = label(page_, "Channel results", 14, "#F2F4F7", true);
    heading->setObjectName("GraphHeading");
    auto pulse = label(page_, "●", 10, "#70BD8C");
    pulse->setObjectName("ScanPulse");
    pulse->setAccessibleName("Ready");
    range_ = label(page_, "—", 12, "#A8B0BB");
    range_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    range_->setObjectName("ObservedRangeText");
    timing_group_ = panel(page_, "#11161A", "#343A3F", 7);
    paired_ = new QRadioButton("Connection / Reply", timing_group_);
    paired_->setObjectName("PairedTimingView");
    total_ = new QRadioButton("Total time", timing_group_);
    total_->setObjectName("FullTripView");
    for (auto control : {paired_, total_}) {
        control->setAccessibleName(control->text());
        control->setStyleSheet(
            "QRadioButton {font-size:12px; padding:0px 10px; border:1px solid transparent; border-radius:5px; spacing:0;} QRadioButton::indicator {width:0;height:0;} QRadioButton:checked {background:#233532; border-color:#6FAEA3; color:#A7D8D0;} QRadioButton:focus {border:2px solid #A7D8D0;}");
    }
    // Reserve both padding and the thicker focus border in either state. A
    // fixed segment width clipped the full label with Qt's font metrics.
    for (auto control : {paired_, total_}) {
        control->ensurePolished();
        control->setFixedSize(control->fontMetrics().horizontalAdvance(control->text()) + 24, 28);
    }
    paired_->move(3, 3);
    total_->move(3 + paired_->width(), 3);
    paired_->setToolTip(
        "Teal: Connection time. Dashed blue: Reply wait. Charts share a zero-based scale that adjusts to the results.");
    total_->setToolTip(
        "Connection time and Reply wait combined. Charts share a zero-based scale that adjusts to the results.");
    details_ = button(page_, "Details", "ToggleDetails");
    details_->setStyleSheet("padding:0px 12px;");
    status_ = label(page_, "", 12, "#A8B0BB");
    status_->setObjectName("StatusText");
    progress_ = label(page_, "0%", 15, "#A7D8D0", true);
    progress_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    progress_bar_ = new scan_progress_bar(page_);
    progress_bar_->setObjectName("ScanProgressBar");
    progress_bar_->setAccessibleName("Scan progress");
    progress_bar_->setRange(0, 100);
    progress_bar_->setTextVisible(false);
    progress_bar_->setStyleSheet(
        "QProgressBar {border:0; padding:1px 0px; background:#11161A;} QProgressBar::chunk {background:#6FAEA3; border-radius:2px;}");
    activity_ = new QScrollArea(page_);
    activity_->setWidgetResizable(false);
    activity_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    activity_->setFocusPolicy(Qt::NoFocus);
    activity_->setStyleSheet("QScrollArea {background:transparent; border:0;}");
    activity_->viewport()->setAutoFillBackground(false);
    activity_->setAccessibleName("Live channel charts");
    activity_->setObjectName("ChannelCardScroll");
    card_grid_ = new QWidget;
    card_grid_->setObjectName("ChannelCardGrid");
    card_grid_->setAutoFillBackground(false);
    activity_->setWidget(card_grid_);
    card_grid_->setAutoFillBackground(false);
    activity_->viewport()->setAutoFillBackground(false);
    table_ = new QTableWidget(0, 10, page_);
    table_->setAccessibleName("Channel measurements");
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->verticalHeader()->hide();
    table_->setHorizontalHeaderLabels({"", "Channel", "Connection (ms)", "Jitter (ms)",
                                       "Connection P95", "Reply wait (ms)", "Total time (ms)",
                                       "Successful checks", "Failed", "Status"});
    table_->setSortingEnabled(false);
    table_->setHorizontalHeader(new measurement_header(table_));
    table_->setItemDelegate(new measurement_cell(table_));
    table_->setShowGrid(false);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    const int column_widths[]{80, 90, 114, 114, 114, 114, 110, 114, 80, 134};
    for (int i = 0; i < 10; ++i)
        table_->setColumnWidth(i, column_widths[i]);
    connect(table_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        if (row >= 0)
            show_details(static_cast<std::size_t>(row));
    });
    auto endpoint = label(page_, QString("Custom channel list · %1 channels").arg(cards_.size()),
                          13, "#A8B0BB");
    endpoint->setObjectName("EndpointSummary");
    copy_ = button(page_, "Copy report", "CopyReport");
    export_ = button(page_, "Export…", "ExportReport");
    feedback_ = label(page_, "", 12, "#A8B0BB");
    feedback_->setObjectName("ReportFeedbackText");
#ifdef CC_PROFILE_HISTORY
    auto version = label(
        page_, QString("Version: %1").arg(QString::fromLatin1(application_version)), 12, "#A8B0BB");
#else
    auto version = label(page_, "Version: 2026.11.23", 12, "#A8B0BB");
#endif
    version->setObjectName("VersionText");
    auto platform = label(page_, "Windows x64", 12, "#A8B0BB");
    platform->setObjectName("PlatformText");
    platform->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    connect(paired_, &QRadioButton::clicked, this, [this] {
        total_view_ = false;
        refresh();
    });
    connect(total_, &QRadioButton::clicked, this, [this] {
        total_view_ = true;
        refresh();
    });
    connect(details_, &QPushButton::clicked, this, [this] {
        details_view_ = !details_view_;
        refresh();
    });
    connect(start_, &QPushButton::clicked, this, [this] { start_scan(); });
    connect(cancel_, &QPushButton::clicked, this, [this] { cancel_scan(); });
    connect(help_, &QPushButton::clicked, this, [this] { show_dialog("help"); });
    connect(settings_, &QPushButton::clicked, this, [this] { show_dialog("settings-on"); });
    connect(edit_, &QPushButton::clicked, this, [this] { show_dialog("channels"); });
    updates_->setText("Updates disabled");
    updates_->setEnabled(false);
    connect(updates_, &QPushButton::clicked, this, [this] {
#ifdef CC_WINDOW_SERVICES
        if (window_services_) {
            update_clicked();
            return;
        }
#endif
    });
    connect(copy_, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setText(model_.text_report());
        feedback_->setText("Report copied to clipboard.");
    });
    connect(export_, &QPushButton::clicked, this, [this] {
        QString filter = "JSON report (*.json)";
        auto stamp = model_.document["Result"].toObject()["CompletedAtUtc"].toString().left(19);
        stamp.remove('-');
        stamp.remove(':');
        stamp.replace('T', '-');
        const auto path =
            QFileDialog::getSaveFileName(this, "Export report", "MabiTools-Channels-" + stamp,
                                         "JSON report (*.json);;Text report (*.txt)", &filter);
        if (path.isEmpty())
            return;
        if (!write_report(path, filter.startsWith("JSON"))) {
            QMessageBox::warning(this, "Export failed", "The report could not be saved.");
            return;
        }
        feedback_->setText("Report saved.");
    });
    rebuild_channel_views();
    BOOL dark = TRUE;
    DwmSetWindowAttribute(reinterpret_cast<HWND>(winId()), 20, &dark, sizeof(dark));
    refresh();
    if (!profile_root_.isEmpty() && !scan_config_) {
        start_->setEnabled(false);
        start_->setToolTip("No scan configuration is available.");
        if (settings_warning.isEmpty())
            settings_warning =
                "No scan configuration is available. Launch normally to use the bundled channel list.";
    }
    if (!settings_warning.isEmpty())
        feedback_->setText(settings_warning);
    if (scan_config_)
        samples_->setCurrentText(QString::number(scan_config_->settings.samples));
    scan_poll_ = new QTimer(this);
    scan_poll_->setInterval(10);
    connect(scan_poll_, &QTimer::timeout, this, [this] { finish_scan(); });
    if (history_) {
        history_poll_ = new QTimer(this);
        history_poll_->setInterval(20);
        connect(history_poll_, &QTimer::timeout, this, [this] {
            refresh_history_settings();
            if (closing_ && !scan_running_ && history_->view().pending == 0)
                close();
        });
        history_poll_->start();
    }
}

main_window::~main_window() {
    qApp->removeEventFilter(this);
    closing_ = true;
    ++scan_generation_;
    scan_worker_.request_stop();
    // QObject children and queued-callback receiver remain alive until join.
    if (scan_worker_.joinable())
        scan_worker_.join();
    if (history_)
        history_->drain();
#ifdef CC_WINDOW_SERVICES
    if (window_services_)
        window_services_->close();
#endif
}

void main_window::refresh_table() {
    for (int row = 0; row < table_->rowCount(); ++row) {
        const auto index = static_cast<std::size_t>(row);
        const auto& s = model_.summary.channels[index];
        const QStringList values{
            QString::number(model_.channels[index].number),
            milliseconds(s.average_connection_ms),
            milliseconds(s.total_jitter_ms),
            milliseconds(s.p95_connection_ms),
            milliseconds(s.average_reply_ms),
            milliseconds(s.average_total_ms),
            QString("%1/%2").arg(s.successful).arg(s.attempted),
            QString::number(s.failed),
            model_.state == "ready" ? "Not checked" : QString::fromUtf8(cc::status_text(s.status))};
        for (int column = 0; column < values.size(); ++column)
            table_->item(row, column + 1)->setText(values[column]);
    }
}

bool main_window::write_report(const QString& path, bool as_json) const {
    const auto payload = as_json ? model_.original_json : model_.text_report().toUtf8();
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(payload) == payload.size() &&
           file.commit();
}

void main_window::apply_channels(std::vector<cc::scanning::endpoint> targets) {
    if (scan_running_ || closing_)
        throw std::runtime_error("Wait for the current check to finish before changing channels.");
#ifdef CC_WINDOW_SERVICES
    if (window_services_ && window_services_->scan() != cc::service_flow::scan_status::idle)
        throw std::runtime_error("Wait for the current check to finish before changing channels.");
#endif
    if (!profile_root_.isEmpty())
        validate_profile_channels(targets, authorized_scan_);
    const auto persist = [&] {
        if (!profile_root_.isEmpty())
            channel_settings::save_profile(profile_root_, targets);
        else if (!channel_settings_root_.isEmpty())
            channel_settings::save(channel_settings_root_, targets);
    };
    bool unchanged = targets.size() == model_.channels.size();
    for (std::size_t i = 0; unchanged && i < targets.size(); ++i) {
        const auto& target = targets[i];
        unchanged = model_.endpoint(i) == QString::number(target.number) + ',' +
                                              QString::fromStdString(target.host) + ',' +
                                              QString::number(target.port);
    }
    if (unchanged) {
        // Explicit Apply can save unchanged defaults or replace a corrupt fixture.
        persist();
        return; // Applying the same list must retain the completed scan.
    }
    auto settings = scan_config_ ? scan_config_->settings : cc::scanning::options{};
    settings.samples = samples_->currentText().toInt();
    auto ready = replay_model::ready_channels(targets, settings);
    persist();
    if (hover_dialog_)
        hover_dialog_->close();
    hover_dialog_.clear();
    if (details_dialog_)
        details_dialog_->close();
    details_dialog_.clear();
    ++scan_generation_; // Any obsolete queued progress belongs to the old list.
    clear_channel_views();
    model_ = std::move(ready);
    if (scan_config_)
        scan_config_->targets = std::move(targets);
    cancel_requested_ = false;
    scan_error_.clear();
    feedback_->clear();
    rebuild_channel_views();
    refresh();
    refresh_update_status();
}

void main_window::clear_channel_views() {
    // Deleting a focused card can synchronously transfer focus to another old
    // card. Disable every callback before any deletion or model replacement.
    for (auto card : cards_) {
        card->open_details = {};
        card->show_hover = {};
        card->leave_hover = {};
        card->dismiss_hover = {};
    }
    for (int row = 0; row < table_->rowCount(); ++row)
        if (auto cell = table_->cellWidget(row, 0))
            for (auto view : cell->findChildren<QPushButton*>())
                disconnect(view, nullptr, this, nullptr);
    for (auto card : cards_)
        delete card;
    cards_.clear();
    table_->setRowCount(0);
}

void main_window::rebuild_channel_views() {
    clear_channel_views();
    table_->setRowCount(static_cast<int>(model_.channels.size()));
    for (std::size_t i = 0; i < model_.channels.size(); ++i) {
        auto card = new channel_card(model_, i, card_grid_);
        card->open_details = [this](std::size_t index) { show_details(index); };
        card->show_hover = [this](std::size_t index) { show_hover(index); };
        card->leave_hover = [this] { leave_hover(); };
        card->dismiss_hover = [this] {
            if (hover_dialog_)
                hover_dialog_->close();
        };
        cards_.push_back(card);
        const auto row = static_cast<int>(i);
        auto cell = new QWidget(table_);
        auto inset = new QHBoxLayout(cell);
        inset->setContentsMargins(12, 8, 12, 8);
        inset->setSpacing(0);
        auto view = button(cell, "View", "ViewChannelDetails");
        view->setAccessibleName("View channel scan and history details");
        view->setFixedSize(56, 28);
        view->setStyleSheet("padding:3px 8px;");
        inset->addWidget(view, 0, Qt::AlignLeft);
        connect(view, &QPushButton::clicked, this, [this, i] { show_details(i); });
        table_->setCellWidget(row, 0, cell);
        for (int column = 1; column < 10; ++column)
            table_->setItem(row, column, new QTableWidgetItem);
        table_->setRowHeight(row, 44);
    }
    // Rebuild the existing tab order when the channel count changes. No stale
    // card index may survive an applied list with fewer channels.
    std::vector<QWidget*> order{help_,   updates_, settings_, edit_,  samples_,
                                cancel_, start_,   paired_,   total_, details_};
    for (auto card : cards_)
        order.push_back(card);
    order.push_back(copy_);
    order.push_back(export_);
    for (std::size_t i = 1; i < order.size(); ++i)
        setTabOrder(order[i - 1], order[i]);
    page_->findChild<QLabel*>("EndpointSummary")
        ->setText(QString("Custom channel list · %1 channels").arg(cards_.size()));
}

bool main_window::eventFilter(QObject* object, QEvent* event) {
    auto widget = qobject_cast<QWidget*>(object);
    // QObject ownership includes separate Settings and confirmation windows;
    // QWidget::isAncestorOf stops at a window boundary.
    bool owned = false;
    for (auto* ancestor = object; ancestor; ancestor = ancestor->parent())
        if (ancestor == this) {
            owned = true;
            break;
        }
    if ((object == qApp && event->type() == QEvent::ApplicationStateChange &&
         qApp->applicationState() != Qt::ApplicationActive) ||
        (owned && widget && widget->isWindow() && event->type() == QEvent::WindowDeactivate)) {
        if (hover_dialog_)
            hover_dialog_->close();
        QToolTip::hideText();
        if (cursor_action_)
            cursor_action_->unsetCursor();
        cursor_action_.clear();
    }
    if (widget && owned) {
        // The pointer can leave the panel directly for another control without
        // crossing its channel card again. Observe both sides of that transition.
        if (hover_dialog_ && hover_dialog_->isVisible() &&
            (event->type() == QEvent::Enter || event->type() == QEvent::Leave ||
             event->type() == QEvent::MouseMove || event->type() == QEvent::HoverMove))
            leave_hover();
        if (!cursor_sync_pending_ &&
            (event->type() == QEvent::Enter || event->type() == QEvent::Leave ||
             event->type() == QEvent::EnabledChange || event->type() == QEvent::WindowActivate)) {
            cursor_sync_pending_ = true;
            QTimer::singleShot(0, this, [this] {
                cursor_sync_pending_ = false;
                auto* under = QApplication::widgetAt(QCursor::pos());
                // Layout changes and popup transitions need not deliver Leave
                // to the old control. Keep only the actual hovered action's
                // cursor, so later enable changes cannot affect empty space.
                if (cursor_action_ && (cursor_action_ != under || !under->isActiveWindow())) {
                    cursor_action_->unsetCursor();
                    cursor_action_.clear();
                }
                if (!under || !under->isActiveWindow())
                    return;
                bool owned_under = false;
                for (auto* parent = static_cast<QObject*>(under); parent; parent = parent->parent())
                    if (parent == this) {
                        owned_under = true;
                        break;
                    }
                if (!owned_under)
                    return;
                if (qobject_cast<QAbstractButton*>(under) || qobject_cast<QComboBox*>(under)) {
                    cursor_action_ = under;
                    under->setCursor(Qt::PointingHandCursor);
                }
                // Qt can retain an ancestor or another window in its cursor
                // cache. Reconcile only the actual hit-tested widget after
                // enable/activation events, including a stationary pointer.
                if (auto* native = under->window()->windowHandle())
                    native->setCursor(under->isEnabled() ? under->cursor()
                                                         : QCursor(Qt::ArrowCursor));
            });
        }
        // Only empty layout surfaces dismiss focus. Editors, table rows and
        // buttons retain their normal mouse behavior and keyboard focus chain.
        if (event->type() == QEvent::MouseButtonPress &&
            static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton &&
            (widget == page_ || widget == options_ || widget == results_ || widget == card_grid_ ||
             widget == timing_group_ || (activity_ && widget == activity_->viewport()) ||
             (scroll_ && widget == scroll_->viewport())) &&
            !widget->childAt(static_cast<QMouseEvent*>(event)->position().toPoint())) {
            if (auto focused = QApplication::focusWidget(); focused && focused->window() == this)
                focused->clearFocus();
            if (hover_dialog_)
                hover_dialog_->close();
        }
    }
    if (object == table_ && event->type() == QEvent::KeyPress) {
        const auto key = static_cast<QKeyEvent*>(event)->key();
        if ((key == Qt::Key_Return || key == Qt::Key_Enter) && table_->currentRow() >= 0) {
            show_details(static_cast<std::size_t>(table_->currentRow()));
            return true;
        }
    }
    return QWidget::eventFilter(object, event);
}

void main_window::refresh(bool animate_progress) {
    const bool running = model_.state == "active";
    const bool partial = running || model_.state == "cancelled";
    const bool ready = model_.state == "ready";
    const auto& picks = model_.summary.picks;
    const bool picked = !partial && !ready && picks.overall >= 0;

    // Partial scans can show observations, but not a final recommendation.
    if (!picked && result_fade_.isValid()) {
        result_fade_.invalidate();
        for (auto* card : recommendations_)
            card->setGraphicsEffect(nullptr);
    }
    recommendation_->setVisible(!picked);
    for (auto card : recommendations_)
        card->setVisible(picked);
    recommendation_->setText(ready     ? "Ready"
                             : running ? "Scanning…"
                             : partial ? "Scan incomplete"
                                       : "Not enough data");
    if (picked) {
        const auto fastest = static_cast<std::size_t>(picks.fastest);
        const auto consistent = static_cast<std::size_t>(picks.consistent);
        pick_[0]->setText(QString("Ch. %1").arg(model_.channels[fastest].number));
        pick_[1]->setText(QString("Ch. %1").arg(model_.channels[consistent].number));
        pick_[2]->setText(model_.suggested_label());
        note_[0]->setText(milliseconds(model_.summary.channels[fastest].average_connection_ms) +
                          " ms average this scan");
        note_[1]->setText(milliseconds(model_.summary.channels[consistent].total_jitter_ms) +
                          " ms jitter");
        note_[2]->setText(model_.suggested_note());
    }
    int attempted_checks = 0, eligible_channels = 0;
    for (const auto& channel : model_.summary.channels) {
        attempted_checks += channel.attempted;
        if (channel.eligible())
            ++eligible_channels;
    }
    const auto completed_status =
        eligible_channels == 0 ? QString("No channels responded reliably. Check your connection.")
        : eligible_channels == 1
            ? QString("Only one channel had enough successful checks for a comparison.")
        : !picked ? QString("Not enough comparable successful checks. Try a longer scan.")
                  : QString("Checked at %1 UTC · %2 channels checked")
                        .arg(model_.completed_time())
                        .arg(cards_.size());
    status_->setText(
        ready     ? ""
        : running ? QString("Performing checks… · %1 of %2 checks complete")
                        .arg(attempted_checks)
                        .arg(model_.target_samples * static_cast<int>(cards_.size()))
        : partial
            ? "Scan cancelled. Partial measurements are shown; run a complete scan for recommendations."
            : completed_status);
    if (scan_config_ && running && warming_up_)
        status_->setText("Performing checks…");
    if (!profile_root_.isEmpty() && !scan_config_)
        status_->setText("Offline. No network checks are enabled.");
    if (!scan_error_.isEmpty()) {
        recommendation_->setText("Couldn't check channels");
        status_->setText("Scan failed: " + scan_error_);
    }
    const double measured = measured_percent();
    progress_bar_->set_measured(measured, animate_progress && running && !cancel_requested_);
    progress_->setText(QString::number(progress_bar_->value()) + "%");
    pick_[0]->setToolTip(
        picks.fastest_status == cc::recommendation_status::clear_lead
            ? "Clear lead in both halves of the scan."
            : "Lowest measured average; a small difference may not matter in play.");
    pick_[1]->setToolTip(
        picks.consistent_status == cc::recommendation_status::clear_lead
            ? "Clear lead in both halves of the scan."
            : "Lowest measured jitter; a small difference may not matter in play.");
    pick_[2]->setToolTip("Favours successful checks, then balances total time and jitter.");
    updates_->setEnabled(false);
    start_->setEnabled(!running && (profile_root_.isEmpty() || scan_config_.has_value()));
    edit_->setEnabled(!running);
    samples_->setEnabled(!running);
    cancel_->setEnabled(running);
    copy_->setEnabled(!partial && !ready);
    export_->setEnabled(!partial && !ready);
    if (scan_config_ && cancel_requested_)
        cancel_->setEnabled(false);
#ifdef CC_WINDOW_SERVICES
    if (window_services_)
        render_window_services();
#if defined(CC_PROFILE_HISTORY) && defined(CC_BUNDLED_COMPARISON)
    edit_->setToolTip("Choose or reorder channels from the bundled NA Erinn list.");
#endif
#endif
    for (auto control : {paired_, total_}) {
        control->setProperty("chosen", control == (total_view_ ? total_ : paired_));
        control->setChecked(control == (total_view_ ? total_ : paired_));
        control->style()->unpolish(control);
        control->style()->polish(control);
    }
    details_->setText(details_view_ ? "Charts" : "Details");
    table_->setVisible(details_view_);
    activity_->setVisible(!details_view_);
    std::vector<double> totals, connections, replies;
    for (const auto& channel : model_.channels)
        for (const auto& sample : channel.samples)
            if (sample.total_ms && !sample.has_failure) {
                totals.push_back(*sample.total_ms);
                if (sample.connection_ms) {
                    connections.push_back(*sample.connection_ms);
                    replies.push_back(*sample.total_ms - *sample.connection_ms);
                }
            }
    auto range = [](const std::vector<double>& values) {
        return values.empty() ? QString("—")
                              : compact(*std::min_element(values.begin(), values.end())) + "–" +
                                    compact(*std::max_element(values.begin(), values.end()));
    };
    const auto total_range = range(totals) + (totals.empty() ? "" : " ms");
    const auto paired_range = range(connections) + " / " + range(replies) + " ms";
    range_->ensurePolished();
    range_->setProperty("requiredWidth",
                        std::max(range_->fontMetrics().horizontalAdvance(total_range),
                                 range_->fontMetrics().horizontalAdvance(paired_range)));
    range_->setText(total_view_ ? total_range : paired_range);
    range_->setToolTip(
        total_view_
            ? "Total time range: " + range_->text() +
                  ". Minimum and maximum individual successful measurements. Warm-up and failed checks excluded."
            : "Connection time / Reply wait ranges: " + range_->text() +
                  ". Warm-up and failed checks excluded.");
    range_->setAccessibleName(range_->toolTip());
    double maximum = 0;
    const auto& values = total_view_ ? totals : connections;
    for (double v : values)
        maximum = std::max(maximum, v);
    if (!total_view_)
        for (double v : replies)
            maximum = std::max(maximum, v);
    for (auto card : cards_) {
        card->setVisible(!details_view_);
        card->set_view(total_view_, std::max(10., std::ceil(maximum / 10) * 10), picked,
                       animate_progress && !details_view_);
    }
    if (animate_progress || progress_bar_->fill_in_motion() || result_fade_.isValid()) {
        if (!motion_timer_) {
            motion_timer_ = new QTimer(this);
            constexpr int frame_interval_ms = 16;
            motion_timer_->setInterval(frame_interval_ms);
            connect(motion_timer_, &QTimer::timeout, this, &main_window::tick_motion);
        }
        if (!motion_timer_->isActive())
            motion_timer_->start();
    }
    refresh_table();
    arrange();
}

double main_window::measured_percent() const {
    if (model_.state == "ready")
        return 0.;
    if (model_.state != "active" && model_.state != "cancelled")
        return 100.;
    int attempted_checks = 0;
    for (const auto& channel : model_.summary.channels)
        attempted_checks += channel.attempted;
    const double planned =
        static_cast<double>(model_.target_samples) * static_cast<double>(model_.channels.size());
    return planned > 0. ? 100. * static_cast<double>(attempted_checks) / planned : 0.;
}

void main_window::unavailable_in_fixture(const QString& feature) {
    QMessageBox::information(this, "Unavailable", feature + " is unavailable in this mode.");
}

void main_window::show_details(std::size_t index) {
    if (index >= model_.channels.size())
        return;
    if (hover_dialog_)
        hover_dialog_->close();
    if (details_dialog_)
        details_dialog_->close();
#ifdef CC_PROFILE_HISTORY
    load_channel_history(index);
#endif
    details_dialog_ = detail_dialog(this, model_, index);
    details_dialog_->show();
}

void main_window::show_hover(std::size_t index) {
    if (closing_ || !isActiveWindow() || qApp->applicationState() != Qt::ApplicationActive ||
        index >= model_.channels.size() || index >= cards_.size())
        return;
    if (hover_dialog_ && hover_dialog_->isVisible() && hovered_index_ == index)
        return;
    if (hover_dialog_)
        hover_dialog_->close();
    if (hover_leave_timer_)
        hover_leave_timer_->stop();
#ifdef CC_PROFILE_HISTORY
    load_channel_history(index);
#endif
    hovered_index_ = index;
    hover_dialog_ = detail_dialog(this, model_, index, false);
    // Realize the frameless panel before positioning its native frame origin.
    // QWidget::move can retain the original dialog's caption offset on Windows;
    // QWindow's explicit frame position anchors the actual panel to the tile.
    hover_dialog_->show();
    const auto card = cards_[index];
    auto position = card->mapToGlobal(QPoint(0, card->height() + 3));
    const auto available = screen()->availableGeometry();
    position.setX(
        std::clamp(position.x(), available.left(),
                   std::max(available.left(), available.right() - hover_dialog_->width() + 1)));
    if (position.y() + hover_dialog_->height() > available.bottom())
        position.setY(card->mapToGlobal(QPoint()).y() - hover_dialog_->height() - 3);
    position.setY(std::max(available.top(), position.y()));
    hover_dialog_->windowHandle()->setFramePosition(position);
}

void main_window::leave_hover() {
    if (!hover_leave_timer_) {
        hover_leave_timer_ = new QTimer(this);
        hover_leave_timer_->setSingleShot(true);
        connect(hover_leave_timer_, &QTimer::timeout, this, [this] {
            if (!hover_dialog_ || !hover_dialog_->isVisible())
                return;
            const auto position = QCursor::pos();
            auto* under = QApplication::widgetAt(position);
            const bool over_card =
                hovered_index_ < cards_.size() && under &&
                (under == cards_[hovered_index_] || cards_[hovered_index_]->isAncestorOf(under));
            if (!isActiveWindow() || qApp->applicationState() != Qt::ApplicationActive ||
                (!hover_dialog_->geometry().contains(position) && !over_card))
                hover_dialog_->close();
        });
    }
    // Allow the small gap between card and panel, without extending the grace
    // period indefinitely while the pointer moves over an unrelated control.
    if (!hover_leave_timer_->isActive())
        hover_leave_timer_->start(100);
}

void main_window::refresh_update_status() {
#ifdef CC_WINDOW_SERVICES
    if (window_services_) {
        render_window_services();
        return;
    }
#endif
    updates_->setText("Updates disabled");
    updates_->setEnabled(false);
}

void capture_widget(QWidget& widget, const QString& output) {
    // WPF's reference canvas rounds fractional physical extents upward. QWidget
    // grab() rounds to nearest; use the same canvas without changing logical size.
    const auto scale = widget.devicePixelRatioF();
    QImage image(static_cast<int>(std::ceil(widget.width() * scale)),
                 static_cast<int>(std::ceil(widget.height() * scale)),
                 QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(scale);
    image.fill(QColor("#0F1314"));
    QPainter painter(&image);
    widget.render(&painter);
    painter.end();
    if (!image.save(output))
        throw std::runtime_error("Cannot save window capture");
}

void main_window::capture(const QString& output) {
    if (auto focused = QApplication::focusWidget())
        focused->clearFocus();
    capture_widget(*this, output);
}
}
