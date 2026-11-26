// Loopback-only updater integration checks. Synthetic bytes exercise discovery,
// redirect policy, verification and cache/drain transitions without launching
// Setup. This proves transport policy, not a real installed-version upgrade.

#include "service_http.hpp"
#include "profile_paths.hpp"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <iostream>
#include <stdexcept>

using namespace cc::service_flow;

namespace {
void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

QByteArray json(const QJsonObject& value) {
    return QJsonDocument(value).toJson(QJsonDocument::Compact);
}

void until(const std::function<bool()>& condition, const char* failure, int timeout = 8000) {
    QElapsedTimer time;
    time.start();
    while (!condition() && time.elapsed() < timeout) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    check(condition(), failure);
}

void pump(int duration) {
    QElapsedTimer time;
    time.start();
    while (time.elapsed() < duration) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
}

QJsonObject feed(const QByteArray& bytes) {
    const QString version = "2026.11.26.2", tag = "cc-v" + version;
    const QString hub = "https://github.com/LOOKaUSERNAME/mabi-tools-releases/releases/";
    QJsonArray assets;
    for (const auto* role : {"installer", "portable"}) {
        const QString name = "MabiTools-Channel-Checker-" + version +
                             (QString(role) == "installer" ? "-Setup-x64.exe" : "-win-x64.zip");
        assets.append(QJsonObject{
            {"role", role},
            {"name", name},
            {"size", bytes.size()},
            {"sha256", QString::fromLatin1(
                           QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())},
            {"downloadUrl", hub + "download/" + tag + "/" + name}});
    }
    return {{"schemaVersion", 1},
            {"productId", "cc"},
            {"channel", "stable"},
            {"feedId", "cc-native-stable-v1"},
            {"architecture", "windows-x64"},
            {"installerAppId", "{B81A179F-E1D0-471B-B274-B521D71C8088}"},
            {"release",
             QJsonObject{{"version", version},
                         {"centralTag", tag},
                         {"releaseUrl", hub + "tag/" + tag},
                         {"assets", assets},
                         {"source", QJsonObject{{"repository", "LOOKaUSERNAME/mabi-tools-cc"},
                                                {"tag", "v" + version},
                                                {"commit", QString(40, 'a')},
                                                {"tagObject", QString(40, 'b')}}}}}};
}

class fixture final : public QTcpServer {
public:
    QByteArray payload = QByteArray(512 * 1024, 'x');
    QJsonObject document = feed(payload);
    QString mode;
    int downloads{}, metadata{};
    QSet<QTcpSocket*> active;

    explicit fixture(QString scenario) : mode(std::move(scenario)) {
        check(listen(QHostAddress::LocalHost, 0), "Fixture listen failed");
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                auto* socket = nextPendingConnection();
                active.insert(socket);
                connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
                    active.remove(socket);
                    socket->deleteLater();
                });
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    if (socket->property("handled").toBool()) {
                        socket->readAll();
                        return;
                    }
                    const auto request =
                        socket->property("request").toByteArray() + socket->readAll();
                    socket->setProperty("request", request);
                    if (!request.contains("\r\n\r\n"))
                        return;
                    socket->setProperty("handled", true);
                    const auto path = request.split(' ').value(1);
                    if (path.startsWith("/update/")) {
                        ++metadata;
                        const auto body = json(document);
                        socket->write("HTTP/1.1 200 OK\r\nContent-Length: " +
                                      QByteArray::number(body.size()) +
                                      "\r\nConnection: close\r\n\r\n" + body);
                        socket->disconnectFromHost();
                        return;
                    }
                    ++downloads;
                    if (mode == "redirect-bad" ||
                        (mode == "redirect-good" && !path.endsWith("/redirected.bin"))) {
                        const auto location =
                            mode == "redirect-bad"
                                ? QByteArray("https://example.com/setup.exe")
                                : QByteArray(
                                      "https://release-assets.githubusercontent.com/redirected.bin");
                        socket->write("HTTP/1.1 302 Found\r\nContent-Length: 0\r\nLocation: " +
                                      location + "\r\nConnection: close\r\n\r\n");
                        socket->disconnectFromHost();
                        return;
                    }
                    auto body = payload;
                    if (mode == "corrupt")
                        body[0] = 'z';
                    const auto declared = body.size() + (mode == "size" ? 1 : 0);
                    socket->write("HTTP/1.1 200 OK\r\nContent-Length: " +
                                  QByteArray::number(declared) + "\r\nConnection: close\r\n\r\n");
                    if (mode == "truncated") {
                        socket->write(body.first(body.size() / 2));
                        socket->disconnectFromHost();
                    } else if (mode == "slow") {
                        auto* timer = new QTimer(socket);
                        timer->setInterval(10);
                        connect(timer, &QTimer::timeout, socket,
                                [socket, timer, body, offset = qsizetype{}]() mutable {
                                    if (socket->state() != QAbstractSocket::ConnectedState) {
                                        timer->stop();
                                        return;
                                    }
                                    const auto chunk = body.mid(offset, 4096);
                                    socket->write(chunk);
                                    offset += chunk.size();
                                    if (offset == body.size()) {
                                        timer->stop();
                                        socket->disconnectFromHost();
                                    }
                                });
                        timer->start();
                    } else {
                        socket->write(body);
                        socket->disconnectFromHost();
                    }
                });
            }
        });
    }
};

struct harness {
    QTemporaryDir directory;
    QString root = cc::ui::open_profile_root_under(directory.path());
    fixture server;
    service_http wire;
    coordinator flow;
    QElapsedTimer clock;
    QTimer tick;

    explicit harness(QString scenario = {}, update_mode mode = update_mode::installed,
                     QString version = "2026.11.26.1")
        : server(std::move(scenario)), wire(server.serverPort(), root),
          flow(wire, std::move(version), {}, true, mode) {
        clock.start();
        wire.bind(flow, [this] { advance(); });
        advance();
        QObject::connect(&tick, &QTimer::timeout, &tick, [this] { advance(); });
        tick.start(1);
    }

    ~harness() {
        flow.close();
        until([this] { return flow.closed(); }, "Close did not drain");
    }

    void advance() { flow.advance(clock.elapsed(), QDateTime::currentDateTimeUtc()); }

    void ready() {
        flow.request_update();
        until(
            [this] {
                return flow.updates() == update_status::ready ||
                       flow.updates() == update_status::failed;
            },
            "Download did not finish");
    }
};

// Mutate one identity field at a time so rejection cannot be attributed to an
// unrelated invalid field. Version equality/rollback must also omit installer data,
// not merely hide an update indication in the UI.
void policies() {
    const auto original = feed("fixture");
    check(cc::service_policy::native_update(json(original), "2026.11.26.1")["available"].toBool(),
          "New version was refused");
    for (const auto* current : {"2026.11.26.2", "2026.11.26.3"}) {
        const auto result = cc::service_policy::native_update(json(original), current);
        check(!result["available"].toBool() && !result.contains("installer"),
              "Equal/old release became installable");
    }
    for (const auto* key : {"feedId", "productId", "channel", "architecture", "installerAppId"}) {
        auto wrong = original;
        wrong[key] = "wrong";
        bool rejected = false;
        try {
            cc::service_policy::native_update(json(wrong), "2026.11.26.1");
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        check(rejected, "Wrong feed identity accepted");
    }
    auto legacy = original;
    legacy.remove("feedId");
    bool rejected = false;
    try {
        cc::service_policy::native_update(json(legacy), "2026.11.26.1");
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "Legacy feed accepted for native installation");
    for (const auto* key : {"centralTag", "releaseUrl", "version"}) {
        auto wrong = original;
        auto release = wrong["release"].toObject();
        release[key] = "wrong";
        wrong["release"] = release;
        rejected = false;
        try {
            cc::service_policy::native_update(json(wrong), "2026.11.26.1");
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        check(rejected, "Mismatched release identity accepted");
    }
    for (const auto* key : {"name", "downloadUrl", "sha256", "size", "role"}) {
        auto wrong = original;
        auto release = wrong["release"].toObject();
        auto assets = release["assets"].toArray();
        auto asset = assets[0].toObject();
        asset[key] = "wrong";
        assets[0] = asset;
        release["assets"] = assets;
        wrong["release"] = release;
        rejected = false;
        try {
            cc::service_policy::native_update(json(wrong), "2026.11.26.1");
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        check(rejected, "Mismatched installer asset accepted");
    }
    for (const auto* url :
         {"http://github.com/LOOKaUSERNAME/mabi-tools-releases/releases/download/cc-v1/setup.exe",
          "https://github.com.evil.test/setup.exe",
          "https://user@github.com/LOOKaUSERNAME/mabi-tools-releases/releases/download/cc-v1/a.exe",
          "https://release-assets.githubusercontent.com:444/test",
          "https://github.com/other/repo/releases/download/a/b"})
        check(!trusted_asset_url(QUrl(url)), "Untrusted redirect accepted");
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        if (app.arguments().size() == 4 && app.arguments()[1] == "--validate-feed") {
            QFile input(app.arguments()[2]);
            check(input.open(QIODevice::ReadOnly) && input.size() <= 64 * 1024,
                  "Cannot read bounded feed");
            const auto parsed =
                cc::service_policy::native_update(input.readAll(), app.arguments()[3]);
            std::cout << QJsonDocument(parsed).toJson(QJsonDocument::Indented).constData();
            return 0;
        }
        policies();
        for (const auto* scenario : {"corrupt", "truncated", "size", "redirect-bad"}) {
            harness h(scenario);
            h.ready();
            check(h.flow.updates() == update_status::failed && h.flow.ready_file().isEmpty(),
                  "Invalid download reached ready");
        }
        // Reopen the verified cache, then tamper with the same path. Reuse must
        // depend on the current bytes, not a remembered successful download.
        {
            harness h("redirect-good");
            h.ready();
            check(h.flow.updates() == update_status::ready && h.server.downloads == 2,
                  "Allowed redirect failed");
            const auto path = h.flow.ready_file();
            const auto asset = h.flow.update_metadata()["installer"].toObject();
            check(verify_update_file(h.root, path, asset), "Verified cache did not reopen");
            h.ready();
            check(h.server.downloads == 2 && h.flow.updates() == update_status::ready,
                  "Verified cache was not reused");
            QFile file(path);
            check(file.open(QIODevice::WriteOnly), "Cache tamper fixture failed");
            file.write("corrupt");
            file.close();
            check(!verify_update_file(h.root, path, asset), "Changed cache still verifies");
            h.ready();
            check(h.server.downloads == 4 && h.flow.updates() == update_status::ready,
                  "Corrupt cache was not replaced");
        }
        // Hold a body open across scan admission. Observe actual live-request
        // count and download count to distinguish drainage from a UI state change.
        {
            harness h("slow");
            h.flow.request_update();
            until([&] { return h.server.downloads == 1; }, "Slow download did not start");
            h.flow.request_scan();
            until([&] { return h.flow.scan() == scan_status::ready; },
                  "Scan did not wait for download drain");
            check(h.wire.live_requests() == 0, "Scan admitted with live transport");
            h.flow.scan_started();
            pump(100);
            check(h.server.downloads == 1, "Download restarted during measured scan");
            h.flow.scan_finished();
            until([&] { return h.flow.updates() == update_status::ready; },
                  "Paused download did not resume");
            check(h.server.downloads == 2, "Download did not resume exactly once");
        }
        {
            harness h("slow");
            h.flow.request_update();
            until([&] { return h.server.downloads == 1; }, "Close fixture did not start");
            h.flow.close();
            until([&] { return h.flow.closed(); }, "Close did not drain download");
            pump(100);
            check(h.wire.live_requests() == 0 && h.server.downloads == 1 &&
                      h.flow.ready_file().isEmpty(),
                  "Close restarted or retained an installable download");
        }
        // Portable discovery can advertise the ZIP but must never create an
        // installable cache file or begin a Setup body request.
        {
            harness h({}, update_mode::portable);
            h.flow.request_update();
            until([&] { return h.flow.updates() == update_status::available; },
                  "Portable discovery failed");
            pump(50);
            check(h.server.downloads == 0 && h.flow.ready_file().isEmpty() &&
                      h.flow.update_metadata()["portableUrl"].toString().endsWith("-win-x64.zip"),
                  "Portable attempted a Setup download");
        }
        for (const auto* installed : {"2026.11.26.2", "2026.11.26.3"}) {
            harness h({}, update_mode::installed, installed);
            h.flow.request_update();
            until([&] { return h.flow.updates() == update_status::current; },
                  "Equal/old comparison failed");
            check(h.server.downloads == 0, "Equal or older release downloaded");
        }
        std::cout
            << "PASS native feed identity, newer-only versions, redirect allowlist, bounded streamed downloads, corrupt/truncated refusal, cache revalidation, scan drain/resume, close drain and portable isolation.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
