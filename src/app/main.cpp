// Application composition and launch modes. Normal startup opens the native
// profile and wires background services to the scan drain barrier. Diagnostics
// use disposable profiles; legacy import is explicit and remains isolated.
// The window owns measurement/history work. This entry point owns HTTP lifetime
// and the final verified-installer handoff after the event loop has drained.

#include "profile_paths.hpp"
#include "channel_settings.hpp"
#include <QDir>
#include "comparison_catalog.hpp"
#include "service_http.hpp"
#include "main_window.hpp"
#include "widget_style.hpp"
#include "notice_cache.hpp"
#include "wpf_migration.hpp"
#include <QStandardPaths>
#include <QScrollBar>
#include <QScreen>
#include <QApplication>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QFile>
#include <QPlainTextEdit>
#include <QTemporaryDir>
#include <QSslSocket>
#include <iostream>
#include <QMessageBox>
#include <QThread>

namespace {
using namespace cc::service_flow;

struct no_requests final : transport {
    void start(request_id, source) override {
        throw std::logic_error("Offline self-check tried to issue an HTTP request.");
    }

    void abort(request_id) override {
        throw std::logic_error("Offline self-check has no HTTP activity.");
    }
};

// Exercise startup and saved selections without scanning or contacting services.
// The temporary directory outlives the window and its history worker; the throwing
// transport makes accidental HTTP activity fail instead of silently escaping offline mode.
int self_check(QApplication& app, const cc::ui::scan_config& config) {
    // Build a real profile under disposable storage, with an HTTP guard rather
    // than a network adapter. UI settings still use the normal persistence code.
    const QTemporaryDir dir;
    if (!dir.isValid())
        throw std::runtime_error("Cannot create disposable root.");
    const auto root = cc::ui::open_profile_root_under(dir.path());
    no_requests wire;
    coordinator services(wire, "2026.11.24", {}, false);
    const auto now = QDateTime::currentDateTimeUtc();
    services.advance(0, now);

    // Seed and reopen the disk cache before attaching the window. This checks
    // restoration of a confirmed maintenance boundary, not just an in-memory value.
    const QJsonObject known{{"sourceId", "123"},
                            {"completedUtc", now.addSecs(-3600).toString(Qt::ISODate)}};
    cc::notice_cache::store cache(root);
    if (cache.save({now.addSecs(-60), known, false}, now) != cc::notice_cache::write_result::saved)
        throw std::runtime_error("Could not seed isolated cache fixture.");
    const auto cached = cache.read(now);
    if (!cached.value)
        throw std::runtime_error("Could not reopen isolated cache fixture.");
    services.restore_confirmed(cached.value->event, cached.value->fetched, cached.value->uncertain);

    // Process queued Qt layout/initialisation work before inspecting the visible
    // controls. No Start click occurs: these assertions must not launch probes.
    cc::ui::main_window window(cc::ui::replay_model{}, config, {}, {}, {}, root);
    window.attach_window_services(services);
    window.enable_profile_settings();
    window.show();
    app.processEvents();
    if (window.windowIcon().isNull() || window.windowIcon().pixmap(32, 32).isNull())
        throw std::runtime_error("The packaged Windows icon cannot be rendered.");
    const auto samples = window.findChild<QComboBox*>("Samples");
    if (!samples || samples->currentText() != "40" || samples->findText("10") < 0 ||
        window.windowTitle() != "Channel Checker")
        throw std::runtime_error(
            "Expected the Channel Checker title, 40-sample default and optional 10-sample scan.");
    const auto title = window.findChild<QLabel*>("AppTitle");
    const auto ready = window.findChild<QLabel*>("RecommendationStatusText");
    if (!title || title->text() != "MabiTools - Channel Checker" || !ready ||
        ready->text() != "Ready")
        throw std::runtime_error("Expected the MabiTools heading and Ready startup status.");
    const auto start = window.findChild<QPushButton*>("StartScan");
    const auto edit = window.findChild<QPushButton*>("EditChannels");
    const auto updates = window.findChild<QPushButton*>("CheckForUpdates");
    services.request_update(); // No request may reach no_requests.
    const auto banner = window.findChild<QWidget*>("UpdateBanner");
    const auto installer = window.findChild<QPushButton*>("InstallUpdate");
    auto* dialog = window.show_dialog("settings-on");
    const auto notice = dialog->findChild<QLabel*>("MaintenanceStatus");
    const auto automatic = dialog->findChild<QCheckBox*>("AutomaticMaintenance");
    if (!start || !start->isEnabled() || !edit || !edit->isEnabled() || banner || installer ||
        !updates || updates->isEnabled() || updates->text() != "Updates disabled" || !notice ||
        !notice->text().contains("Last confirmed maintenance") || !automatic ||
        automatic->isChecked() || window.model().channels.size() != 16 || window.scan_running() ||
        !window.model().original_json.isEmpty())
        throw std::runtime_error("Unexpected application startup state.");

    // Parsing a row does not grant network permission. Mutating either the port
    // or host of a bundled endpoint must fail the production allowlist.
    const auto reject = [](const cc::scanning::endpoint& target) {
        try {
            cc::scanning::validate({target}, {10, 1500, 1000, 4});
        } catch (const std::invalid_argument&) {
            return;
        }
        throw std::runtime_error("Unlisted target passed the bundled endpoint gate.");
    };
    auto changed = config.targets.front();
    ++changed.port;
    reject(changed);
    changed = config.targets.front();
    changed.host = "127.0.0.1";
    reject(changed);
    dialog->close();
    window.close();

    // Reopen through the real loader without pressing Start. The exact ordered
    // 13-channel prefix identifies the old default preset, so it gains the three
    // added channels. A reordered list or subset expresses a saved choice and
    // must not be replaced merely because its entries are bundled endpoints.
    const auto selected_count = [&](const auto& rows) {
        cc::ui::channel_settings::save_profile(root, rows);
        cc::ui::main_window reopened(cc::ui::replay_model{}, config, {}, {}, {}, root);
        return reopened.model().channels.size();
    };
    auto legacy = config.targets;
    legacy.resize(13);
    if (selected_count(legacy) != 16)
        throw std::runtime_error("Exact old bundled preset did not expand.");
    std::swap(legacy[0], legacy[1]);
    if (selected_count(legacy) != 13)
        throw std::runtime_error("Reordered legacy list expanded unexpectedly.");
    legacy.resize(2);
    if (selected_count(legacy) != 2)
        throw std::runtime_error("Saved subset did not reopen.");

    // Malformed settings must warn and fall back in memory, leaving the original
    // bytes available for recovery. Startup must not silently overwrite the file.
    QFile invalid(QDir(root).filePath("channels.csv"));
    const QByteArray malformed("invalid settings\n");
    if (!invalid.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        invalid.write(malformed) != malformed.size())
        throw std::runtime_error("Could not seed invalid channel list.");
    invalid.close();
    cc::ui::main_window fallback(cc::ui::replay_model{}, config, {}, {}, {}, root);
    const auto warning = fallback.findChild<QLabel*>("ReportFeedbackText");
    if (!warning || !warning->text().contains("kept but not loaded") ||
        fallback.model().channels.size() != 16 || !invalid.open(QIODevice::ReadOnly) ||
        invalid.readAll() != malformed)
        throw std::runtime_error(
            "Invalid channel list did not warn, preserve bytes, and use bundled defaults.");
    std::cout
        << "PASS bundled channel list loads legacy, reordered and subset settings, and preserves invalid input.\n";
    std::cout
        << "PASS application opens offline, restores its notice, and rejects unlisted destinations.\n";
    return 0;
}

// Check layout using real widgets in a disposable profile, without scanning.
// Qt defers layout and resize work through its event queue; flush that work after
// show/resize before using widget geometry as evidence of fit or clipping.
void verify_grid(QApplication& app, const cc::ui::scan_config& config) {
    const QTemporaryDir fixture;
    if (!fixture.isValid())
        throw std::runtime_error("Cannot create layout fixture.");
    no_requests wire;
    coordinator services(wire, "2026.11.24", {}, false);
    const auto root = cc::ui::open_profile_root_under(fixture.path());
    cc::ui::main_window window(cc::ui::replay_model{}, config, {}, {}, {}, root);
    window.attach_window_services(services);
    window.enable_profile_settings();
    window.show();
    app.processEvents();
    window.fit_initial_results_grid();
    app.processEvents();
    auto* scroll = window.findChild<QScrollArea*>("ChannelCardScroll");
    auto* grid = window.findChild<QWidget*>("ChannelCardGrid");
    if (!scroll || !grid || window.model().channels.size() != 16 ||
        window.frameGeometry().height() > window.screen()->availableGeometry().height())
        throw std::runtime_error("Initial 16-channel window must fit within the screen.");

    // A tall client must show every row without a needless vertical scrollbar.
    // Exercise this explicitly rather than relying on the screen's initial size.
    window.resize(1144, 1050);
    app.processEvents();
    if (scroll->verticalScrollBar()->maximum() != 0 ||
        grid->height() > scroll->viewport()->height())
        throw std::runtime_error(
            "The tall client clipped a row or unnecessarily showed a scrollbar.");

    // A compact client needs a scrollbar and its horizontal gutter. Scrolling
    // to the end must expose the last row, not leave it clipped below the viewport.
    window.resize(1144, 820);
    app.processEvents();
    const auto extent = scroll->verticalScrollBar()->sizeHint().width();
    if (scroll->verticalScrollBar()->maximum() <= 0 || extent <= 0 ||
        grid->width() > scroll->width() - extent)
        throw std::runtime_error("The compact client did not reserve a scrollbar gutter.");
    scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
    if (grid->geometry().bottom() >= scroll->viewport()->height())
        throw std::runtime_error("The last row cannot be reached by scrolling.");
    window.close();
    std::cout << "PASS default grid fit and unobstructed compact scrolling.\n";
}

// Enable background maintenance and updates only in normal mode.
// Normal mode schedules one update check at startup. Isolated and imported
// profiles leave both services disabled, regardless of saved preferences.
int launch(QApplication& app, const cc::ui::scan_config& config, const QString& root,
           bool normal = false) {
#ifdef Q_OS_WIN
    if (QSslSocket::availableBackends().contains("schannel")) {
        (void)QSslSocket::setActiveBackend("schannel");
    }
#endif
    // Apply the history profile's reparse-path checks before opening the
    // application's cache and lock files.
    (void)cc::ui::checked_profile_file(root, "native-notice-test.json");
    (void)cc::ui::checked_profile_file(root, "native-notice-test.json.lock");
    cc::notice_cache::store cache(root);
    service_http wire(root);
    const auto mode = is_setup_install() ? update_mode::installed : update_mode::portable;
    // The first normal argument permits update requests; the last permits
    // maintenance requests. mode separately limits portable copies to ZIP links.
    coordinator services(wire, cc::ui::application_version, {}, normal, mode, normal);

    // Elapsed milliseconds drive deadlines and retry delays without wall-clock
    // jumps. UTC dates validate notice/cache timestamps and date-based freshness.
    QElapsedTimer clock;
    clock.start();
    const auto advance = [&] {
        services.advance(clock.elapsed(), QDateTime::currentDateTimeUtc());
    };
    wire.bind(services, advance);
    advance();
    auto last_written = QDateTime{};
    // Restore before enabling profile settings, so history already has its last
    // confirmed maintenance boundary while an asynchronous refresh is pending.
    const auto restored = cache.read(QDateTime::currentDateTimeUtc());
    if (restored.value) {
        const auto& value = *restored.value;
        services.restore_confirmed(value.event, value.fetched, value.uncertain);
        last_written = value.fetched;
    }

    // This event-loop timer advances service timeouts/retries and persists newly
    // retrieved notices. HTTP data arrives through transport signals, not this
    // timer. Its captured cache, coordinator and clock all outlive the event loop.
    QTimer tick;
    QObject::connect(&tick, &QTimer::timeout, &tick, [&] {
        advance();
        const auto retrieved = services.notice_retrieved();
        if (retrieved.isValid() && (!last_written.isValid() || retrieved > last_written)) {
            // Only a successful parsed retrieval reaches this point. Failed
            // fetches leave the profile cache and last confirmed date alone.
            last_written = retrieved;
            (void)cache.save({retrieved, services.confirmed(), services.notice_uncertain()},
                             QDateTime::currentDateTimeUtc());
        }
    });
    tick.start(20);
    cc::ui::main_window window(cc::ui::replay_model{}, config, {}, {}, {}, root);
    window.attach_window_services(services);
    window.enable_profile_settings();
    window.show();
    window.fit_initial_results_grid();
    // Queue the single startup check after construction and initial layout. Qt
    // runs it when event processing resumes; using the window as context also
    // prevents delivery if that window is destroyed before the callback runs.
    if (normal)
        QTimer::singleShot(0, &window, [&] { services.request_update(); });
    // Consent records an install request and closes the window. closeEvent keeps
    // the event loop alive until probes, history and service requests drain.
    // Only then may Setup replace this installation; reverify the cached bytes
    // immediately before launch rather than trusting the earlier ready state.
    const int result = app.exec();
    const auto request = window.install_request();
    if (!request.isEmpty()) {
        QString error;
        if (!services.closed() || wire.live_requests() != 0 ||
            !launch_verified_update(root, request["verifiedPath"].toString(),
                                    request["installer"].toObject(), error)) {
            QMessageBox::warning(
                nullptr, "Channel Checker",
                error.isEmpty() ? "Background work has not stopped. The installer was not started."
                                : error);
            return 1;
        }
    }
    return result;
}
}

int main(int argc, char** argv) {
    QApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    QApplication app(argc, argv);
    app.setApplicationName("MabiTools Channel Checker");
    app.setStyle(new cc::ui::widget_style);
    QFont font("Segoe UI Variable");
    font.setPixelSize(13);
    app.setFont(font);
    app.setStyleSheet(cc::ui::stylesheet());
    try {
        const auto args = app.arguments();
        const cc::ui::scan_config config{cc::scanning::comparison_catalog, {}};
        if (args.size() == 2 && args.at(1) == "--self-check")
            return self_check(app, config);
        if (args.size() == 2 && args.at(1) == "--layout-self-check") {
            verify_grid(app, config);
            return 0;
        }
        // Capture the real Settings dialog with offline services and disposable
        // history. Keep all owners alive while queued layout/history work settles.
        if (args.size() == 3 && args.at(1) == "--settings-capture") {
            QTemporaryDir fixture;
            no_requests wire;
            coordinator services(wire, cc::ui::application_version, {}, false);
            services.advance(0, QDateTime::currentDateTimeUtc());
            cc::ui::main_window window(cc::ui::replay_model{}, config, {}, {}, {},
                                       cc::ui::open_profile_root_under(fixture.path()));
            window.attach_window_services(services);
            window.enable_profile_settings();
            window.show();
            auto* settings = window.show_dialog("settings-on");
            // Give asynchronous history status and Qt layout a bounded settling
            // period before checking clipping and capturing pixels.
            QElapsedTimer elapsed;
            elapsed.start();
            while (elapsed.elapsed() < 500) {
                app.processEvents();
                QThread::msleep(1);
            }
            auto* path = settings->findChild<QPlainTextEdit*>("HistoryFolderPath");
            auto* scroll = settings->findChild<QScrollArea*>("SettingsScroll");
            auto* close = settings->findChild<QPushButton*>("CloseHistorySettings");
            if (!path || !path->isReadOnly() || !scroll ||
                scroll->horizontalScrollBar()->maximum() != 0 || !close ||
                !settings->rect().contains(close->mapTo(settings, close->rect().bottomRight())))
                throw std::runtime_error(
                    "Settings controls are clipped or history path is not copyable.");
            cc::ui::capture_widget(*settings, args.at(2));
            settings->close();
            window.close();
            std::cout
                << "PASS fixed Settings heading and Close, scrollable sections and copyable history path.\n";
            return 0;
        }
        // First validate snapshot import, then reopen its separate profile in
        // the UI. The original archive remains distinct from native scan history.
        if (args.size() == 2 && args.at(1) == "--migration-self-check") {
            QTemporaryDir fixture;
            if (!fixture.isValid())
                throw std::runtime_error("Cannot create migration test fixture.");
            cc::ui::verify_wpf_migration(fixture.path());
            const auto root = cc::ui::imported_profile_root(fixture.path());
            no_requests wire;
            coordinator services(wire, "2026.11.24", {}, false);
            cc::ui::main_window window(cc::ui::replay_model{}, config, {}, {}, {}, root);
            window.attach_window_services(services);
            window.enable_profile_settings();
            window.show();
            app.processEvents();
            auto* settings = window.show_dialog("settings-on");
            const auto archive = settings->findChild<QPushButton*>("ViewImportedWpfHistory");
            const auto maintenance = settings->findChild<QCheckBox*>("AutomaticMaintenance");
            if (!archive || !maintenance || maintenance->isChecked() ||
                window.model().channels.size() != 2)
                throw std::runtime_error(
                    "Application did not reopen imported settings and archive.");
            bool opened = false;
            // Queue before click: the archive opens synchronously in exec(),
            // which starts a nested Qt event loop. That loop runs this callback
            // while the modal exists, allowing inspection and rejection before
            // click returns. opened and the enclosing window remain alive throughout.
            QTimer::singleShot(0, settings, [&] {
                auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                if (!modal)
                    return;
                const auto raw = modal->findChild<QPlainTextEdit*>();
                opened = raw && raw->toPlainText().contains("Keep original bytes.");
                modal->reject();
            });
            archive->click();
            if (!opened)
                throw std::runtime_error("Application did not display the original archived scan.");
            settings->close();
            window.close();
            std::cout << "PASS migration isolation, settings, raw archive and conflict refusal.\n";
            return 0;
        }
        // Profile modes are deliberately separate from offline checks above.
        // An isolated interactive window can scan the approved catalogue, but
        // cannot reach live feeds. Import only archives an explicit snapshot;
        // opening that archive later never merges it into the normal profile.
        if (args.size() == 2 && args.at(1) == "--isolated-profile") {
            QTemporaryDir test;
            if (!test.isValid())
                throw std::runtime_error("Cannot create an isolated test profile.");
            return launch(app, config, cc::ui::open_profile_root_under(test.path()));
        }
        if (args.size() == 1)
            return launch(app, config, cc::ui::open_profile_root(), true);
        const auto local = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
        if (args.size() == 3 && args.at(1) == "--import-legacy") {
            const auto count = cc::ui::import_wpf_snapshot(args.at(2), local);
            std::cout << "PASS archived " << count
                      << " original WPF scans. Start with --legacy-profile to review.\n";
            return 0;
        }
        if (args.size() == 2 && args.at(1) == "--legacy-profile")
            return launch(app, config, cc::ui::imported_profile_root(local));
        std::cerr
            << "Unknown option. Launch without arguments, or use --import-legacy <snapshot> and --legacy-profile.\n";
        return 2;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
