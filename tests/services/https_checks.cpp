#include "service_http.hpp"
#include "profile_paths.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QSslSocket>
#include <QTemporaryDir>
#include <QThread>
#include <type_traits>
#include <iostream>

using namespace cc::service_flow;
static_assert(!std::is_constructible_v<service_http, quint16, QString>,
              "Production transport must not accept a fixture port");
static_assert(!std::is_constructible_v<update_download, QString, QJsonObject, quint16,
                                       update_download::completion>,
              "Production downloads must not accept a fixture port");

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    // This path exercises compile-time constructor restrictions only. Live HTTPS
    // requires its separate explicit flag and must not run during offline checks.
    if (app.arguments().size() == 2 && app.arguments()[1] == "--configuration-check") {
        std::cout
            << "PASS production metadata and download constructors cannot select a fixture port.\n";
        return 0;
    }
    if (app.arguments().size() != 3 || app.arguments()[1] != "--live-native-feed")
        return 2;
    try {
        if (!QSslSocket::availableBackends().contains("schannel") ||
            !QSslSocket::setActiveBackend("schannel"))
            throw std::runtime_error("Windows TLS backend unavailable");
        QTemporaryDir scratch;
        service_http wire(cc::ui::open_profile_root_under(scratch.path()));
        // Explicit, read-only HTTPS diagnostic. It cannot download or launch Setup.
        coordinator flow(wire, CC_APP_VERSION, {}, true, update_mode::portable, false);
        QElapsedTimer clock;
        clock.start();
        const auto advance = [&] {
            flow.advance(clock.elapsed(), QDateTime::currentDateTimeUtc());
        };
        wire.bind(flow, advance);
        advance();
        flow.request_update();
        while (clock.elapsed() < 25000 && flow.updates() != update_status::failed &&
               flow.updates() != update_status::current &&
               flow.updates() != update_status::available) {
            advance();
            app.processEvents();
            QThread::msleep(1);
        }
        const auto state = flow.updates();
        flow.close();
        while (!flow.closed() && clock.elapsed() < 30000) {
            app.processEvents();
            QThread::msleep(1);
        }
        const QJsonObject result{
            {"sourceCommit", CC_SOURCE_COMMIT},
            {"installedVersion", CC_APP_VERSION},
            {"checkedAtUtc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
            {"tlsBackend", QSslSocket::activeBackend()},
            {"transport", "native-https"},
            {"state", state == update_status::current     ? "current"
                      : state == update_status::available ? "available"
                                                          : "unavailable"},
            {"metadata", flow.update_metadata()},
            {"requests", wire.diagnostics()},
            {"drained", flow.closed() && wire.live_requests() == 0},
            {"installerLaunched", false}};
        QFile output(app.arguments()[2]);
        if (!output.open(QIODevice::WriteOnly) || output.write(QJsonDocument(result).toJson()) < 0)
            throw std::runtime_error("Cannot write HTTPS result");
        return flow.closed() ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
