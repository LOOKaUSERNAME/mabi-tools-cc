// Exercise the actual widgets with owned loopback HTTP/probe servers and
// disposable profiles. Server counters detect traffic across the scan barrier;
// filesystem assertions cover consent and persistence. Native cursor checks
// are opt-in because they move the real pointer on an interactive desktop.

#include "main_window.hpp"
#include "widget_style.hpp"
#include "fixture_http_server.hpp"
#include "service_http.hpp"
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QImage>
#include <QJsonDocument>
#include <QTest>
#include <QAbstractItemView>
#include <QCheckBox>
#include <QMessageBox>
#include <QScreen>
#include <QCursor>
#include <QStyleOptionButton>
#include <QStyle>
#include <windows.h>
#undef near
#undef far
#include <iostream>
#include <algorithm>
#include <stdexcept>
#ifdef CC_PROFILE_HISTORY
#include "profile_paths.hpp"
#include "channel_settings.hpp"
#include <QPlainTextEdit>
#include <QTemporaryDir>

namespace cc::ui {
struct main_window_test_access {
    static bool export_json(const main_window& w, const QString& path) {
        return w.write_report(path, true);
    }

    static QString maintenance(const main_window& w) { return w.maintenance_status_; }

    static void update_result(main_window& w) { w.show_update_result(); }

    static void hover(main_window& w) { w.show_hover(0); }

    static bool hover_visible(const main_window& w) {
        return w.hover_dialog_ && w.hover_dialog_->isVisible();
    }

    static QDialog* hover_panel(const main_window& w) { return w.hover_dialog_.data(); }
};
}
#endif

namespace {
using namespace cc::service_flow;
using namespace cc::service_fixtures;

void check(bool condition, const char* problem) {
    if (!condition)
        throw std::runtime_error(problem);
}

template <class T> T* control(QWidget& window, const char* name) {
    auto* widget = window.findChild<T*>(name);
    check(widget != nullptr, "Missing actual window control.");
    return widget;
}

template <class F> void until(F condition, const char* problem, int deadline = 8'000) {
    QElapsedTimer time;
    time.start();
    while (!condition()) {
        check(time.elapsed() < deadline, problem);
        QTest::qWait(5);
    }
}

QByteArray body(const QJsonArray& corpus, const QString& id) {
    for (const auto& value : corpus) {
        const auto row = value.toObject();
        if (row["id"] == id)
            return QJsonDocument(row["document"].toObject()).toJson(QJsonDocument::Compact);
    }
    throw std::runtime_error("Missing fixture document.");
}

struct screen_case {
    fixture_server server;
    service_http wire;
    coordinator flow;
    QTcpServer probes;
    QElapsedTimer clock;
    const QDateTime epoch = QDateTime::fromString("2026-11-20T10:00:00Z", Qt::ISODate);
    qint64 extra{};
    int probe_count{};
    bool overlap{};
    QTimer tick;
    cc::ui::scan_config config;
#ifdef CC_PROFILE_HISTORY
    QTemporaryDir profile;
    QString profile_root;
#endif
    std::unique_ptr<cc::ui::main_window> window;

    screen_case(const QByteArray& update, const QByteArray& maintenance,
                const QString& version = "2026.11.23")
        : server(update, maintenance), wire(server.listener.serverPort()), flow(wire, version) {
        check(probes.listen(QHostAddress(QHostAddress::LocalHost), 0),
              "Could not bind loopback scan fixture.");
        QObject::connect(&probes, &QTcpServer::newConnection, &probes, [this] {
            while (auto* socket = probes.nextPendingConnection()) {
                ++probe_count;
                overlap = overlap || wire.live_requests() != 0 || server.connected != 0 ||
                          flow.scan() != scan_status::running;
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                socket->write(QByteArray("test", 4));
                socket->disconnectFromHost();
            }
        });
        clock.start();
        wire.bind(flow, [this] { advance(); });
        QObject::connect(&tick, &QTimer::timeout, &tick, [this] { advance(); });
        advance();
        tick.start(5);
        config.targets = {{1, "127.0.0.1", probes.serverPort()}};
        config.settings = {10, 250, 250, 1};
#ifdef CC_PROFILE_HISTORY
        check(profile.isValid(), "No disposable application profile.");
        profile_root = cc::ui::open_profile_root_under(profile.path());
        window = std::make_unique<cc::ui::main_window>(cc::ui::replay_model{}, config, QString{},
                                                       QString{}, QString{}, profile_root);
#else
        window = std::make_unique<cc::ui::main_window>(cc::ui::replay_model{}, config);
#endif
        window->attach_window_services(flow);
        window->show();
    }

    void advance() {
        const auto now = clock.elapsed() + extra;
        flow.advance(now, epoch.addMSecs(now));
    }

    QPushButton* button(const char* name) { return control<QPushButton>(*window, name); }

    QString text(const char* name) { return control<QLabel>(*window, name)->text(); }

    QString maintenance() { return cc::ui::main_window_test_access::maintenance(*window); }

    bool has_installer_ui() {
        return window->findChild<QWidget*>("UpdateBanner") ||
               window->findChild<QPushButton*>("InstallUpdate");
    }
};
#ifdef CC_PROFILE_HISTORY
void expect_native_cursor(int resource, const char* problem) {
    const auto expected = LoadCursorW(nullptr, MAKEINTRESOURCEW(resource));
    check(expected != nullptr, "Missing Windows system cursor.");
    until(
        [&] {
            CURSORINFO info{};
            info.cbSize = sizeof(info);
            return GetCursorInfo(&info) && info.hCursor == expected;
        },
        problem);
}

void point_at(QWidget* widget, const QString& stage = {}) {
    // Settings can scroll at higher display scaling. Move the actual Windows
    // pointer only after the target is visible, using Qt's DPI conversion.
    for (auto* parent = widget->parentWidget(); parent; parent = parent->parentWidget())
        if (auto* scroll = qobject_cast<QScrollArea*>(parent))
            scroll->ensureWidgetVisible(widget, 8, 8);
    QTest::qWait(20);
    const auto point = widget->mapToGlobal(widget->rect().center());
    check(widget->screen()->availableGeometry().contains(point),
          "Cursor target is outside the work area.");
    QCursor::setPos(point);
    QTest::qWait(30);
    try {
        until(
            [&] {
                auto* hit = QApplication::widgetAt(point);
                return hit && (hit == widget || widget->isAncestorOf(hit));
            },
            "Cursor target remained obscured.");
    } catch (const std::runtime_error&) {
        const auto rect = [](const QRect& value) {
            return QString("[%1,%2 %3x%4]")
                .arg(value.x())
                .arg(value.y())
                .arg(value.width())
                .arg(value.height());
        };
        auto* hit = QApplication::widgetAt(point);
        auto* active = QApplication::activeWindow();
        const auto screenshot =
            QDir(QCoreApplication::arguments().at(2)).filePath("cursor-target-obscured.png");
        const bool captured = widget->screen()->grabWindow(0).save(screenshot);
        QString problem =
            "Cursor target obscured: " + widget->objectName() + " (" +
            widget->metaObject()->className() + "); hit: " +
            (hit ? hit->objectName() + " (" + hit->metaObject()->className() + ")" : "none") +
            "; stage: " + stage + "; active window: " + (active ? active->windowTitle() : "none") +
            QString("; target point: [%1,%2]; actual pointer: [%3,%4]; target rect: ")
                .arg(point.x())
                .arg(point.y())
                .arg(QCursor::pos().x())
                .arg(QCursor::pos().y()) +
            rect(QRect(widget->mapToGlobal(QPoint()), widget->size())) +
            "; screenshot: " + screenshot + (captured ? "" : " (capture failed)");
        for (auto* top : QApplication::topLevelWidgets())
            problem +=
                "\n  top-level: " + top->windowTitle() + " (" + top->metaObject()->className() +
                ") " + rect(top->frameGeometry()) +
                QString("; visible=%1; active=%2").arg(top->isVisible()).arg(top->isActiveWindow());
        throw std::runtime_error(problem.toStdString());
    }
}

void no_unhovered_action_cursors(QWidget& owner) {
    std::string problem;
    try {
        // Windows pointer movement and the queued cursor reconciliation may
        // arrive after widgetAt already reports the new position.
        until(
            [&] {
                const auto* hit = QApplication::widgetAt(QCursor::pos());
                for (auto* widget : owner.findChildren<QWidget*>())
                    if ((qobject_cast<QAbstractButton*>(widget) ||
                         qobject_cast<QComboBox*>(widget)) &&
                        widget != hit && !(hit && widget->isAncestorOf(hit)))
                        if (widget->testAttribute(Qt::WA_SetCursor)) {
                            problem = ("Unhovered action retained a native cursor: " +
                                       widget->objectName() +
                                       "; pointer target: " + (hit ? hit->objectName() : "none"))
                                          .toStdString();
                            return false;
                        }
                return true;
            },
            "An unhovered action retained its cursor after pointer movement.");
    } catch (const std::runtime_error&) {
        throw std::runtime_error(problem);
    }
}

void channel_action_labels(cc::ui::main_window& window) {
    auto* channels = window.show_dialog("channels");
    channels->resize(channels->minimumSize());
    channels->activateWindow();
    check(QTest::qWaitForWindowActive(channels), "Channel list could not activate.");
    for (const char* name : {"CancelChannelEdit", "SaveChannels", "RestoreChannels"}) {
        auto* action = control<QPushButton>(*channels, name);
        for (const bool focused : {false, true}) {
            if (focused)
                action->setFocus(Qt::OtherFocusReason);
            else
                action->clearFocus();
            QTest::qWait(10);
            QStyleOptionButton option;
            option.initFrom(action);
            option.text = action->text();
            option.rect = action->rect();
            if (focused)
                option.state |= QStyle::State_HasFocus;
            else
                option.state &= ~QStyle::State_HasFocus;
            const auto contents =
                action->style()->subElementRect(QStyle::SE_PushButtonContents, &option, action);
            const auto metrics = action->fontMetrics();
            check(contents.width() >= std::max(metrics.horizontalAdvance(action->text()),
                                               metrics.boundingRect(action->text()).width()) &&
                      contents.height() >= metrics.height(),
                  "Channel-list action label is clipped by its padding or focus border.");
            check(
                channels->rect().contains(QRect(action->mapTo(channels, QPoint()), action->size())),
                "Channel-list action extends outside the minimum-width dialog.");
        }
    }
    channels->close();
    QTest::qWait(30);
}

void interaction_policy(const QByteArray& update, const QByteArray& maintenance,
                        bool native_cursors) {
    using access = cc::ui::main_window_test_access;
    const auto version =
        QJsonDocument::fromJson(update).object()["release"].toObject()["version"].toString();
    screen_case h(update, maintenance, version);
    h.window->enable_profile_settings();
    h.flow.request_update();
    until(
        [&] {
            return h.flow.updates() == update_status::current &&
                   h.button("CheckForUpdates")->text() == "Up to date";
        },
        "Current-version result did not reach the update button.");
    bool interrupted = false;
    QTimer guard;
    QObject::connect(&guard, &QTimer::timeout, &guard, [&] {
        if (auto* modal = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            interrupted = true;
            modal->reject();
        }
    });
    guard.start(10);
    access::update_result(*h.window);
    QTest::qWait(30);
    check(!interrupted, "A current-version check interrupted the player with a modal.");
    check(!h.window->findChild<QLabel*>("UpdateNotice") &&
              !h.window->findChild<QLabel*>("MaintenanceNotice"),
          "Background service prose remains on the main screen.");
    h.button("CheckForUpdates")->click();
    until(
        [&] {
            return h.flow.updates() == update_status::current &&
                   h.button("CheckForUpdates")->isEnabled();
        },
        "The Up to date button could not check again.");
    auto popup_outside = [](QComboBox* combo) {
        combo->showPopup();
        until([&] { return combo->view()->window()->isVisible(); },
              "Dropdown did not finish opening.");
        const QRect field(combo->mapToGlobal(QPoint()), combo->size());
        auto* popup = combo->view()->window();
        if (!popup->isVisible() || popup->frameGeometry().intersects(field))
            std::cerr << "Dropdown " << combo->objectName().toStdString() << " field " << field.x()
                      << ',' << field.y() << ',' << field.width() << ',' << field.height()
                      << " popup " << popup->frameGeometry().x() << ','
                      << popup->frameGeometry().y() << ',' << popup->width() << ','
                      << popup->height() << " visible=" << popup->isVisible() << '\n';
        check(popup->isVisible() && !popup->frameGeometry().intersects(field),
              "Dropdown covers its original field.");
        check(combo->screen()->availableGeometry().contains(popup->frameGeometry()),
              "Dropdown extends outside the work area.");
        combo->hidePopup();
    };
    h.window->activateWindow();
    check(QTest::qWaitForWindowActive(h.window.get()), "Interaction window could not activate.");
    auto* samples = control<QComboBox>(*h.window, "Samples");
    popup_outside(samples);
    auto* settings = h.window->show_dialog("settings-on");
    settings->activateWindow();
    check(QTest::qWaitForWindowActive(settings), "Settings could not activate.");
    until([&] { return control<QComboBox>(*settings, "HistoryAgeLimit")->isEnabled(); },
          "History controls did not become available.");
    for (const char* name : {"HistoryAgeLimit", "HistoryStorageLimit", "OpenHistoryFolder",
                             "AutomaticMaintenance", "ClearSavedHistory", "CloseHistorySettings"}) {
        auto* widget = control<QWidget>(*settings, name);
        check(widget->isEnabled(), "Settings control did not become enabled.");
        if (native_cursors) {
            point_at(widget);
            expect_native_cursor(32649,
                                 "Enabled Settings control lacks the actual Windows hand cursor.");
            no_unhovered_action_cursors(*h.window);
        }
    }
    auto* age = control<QComboBox>(*settings, "HistoryAgeLimit");
    if (native_cursors)
        point_at(age);
    popup_outside(age);
    auto* budget = control<QComboBox>(*settings, "HistoryStorageLimit");
    if (native_cursors)
        point_at(budget);
    popup_outside(budget);
    if (native_cursors) {
        auto* close = control<QPushButton>(*settings, "CloseHistorySettings");
        point_at(close);
        expect_native_cursor(32649, "Hovered Close button lacks the Windows hand cursor.");
        close->setEnabled(false);
        expect_native_cursor(32512, "Disabled button kept the Windows hand cursor.");
        close->setEnabled(true);
        expect_native_cursor(32649,
                             "Stationary pointer did not regain the hand when Close re-enabled.");
        // Enabling main-window actions while an owned dialog has the pointer must
        // neither assign them a cursor nor replace the dialog's cursor.
        h.button("ExportReport")->setEnabled(true);
        h.button("CopyReport")->setEnabled(true);
        QTest::qWait(30);
        no_unhovered_action_cursors(*h.window);
        expect_native_cursor(32649, "An offscreen action changed the hovered Settings cursor.");
    }
    auto* path = control<QPlainTextEdit>(*settings, "HistoryFolderPath");
    check(path->viewport()->cursor().shape() == Qt::IBeamCursor,
          "Copyable history path lost its text cursor.");
    if (native_cursors) {
        point_at(path->viewport());
        expect_native_cursor(32513, "Copyable history path lost its Windows text cursor.");
    }
    settings->close();
    QTest::qWait(30);
    h.window->activateWindow();
    check(QTest::qWaitForWindowActive(h.window.get()), "Main window did not reactivate.");
    channel_action_labels(*h.window);
    h.window->activateWindow();
    check(QTest::qWaitForWindowActive(h.window.get()),
          "Main window did not return from the Channel list.");
    if (native_cursors) {
        auto* page = control<QWidget>(*h.window, "page");
        const auto blank = page->mapToGlobal(QPoint(15, 79));
        QCursor::setPos(blank);
        QTest::qWait(30);
        until(
            [&] {
                auto* hit = QApplication::widgetAt(blank);
                return hit && hit->window() == h.window.get();
            },
            "Blank-page cursor target is not on the visible main window.");
        expect_native_cursor(32512, "Blank main page did not use the Windows arrow cursor.");
        no_unhovered_action_cursors(*h.window);
        for (int scan = 0; scan < 2; ++scan) {
            // Keep the pointer on blank space throughout enable/disable changes.
            h.button("StartScan")->click();
            until([&] { return h.window->scan_running(); },
                  "Cursor regression scan did not start.");
            expect_native_cursor(32512, "Scan startup leaked an action cursor onto blank space.");
            until(
                [&] { return !h.window->scan_running() && h.window->model().state == "completed"; },
                "Cursor regression scan did not finish.");
            check(QCursor::pos() == blank, "Cursor moved during the stationary scan test.");
            expect_native_cursor(32512, "Completed scan left a hand cursor on blank space.");
            no_unhovered_action_cursors(*h.window);
        }
    }
    auto* card = control<QWidget>(*h.window, "Channel1");
    if (native_cursors) {
        point_at(h.button("HowItWorks"),
                 "keyboard focus: park on How it works before opening card panel");
        card->clearFocus();
        QTest::qWait(180);
        const auto parked = QCursor::pos();
        card->setFocus(Qt::TabFocusReason);
        until([&] { return access::hover_visible(*h.window); },
              "Keyboard focus did not open the channel panel.");
        QTest::qWait(180);
        check(QCursor::pos() == parked && access::hover_visible(*h.window),
              "Keyboard-opened channel panel closed without pointer movement.");
        h.button("HowItWorks")->setFocus(Qt::TabFocusReason);
        point_at(h.button("HowItWorks"), "dismiss keyboard-opened panel before mouse hover cases");
        until([&] { return !access::hover_visible(*h.window); },
              "Channel panel remained after keyboard focus left the card.");
        for (const char* name : {"HowItWorks", "Settings"}) {
            // Retained keyboard focus must not pin a mouse hover after the
            // pointer leaves both the channel card and its details panel.
            card->clearFocus();
            // The panel can cover the card on a short display, so hit-testing
            // the card after pointer entry would reject a valid open panel.
            QCursor::setPos(card->mapToGlobal(card->rect().center()));
            until([&] { return access::hover_visible(*h.window); },
                  "Channel hover did not open from actual pointer entry.");
            card->setFocus(Qt::OtherFocusReason);
            QPointer<QDialog> panel = access::hover_panel(*h.window);
            point_at(panel.data(), QString("inside card panel before moving to ") + name);
            QTest::qWait(180);
            check(panel && access::hover_visible(*h.window) &&
                      access::hover_panel(*h.window) == panel.data(),
                  "Channel hover closed while the pointer remained inside its panel.");
            auto* target = h.button(name);
            point_at(target, QString("move directly from card panel to ") + name);
            QTest::qWait(180);
            if (access::hover_visible(*h.window))
                throw std::runtime_error(
                    (QString("Channel hover persisted after moving directly from its panel to ") +
                     name)
                        .toStdString());
            expect_native_cursor(
                32649, "Main action lost its Windows hand cursor after leaving the hover panel.");
        }
    }
    card->setFocus();
    access::hover(*h.window);
    check(access::hover_visible(*h.window), "Channel hover did not open.");
    QWidget other;
    other.setWindowTitle("Interaction test");
    other.show();
    other.activateWindow();
    check(QTest::qWaitForWindowActive(&other), "Other window could not activate.");
    QTest::qWait(150);
    check(!access::hover_visible(*h.window), "Channel hover survived window deactivation.");
    access::hover(*h.window);
    check(!access::hover_visible(*h.window), "An inactive window reopened its hover panel.");
    other.close();
    h.window->activateWindow();
    check(QTest::qWaitForWindowActive(h.window.get()),
          "Main window did not return after switching away.");
    access::hover(*h.window);
    check(access::hover_visible(*h.window), "Hover did not recover after reactivation.");
    std::cout
        << "PASS window interactions: unclipped channel actions, current update without modal, anchored dropdowns and hover dismissal.\n";
    if (native_cursors)
        std::cout
            << "PASS native Windows cursors: Settings controls, stationary re-enable, unhovered actions, blank-page arrow through two scans, keyboard-opened channel panel and pointer dismissal on How it works/Settings.\n";
}

// Exercise the real disk queue with dated reports and controlled byte pressure.
// Waiting for pending work separates admission/publication from retention consent;
// later fixtures test protected files and metadata-only startup independently.
void retention_policy() {
    QTemporaryDir scratch;
    check(scratch.isValid(), "Missing disposable retention directory.");
    const auto root = cc::ui::open_profile_root_under(scratch.path());
    using store = cc::ui::history_store;
    const auto ready = [](const store& history) {
        until(
            [&] {
                const auto state = history.view();
                return state.loaded && state.pending == 0;
            },
            "History worker did not settle.");
    };
    store history(root, true);
    ready(history);
    check(history.view().retention == store::limits{90, 100}, "Unexpected history defaults.");
    check(history.apply_limits(history.preview_limits({0, 500})),
          "Could not set expanded retention.");
    ready(history);
    const auto recent = QDateTime::currentDateTimeUtc();
    const auto stamp = [&](int days) { return recent.addDays(days).toString(Qt::ISODateWithMs); };
    QJsonArray observations;
    for (int round = 1; round <= 10; ++round)
        observations.append(QJsonObject{{"Round", round}, {"LatencyMs", 10.0}, {"ConnectMs", 7.0}});
    const QJsonObject endpoint{{"Channel", 1}, {"Host", "127.0.0.1"}, {"Port", 3456}};
    const QJsonArray channels{QJsonObject{{"Endpoint", endpoint}, {"Samples", observations}}};
    const QJsonObject options{
        {"Samples", 10}, {"TimeoutMs", 250}, {"IntervalMs", 250}, {"MaxConcurrency", 1}};
    const auto make_report = [&](int days, int bytes) {
        return QJsonDocument(QJsonObject{{"SchemaVersion", 7},
                                         {"FixturePadding", QString(bytes, 'x')},
                                         {"Result", QJsonObject{{"StartedAtUtc", stamp(days)},
                                                                {"CompletedAtUtc", stamp(days)},
                                                                {"Channels", channels},
                                                                {"Options", options}}}})
            .toJson(QJsonDocument::Compact);
    };
    const auto seed = [&](int id, int days, const QByteArray& report) {
        const auto hex = QString("%1").arg(id, 32, 16, QLatin1Char('0'));
        const auto path = QDir(history.folder()).filePath("native-scan-" + hex + ".json");
        QFile file(path);
        const auto raw = QJsonDocument(QJsonObject{{"Format", "MabiTools.Qt.CandidateHistory.1"},
                                                   {"Id", hex},
                                                   {"MeasurementPolicy",
                                                    "native-deadline-observed-before-cutoff"},
                                                   {"Report", QString::fromUtf8(report)}})
                             .toJson(QJsonDocument::Compact);
        check(file.open(QIODevice::WriteOnly) && file.write(raw) == raw.size() && file.flush() &&
                  file.setFileTime(recent.addDays(days), QFileDevice::FileModificationTime),
              "Could not write disposable original report.");
        return raw;
    };

    const auto first = seed(1, -40, make_report(-40, 0));
    const auto keep = seed(2, 0, make_report(0, 0));
    const auto age = history.preview_limits({30, 500});
    check(age.affected.size() == 1 && age.bytes == first.size(),
          "Age preview chose the wrong scan.");
    const auto bad_target = QDir(root).filePath("native-history/export.json");
    bool refused = false;
    try {
        (void)history.export_affected(age, bad_target);
    } catch (const std::runtime_error&) {
        refused = true;
    }
    check(refused && history.preview_limits({30, 500}).affected == age.affected,
          "An export error changed the active scans.");
    const auto target = QDir(scratch.path()).filePath("history-export.json");
    check(history.export_affected(age, target), "Export of original records failed.");
    QFile exported(target);
    check(exported.open(QIODevice::ReadOnly) && exported.readAll().contains(first),
          "Export lost original scan bytes.");
    exported.close();
    check(history.preview_limits({30, 500}).affected == age.affected,
          "Export deleted a live record.");
    // A second writer adds an eligible old scan after export, before confirmation.
    (void)seed(3, -35, make_report(-35, 0));
    check(!history.apply_limits(age) && QFile::exists(target) &&
              history.preview_limits({30, 500}).affected.size() == 2,
          "Changed deletion set did not require a new confirmation.");
    check(history.apply_limits(history.preview_limits({30, 500})), "Cannot apply a reviewed plan.");
    ready(history);
    check(history.view().count == 1 && history.view().bytes == keep.size() &&
              history.view().retention == store::limits{30, 500},
          "Wrong age-only result.");
    {
        store reopened(root, true);
        ready(reopened);
        check(reopened.view().retention == store::limits{30, 500} && reopened.view().count == 1,
              "Native limits did not survive reopening.");
    }
    check(history.apply_limits(history.preview_limits({0, 500})), "No-age selection failed.");
    ready(history);
    const auto bulky = make_report(-45, 2 * 1024 * 1024);
    for (int file = 10; file < 63; ++file)
        (void)seed(file, -45, bulky);
    const auto byte = history.preview_limits({0, 100});
    check(!byte.affected.empty() && !byte.over_budget && history.apply_limits(byte),
          "Byte-only pruning failed.");
    ready(history);
    check(history.view().bytes <= 100LL * 1024 * 1024 && history.view().count > 1 &&
              history.view().retention == store::limits{0, 100},
          "No-age mode failed to enforce only the byte cap.");
    const auto locked =
        QDir(history.folder()).filePath("native-scan-ffffffffffffffffffffffffffffffff.json");
    QFile unrecognised(locked);
    check(unrecognised.open(QIODevice::WriteOnly) && unrecognised.resize(101LL * 1024 * 1024),
          "Cannot seed protected record.");
    unrecognised.close();
    const auto over = history.preview_limits({0, 100});
    check(over.over_budget && over.affected.empty(),
          "Unrecognised record was not protected or above-budget condition was concealed.");
    check(QFile::remove(locked), "Cannot remove disposable protected fixture.");
    // Empty files with recognised names exercise the lightweight startup index.
    // Their count must not itself trigger retention or require decoding reports
    // while usage remains within budget and the age limit is disabled.
    for (int file = 100; file < 9100; ++file) {
        QFile meta(
            QDir(history.folder())
                .filePath(QString("native-scan-%1.json").arg(file, 32, 16, QLatin1Char('0'))));
        check(meta.open(QIODevice::WriteOnly), "Could not seed large disposable metadata fixture.");
    }
    store large(root, true);
    ready(large);
    check(large.view().count > 9'000 && large.view().bytes <= 100LL * 1024 * 1024 &&
              large.preview_limits({0, 500}).affected.empty(),
          "Count influenced retention or metadata startup could not open the large history.");
    std::cout
        << "PASS native age and byte retention, raw export, stale confirmation, protected records and 9,000-file startup.\n";
}

void suggested_ties() {
    const auto complete = [](const std::vector<std::pair<int, double>>& channels) {
        using namespace cc::scanning;
        auto snapshot = std::make_shared<cc::scanning::snapshot>();
        std::vector<cc::channel> measurements;
        for (const auto& [number, total] : channels) {
            channel_measurements measured{{number, "127.0.0.1", 3456 + number}, {}};
            cc::channel input{number, {}};
            for (int round = 1; round <= 10; ++round) {
                measurement obs;
                obs.round = round;
                obs.total = total;
                obs.connection = total / 2;
                obs.elapsed = total;
                obs.terminal_offset = total;
                obs.stage = "Read";
                obs.outcome = "Success";
                measured.samples.push_back(obs);
                input.samples.push_back({round, total, total / 2, false});
            }
            snapshot->channels.push_back(std::move(measured));
            measurements.push_back(std::move(input));
        }
        snapshot->summary = cc::summarize(measurements);
        const auto time = std::chrono::system_clock::now();
        return cc::ui::replay_model::completed_native({time, time, snapshot}, {10, 250, 250, 1});
    };
    auto two = complete({{8, 20.}, {3, 20.}, {5, 22.5}});
    check(two.suggested_label() == "Ch. 3 or 8" && two.suggested_note() == "2 channels tied" &&
              two.suggests(0) && two.suggests(1) && !two.suggests(2),
          "Two exact leaders did not share the suggestion.");
    check(two.text_report().contains("Ch. 3 or 8") &&
              QJsonDocument::fromJson(two.original_json).object() == two.document &&
              two.document["Version"] == "2026.11.26",
          "Native tie report differs from the displayed suggestion.");
    auto three = complete({{8, 20.}, {3, 20.}, {5, 20.}, {2, 21.}});
    check(three.suggested_label() == "Ch. 5" && three.suggested_note() == "3 channels tied" &&
              three.summary.picks.fastest == 1 && three.summary.picks.consistent == 3 &&
              three.suggests(2) && !three.suggests(3),
          "Larger exact tie picked outside the group or changed other ranks.");
    auto near = complete({{1, 20.01}, {2, 20.}});
    check(near.suggested_label() == "Ch. 2" &&
              near.suggested_note() == "Several channels look similar",
          "A close but unequal score was promoted to an exact tie.");
    std::cout
        << "PASS exact two-way and larger suggestion ties, unchanged near-tie copy, report consistency and release version.\n";
}

void progress_fill_motion() {
    cc::ui::scan_progress_bar bar;
    bar.setRange(0, 100);
    bar.setTextVisible(false);
    bar.resize(300, 8);
    bar.setStyleSheet(
        "QProgressBar {border:0;background:#11161A;} QProgressBar::chunk {background:#6FAEA3;}");
    const auto frame = [&] {
        QImage img(bar.size(), QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        bar.render(&img);
        return img;
    };
    bar.set_measured(10., false);
    const auto initial = frame();
    bar.set_measured(75.5, true);
    check(bar.value() == 75 && bar.fill_in_motion() && frame() == initial,
          "Measured progress changed late or the fill jumped to the target.");
    QTest::qWait(115);
    check(!bar.advance_fill(true), "Progress fill did not settle.");
    const auto settled = frame();
    bar.set_measured(75.5, false);
    check(frame() == settled && settled != initial, "Motion changed the final progress fill.");
    bar.set_measured(85., true);
    check(!bar.advance_fill(false) && bar.value() == 85,
          "Reduced motion failed to finish a pending fill.");
    bar.set_measured(100., false);
    check(bar.value() == 100 && !bar.fill_in_motion(), "Completion did not settle immediately.");
    bar.set_measured(0., true);
    check(bar.value() == 0 && !bar.fill_in_motion(),
          "A new scan animated backwards instead of resetting.");
    std::cout
        << "PASS native progress fill separates measured value from motion and settles on reset, completion and reduced motion.\n";
}

void chart_motion() {
    cc::ui::scan_config config{{{1, "127.0.0.1", 3456}}, {10, 250, 250, 1}};
    auto model = cc::ui::replay_model::ready_scan(config);
    cc::ui::channel_card card(model, 0, nullptr);
    card.resize(230, 88);
    auto capture = [&] {
        QImage frame(card.size(), QImage::Format_ARGB32_Premultiplied);
        frame.fill(Qt::transparent);
        card.render(&frame);
        return frame;
    };
    model.channels[0].samples = {{1, 6., 4., false}};
    card.set_view(true, 10., false);
    (void)capture();
    model.channels[0].samples.push_back({2, 9., 4., false});
    card.set_view(true, 10., false, true);
    check(card.tick_motion(), "New contiguous sample did not start the reveal.");
    const auto first = capture();
    QTest::qWait(115);
    check(!card.tick_motion(), "Animation kept running after settle.");
    const auto finished = capture();
    card.set_view(true, 10., false, false);
    check(first != finished && finished == capture(),
          "Motion changed the final chart or did not affect its early frame.");
    model.channels[0].samples.push_back({3, {}, {}, true});
    card.set_view(true, 10., false, true);
    check(!card.tick_motion(), "A failed check started a line reveal.");
    model.channels[0].samples.push_back({4, 8., 4., false});
    card.set_view(true, 10., false, true);
    check(!card.tick_motion(), "Reveal connected across a failed check.");
    std::cout << "PASS native sample reveal, final frame parity and no reveal across failures.\n";
}

void custom_channels() {
    const QTemporaryDir profile(QDir::temp().filePath("cc-list-test-XXXXXX"));
    check(profile.isValid(), "Missing disposable channel-list profile.");
    const auto root = cc::ui::open_profile_root_under(profile.path());
    const auto list_path = QDir(root).filePath("channels.csv");
    QTcpServer first, second;
    int first_count = 0, second_count = 0;
    const auto serve = [](QTcpServer& server, int& count) {
        check(server.listen(QHostAddress(QHostAddress::LocalHost), 0),
              "Could not bind an owned channel fixture.");
        QObject::connect(&server, &QTcpServer::newConnection, &server, [&server, &count] {
            while (auto* socket = server.nextPendingConnection()) {
                ++count;
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                socket->write(QByteArray("test", 4));
                socket->disconnectFromHost();
            }
        });
    };
    serve(first, first_count);
    serve(second, second_count);
    const cc::scanning::endpoint a{23, "127.0.0.1", first.serverPort()};
    const cc::scanning::endpoint b{4, "127.0.0.1", second.serverPort()};
    const cc::ui::scan_config config{{a, b}, {10, 250, 250, 1}};
    const auto row = [](const auto& target) {
        return QString("%1,%2,%3")
            .arg(target.number)
            .arg(QString::fromStdString(target.host))
            .arg(target.port);
    };
    const auto build = [&] {
        return std::make_unique<cc::ui::main_window>(cc::ui::replay_model{}, config, QString{},
                                                     QString{}, QString{}, root);
    };
    const auto bytes = [](const QString& path) {
        QFile input(path);
        check(input.open(QIODevice::ReadOnly), "Could not read disposable settings/report.");
        return input.readAll();
    };
    auto window = build();
    window->show();
    const auto editor = [&] {
        control<QPushButton>(*window, "EditChannels")->click();
        for (auto* dialog : window->findChildren<QDialog*>())
            if (dialog->isVisible() && dialog->findChild<QPlainTextEdit*>("ChannelListText"))
                return dialog;
        throw std::runtime_error("Could not open the actual channel editor.");
    };
    {
        auto* dialog = editor();
        auto* draft = control<QPlainTextEdit>(*dialog, "ChannelListText");
        draft->setPlainText(row(b));
        control<QPushButton>(*dialog, "SaveChannels")->click();
        check(!dialog->isVisible() && window->model().channels.size() == 1 &&
                  window->model().endpoint(0) == row(b),
              "Apply did not activate the chosen channel.");
    }
    const auto saved = bytes(list_path);
    {
        auto* dialog = editor();
        control<QPushButton>(*dialog, "RestoreChannels")->click();
        check(control<QPlainTextEdit>(*dialog, "ChannelListText")->toPlainText().contains(row(a)) &&
                  bytes(list_path) == saved && window->model().endpoint(0) == row(b),
              "Restore changed more than the draft.");
        control<QPushButton>(*dialog, "CancelChannelEdit")->click();
        check(!dialog->isVisible() && bytes(list_path) == saved &&
                  window->model().endpoint(0) == row(b),
              "Cancel did not preserve the saved and active list.");
    }
    {
        const auto backup = list_path + ".held";
        check(QFile::rename(list_path, backup) && QDir().mkdir(list_path),
              "Could not set up a disposable save failure.");
        auto* dialog = editor();
        control<QPlainTextEdit>(*dialog, "ChannelListText")->setPlainText(row(a));
        control<QPushButton>(*dialog, "SaveChannels")->click();
        check(dialog->isVisible() && control<QLabel>(*dialog, "ChannelListError")->isVisible() &&
                  window->model().endpoint(0) == row(b),
              "Failed save closed the draft or changed the active list.");
        control<QPushButton>(*dialog, "CancelChannelEdit")->click();
        check(QDir().rmdir(list_path) && QFile::rename(backup, list_path) &&
                  bytes(list_path) == saved,
              "Could not restore the disposable settings fixture.");
    }
    control<QPushButton>(*window, "StartScan")->click();
    check(window->scan_running() && !control<QPushButton>(*window, "EditChannels")->isEnabled(),
          "The active list can be edited during a scan.");
    until([&] { return !window->scan_running() && window->model().state == "completed"; },
          "Custom channel scan did not finish.");
    check(first_count == 0 && second_count == 11 && window->model().endpoint(0) == row(b) &&
              window->model().summary.channels.at(0).successful == 10,
          "Scan did not use exactly the applied channel.");
    auto* settings = window->show_dialog("settings-on");
    until([&] { return control<QLabel>(*settings, "HistoryCount")->text() == "1 saved scans"; },
          "A completed custom scan did not persist.");
    settings->close();

    // Exercise the same writer the Export button uses. Opening a system file
    // chooser would change Qt's recent-folder preferences outside the fixture.
    const auto export_path = QDir(profile.path()).filePath("custom-report.json");
    check(control<QPushButton>(*window, "ExportReport")->isEnabled() &&
              cc::ui::main_window_test_access::export_json(*window, export_path),
          "The completed report could not be exported.");
    const auto exported = bytes(export_path);
    const auto report =
        QJsonDocument::fromJson(exported).object()["Result"].toObject()["Channels"].toArray();
    const auto endpoint = report.at(0).toObject()["Endpoint"].toObject();
    check(report.size() == 1 && endpoint["Channel"].toInt() == b.number &&
              endpoint["Host"].toString() == "127.0.0.1" && endpoint["Port"].toInt() == b.port,
          "Export did not identify the completed channel.");
    {
        auto* dialog = editor();
        control<QPlainTextEdit>(*dialog, "ChannelListText")->setPlainText(row(a));
        control<QPushButton>(*dialog, "SaveChannels")->click();
        check(!dialog->isVisible() && window->model().endpoint(0) == row(a) &&
                  !control<QPushButton>(*window, "ExportReport")->isEnabled() &&
                  bytes(export_path) == exported,
              "Changing lists relabelled or invalidated a prior export.");
    }
    window.reset();
    {
        cc::ui::history_store reopened(root, true);
        until([&] { return reopened.view().loaded; }, "Custom history did not reopen.");
        const auto now = QDateTime::currentDateTimeUtc();
        check(reopened.compare(row(b), config.settings, now).summary.indices.size() == 1 &&
                  reopened.compare(row(a), config.settings, now).summary.indices.empty(),
              "History attributed the scan to the wrong endpoint.");
    }
    window = build();
    check(window->model().channels.size() == 1 && window->model().endpoint(0) == row(a),
          "Second applied list did not reopen.");
    std::cout
        << "PASS actual Qt window: saved list, owned TCP scan, report export and history all agree.\n";
}
#endif
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    app.setStyle(new cc::ui::widget_style);
    app.setStyleSheet(cc::ui::stylesheet());
    const auto args = app.arguments();
    if (args.size() != 3 && args.size() != 4)
        return 2;
    const bool native_cursors = args.size() == 4;
    if (native_cursors && args.at(3) != "--native-cursors")
        return 2;
    QFile file(args.at(1));
    const QDir output(args.at(2));
    if (!output.exists() || !file.open(QIODevice::ReadOnly) || file.size() > 6 * 1024 * 1024)
        return 2;
    const auto corpus = QJsonDocument::fromJson(file.readAll()).array();
    try {
        const auto update = body(corpus, "newer-date");
        const auto maintenance = body(corpus, "confirmed-pst");
        // Native cursor checks require an unobstructed interactive Windows desktop.
        if (native_cursors) {
            interaction_policy(update, maintenance, true);
            return 0;
        }
#ifdef CC_PROFILE_HISTORY
        chart_motion();
        progress_fill_motion();
        suggested_ties();
        retention_policy();
        custom_channels();
#endif
        interaction_policy(update, maintenance, false);
        {
            screen_case h(update, maintenance);
            h.flow.maintenance_context(true, true);
            until(
                [&] {
                    return h.maintenance().contains("2026-11-19 17:45 UTC") &&
                           h.server.connected == 0;
                },
                "Initial maintenance date not retained.");
            h.server.primary = h.server.notice = response::hold;
            const auto prior_bytes = h.wire.received_bytes();
            h.extra += 3'600'000;
            h.advance();
            h.button("CheckForUpdates")->click();
            until(
                [&] {
                    return h.server.connected == 2 && h.wire.received_bytes() >= prior_bytes + 2;
                },
                "Both slow feeds did not start.");
            h.button("StartScan")->click();
            check(
                !h.window->scan_running() && h.text("StatusText").contains("background requests") &&
                    h.flow.updates() == update_status::paused &&
                    !h.button("CheckForUpdates")->isEnabled() &&
                    h.button("CancelScan")->isEnabled() && !h.button("EditChannels")->isEnabled() &&
                    !control<QComboBox>(*h.window, "Samples")->isEnabled() && h.probe_count == 0,
                "Actual window failed to display the drain gate.");
            h.server.primary = response::good;
            h.server.notice = response::unavailable;
            until([&] { return h.probe_count >= 1; }, "Native scan never admitted.");
            check(!h.overlap && h.maintenance().contains("paused") &&
                      !h.button("CheckForUpdates")->isEnabled(),
                  "Native probe overlapped feed sockets or the visible state was wrong.");
            until(
                [&] { return !h.window->scan_running() && h.window->model().state == "completed"; },
                "Native loopback scan did not finish.");
            check(h.probe_count == 11 && !h.overlap &&
                      h.window->model().summary.channels.at(0).successful == 10 &&
                      h.button("ExportReport")->isEnabled(),
                  "Completed scan retained bad probes or no export.");
            until(
                [&] {
                    return h.maintenance().contains("Couldn't refresh") &&
                           h.button("CheckForUpdates")->text() == "Update available";
                },
                "Expected resumed feed states did not reach the actual window.");
            check(h.maintenance().contains("2026-11-19 17:45 UTC") &&
                      h.button("CheckForUpdates")->text() == "Update available" &&
                      !h.has_installer_ui() && h.server.handled == 5 && !h.server.invalid_request,
                  "Lost date, repeated feed, or metadata was presented as install-ready.");
#ifdef CC_PROFILE_HISTORY
            auto* settings = h.window->show_dialog("settings-on");
            until(
                [&] {
                    return control<QLabel>(*settings, "HistoryCount")->text() == "1 saved scans";
                },
                "A completed scan did not persist to isolated scan history.");
            settings->close();
            auto* detail = h.window->show_dialog("history-saved");
            check(detail != nullptr, "Native history details did not open.");
            const auto labels = detail->findChildren<QLabel*>();
            check(
                std::any_of(labels.begin(), labels.end(),
                            [](const QLabel* label) { return label->text().contains("1 scans"); }),
                "The actual details window did not display the persisted native history point.");
            detail->close();
#endif
            (void)h.window->grab().save(output.filePath("services-after-scan.png"));
            std::cout
                << "PASS actual Qt window: both HTTP connections drained before 11 owned TCP probes; notices resumed once and kept the confirmed date.\n";
            std::cout
                << "PASS actual Qt window: update metadata shown as available, no Update ready banner or installer.\n";

            h.server.primary = response::hold;
            const auto before_close = h.server.handled;
            h.button("CheckForUpdates")->click();
            until([&] { return h.server.handled == before_close + 1 && h.server.connected == 1; },
                  "No in-flight request for close.");
            h.window->close();
            check(h.window->isVisible() && !h.flow.closed(), "Window closed before HTTP cleanup.");
            until(
                [&] {
                    return !h.window->isVisible() && h.flow.closed() && h.server.connected == 0;
                },
                "Window close never drained.");
            check(h.server.handled == before_close + 1, "Close restarted traffic.");
#ifdef CC_PROFILE_HISTORY
            h.window.reset();
            cc::ui::history_store reopened(h.profile_root, true);
            until([&] { return reopened.view().loaded && reopened.view().pending == 0; },
                  "Saved history did not reopen.");
            const auto key = QString("1,127.0.0.1,%1").arg(h.probes.serverPort());
            const auto now = QDateTime::currentDateTimeUtc();
            check(reopened.compare(key, h.config.settings, now).summary.indices.size() == 1 &&
                      reopened.compare(key, h.config.settings, now, now).summary.indices.empty(),
                  "Reopened history did not respect its maintenance cutoff.");
            std::cout
                << "PASS persistent scan history: real scan, rendered history, reopened data and maintenance cutoff.\n";
#endif
            std::cout
                << "PASS actual Qt window: close waited for a real pending HTTP request and never restarted it.\n";
        }
        {
            screen_case h(update, maintenance);
            h.server.primary = response::hold;
            h.button("CheckForUpdates")->click();
            until([&] { return h.server.connected == 1 && h.wire.received_bytes() == 1; },
                  "No pending update for scan cancellation.");
            h.button("StartScan")->click();
            h.button("CancelScan")->click();
            h.server.primary = response::good;
            until([&] { return h.flow.updates() == update_status::available; },
                  "Cancelled pre-scan did not resume service.");
            check(h.probe_count == 0 && !h.window->scan_running() &&
                      h.window->model().state == "ready" &&
                      !h.text("StatusText").contains("background requests") &&
                      h.button("StartScan")->isEnabled(),
                  "Cancel while draining still started a probe or kept waiting UI.");
            h.window->close();
            std::cout
                << "PASS actual Qt window: cancelling while services drain kept the scan idle.\n";
        }
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
    std::cout
        << "PASS 4 window service cases; loopback transport and disposable profiles, no external feeds or installer.\n";
}
