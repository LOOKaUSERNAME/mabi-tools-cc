#include "update_download.hpp"
#include "profile_paths.hpp"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLockFile>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSettings>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QTimer>
#include <windows.h>
#include <array>
#include <vector>

namespace cc::service_flow {
namespace {
constexpr qint64 maximum_bytes = 256LL * 1024 * 1024;

QString cache_path(const QString& root, const QJsonObject& asset) {
    const auto hash = asset["sha256"].toString();
    if (!QRegularExpression("\\A[0-9a-f]{64}\\z").match(hash).hasMatch())
        throw std::invalid_argument("Invalid update checksum.");
    return cc::ui::checked_profile_file(root, "update-" + hash + ".exe");
}

struct locked_file {
    HANDLE handle{INVALID_HANDLE_VALUE};

    ~locked_file() {
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
    }

    bool verify(const QString& root, const QString& path, const QJsonObject& asset) {
        try {
            if (path != cache_path(root, asset))
                return false;
        } catch (const std::exception&) {
            return false;
        }
        const auto size = asset["size"].toInteger(-1);
        if (size <= 0 || size > maximum_bytes)
            return false;
        handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ, FILE_SHARE_READ,
                             nullptr, OPEN_EXISTING,
                             FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return false;
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(handle, &info) ||
            info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY) ||
            info.nNumberOfLinks != 1 ||
            (static_cast<quint64>(info.nFileSizeHigh) << 32 | info.nFileSizeLow) !=
                static_cast<quint64>(size))
            return false;
        QCryptographicHash hash(QCryptographicHash::Sha256);
        std::array<char, 65536> block{};
        DWORD received{};
        for (;;) {
            if (!ReadFile(handle, block.data(), static_cast<DWORD>(block.size()), &received,
                          nullptr))
                return false;
            if (!received)
                break;
            hash.addData(QByteArrayView(block.data(), static_cast<qsizetype>(received)));
        }
        return hash.result().toHex() == asset["sha256"].toString().toLatin1();
    }
};
}

bool trusted_asset_url(const QUrl& url) {
    if (!url.isValid() || url.scheme() != "https" || !url.userInfo().isEmpty() ||
        url.hasFragment() || url.port(443) != 443)
        return false;
    const auto host = url.host();
    if (host == "github.com")
        return url.path().startsWith("/LOOKaUSERNAME/mabi-tools-releases/releases/download/cc-v");
    return host == "release-assets.githubusercontent.com" ||
           host == "objects.githubusercontent.com";
}

bool is_setup_install() {
    const auto expected =
        QDir::cleanPath(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
                        "/Programs/MabiTools/Channel Checker");
    const auto actual = QCoreApplication::applicationDirPath();
    if (actual.compare(expected, Qt::CaseInsensitive) != 0 ||
        QDir(actual).canonicalPath().compare(actual, Qt::CaseInsensitive) != 0)
        return false;
    const auto marker = QDir(actual).filePath("install-kind.ini");
    const auto attrs = GetFileAttributesW(reinterpret_cast<LPCWSTR>(marker.utf16()));
    if (attrs == INVALID_FILE_ATTRIBUTES ||
        attrs & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))
        return false;
    QSettings package(marker, QSettings::IniFormat);
    if (package.value("Package/Kind").toString() != "setup" ||
        package.value("Package/Product").toString() != "cc-native")
        return false;
    QSettings installed(
        "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{B81A179F-E1D0-471B-B274-B521D71C8088}_is1",
        QSettings::NativeFormat);
    const auto name = installed.value("DisplayName").toString();
    // Accept the previous display label while retaining all native identity checks.
    return QDir::cleanPath(
               QDir::fromNativeSeparators(installed.value("InstallLocation").toString()))
                   .compare(expected, Qt::CaseInsensitive) == 0 &&
           (name == "Channel Checker" || name == "MabiTools Channel Checker");
}

bool verify_update_file(const QString& root, const QString& path, const QJsonObject& asset) {
    locked_file file;
    return file.verify(root, path, asset);
}

bool launch_verified_update(const QString& root, const QString& path, const QJsonObject& asset,
                            QString& error) {
    // Deny writes/deletion from the final rehash through CreateProcess. A shell,
    // metadata-supplied arguments and silent installer switches are never used.
    locked_file file;
    if (!is_setup_install() || !file.verify(root, path, asset)) {
        error = "The downloaded update no longer verifies. Check for updates again.";
        return false;
    }
    const auto command = '"' + QDir::toNativeSeparators(path) + "\" /SP- /NORESTART";
    std::vector<wchar_t> arguments(static_cast<std::size_t>(command.size() + 1));
    command.toWCharArray(arguments.data());
    arguments.back() = L'\0';
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(reinterpret_cast<LPCWSTR>(path.utf16()), arguments.data(), nullptr, nullptr,
                        FALSE, 0, nullptr, nullptr, &startup, &process)) {
        error = "Couldn't start the verified installer (Windows error " +
                QString::number(GetLastError()) + ").";
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

struct update_download::state {
    QString root, path;
    QJsonObject asset;
#ifndef CC_HTTPS_SERVICES
    quint16 port{};
#endif
    completion done;
    std::unique_ptr<QNetworkAccessManager> manager;
    QNetworkReply* reply{};
    std::unique_ptr<QLockFile> lock;
    std::unique_ptr<QTemporaryFile> file;
    QCryptographicHash hash{QCryptographicHash::Sha256};
    QTimer timer;
    QElapsedTimer elapsed, idle;
    QUrl logical_url;
    qint64 received{}, expected{};
    int redirects{};
    bool header{}, redirect{}, cancelled{}, finishing{};
};

update_download::update_download(QString root, QJsonObject asset,
#ifndef CC_HTTPS_SERVICES
                                 quint16 port,
#endif
                                 completion done)
    : state_(std::make_unique<state>()) {
    state_->root = std::move(root);
    state_->asset = std::move(asset);
    state_->done = std::move(done);
#ifndef CC_HTTPS_SERVICES
    state_->port = port;
#endif
}

update_download::~update_download() = default;

// Acquire exclusive cache ownership before reuse or replacement. A verified
// existing file completes without network work; otherwise stream into a separate
// temporary file and route every failure through the same deferred cleanup.
void update_download::start() {
    auto& s = *state_;
    if (s.cancelled || s.finishing)
        return;
    try {
        s.path = cache_path(s.root, s.asset);
        s.expected = s.asset["size"].toInteger(-1);
        if (s.expected <= 0 || s.expected > maximum_bytes ||
            !trusted_asset_url(QUrl(s.asset["url"].toString())))
            throw std::invalid_argument("Invalid installer download metadata.");
        s.lock =
            std::make_unique<QLockFile>(cc::ui::checked_profile_file(s.root, "native-update.lock"));
        if (!s.lock->tryLock(0))
            throw std::runtime_error("Another Channel Checker process is managing an update.");
        if (verify_update_file(s.root, s.path, s.asset)) {
            finish(true);
            return;
        }
        if (QFileInfo::exists(s.path) && !QFile::remove(s.path))
            throw std::runtime_error("Cannot replace an invalid cached update.");
        s.file = std::make_unique<QTemporaryFile>(QDir(s.root).filePath("update-XXXXXX.part"));
        if (!s.file->open())
            throw std::runtime_error("Cannot create the update download file.");
        // Bound both total download time (15 minutes) and time without progress
        // (30 seconds). Redirects reuse these clocks rather than renewing the budget.
        s.elapsed.start();
        s.idle.start();
        s.timer.setInterval(100);
        connect(&s.timer, &QTimer::timeout, this, [this] {
            if (state_->elapsed.elapsed() >= 15 * 60 * 1000 || state_->idle.elapsed() >= 30 * 1000)
                finish(false,
                       "The update download timed out. Try again or open the downloads page.");
        });
        s.timer.start();
        request(QUrl(s.asset["url"].toString()));
    } catch (const std::exception& error) {
        finish(false, QString::fromUtf8(error.what()));
    }
}

void update_download::cancel() {
    state_->cancelled = true;
    finish(false);
}

void update_download::request(const QUrl& url) {
    auto& s = *state_;
    if (!trusted_asset_url(url)) {
        finish(false, "The update redirected to an untrusted address.");
        return;
    }
    s.logical_url = url;
    s.header = false;
    s.redirect = false;
    s.manager = std::make_unique<QNetworkAccessManager>();
    QUrl destination = url;
#ifndef CC_HTTPS_SERVICES
    if (s.port) {
        destination = QUrl("http://127.0.0.1:" + QString::number(s.port) + "/asset" + url.path());
        s.manager->setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
    }
#endif
    QNetworkRequest spec(destination);
    spec.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                      QNetworkRequest::ManualRedirectPolicy);
    spec.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    spec.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    spec.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    spec.setRawHeader("Accept-Encoding", "identity");
    spec.setRawHeader("Connection", "close");
    spec.setRawHeader("User-Agent", "MabiTools-ChannelChecker/1");
    auto tls = QSslConfiguration::defaultConfiguration();
    tls.setProtocol(QSsl::TlsV1_2OrLater);
    tls.setPeerVerifyMode(QSslSocket::VerifyPeer);
    spec.setSslConfiguration(tls);
    s.reply = s.manager->get(spec);
    s.reply->setReadBufferSize(64 * 1024);
    connect(s.reply, &QNetworkReply::metaDataChanged, this, [this] { consume(); });
    connect(s.reply, &QNetworkReply::readyRead, this, [this] { consume(); });
    connect(s.reply, &QNetworkReply::finished, this, [this] { finished(); });
}

// Validate response framing before writing body bytes. Redirect responses are
// handled at completion; their bodies never enter the installer file or hash.
// Count and hash each written chunk, refusing overflow before it reaches disk.
void update_download::consume() {
    auto& s = *state_;
    if (s.finishing || s.cancelled)
        return;
    if (!s.header) {
        const auto status = s.reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        if (!status.isValid())
            return;
        const int code = status.toInt();
        s.header = true;
        if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
            s.redirect = true;
            return;
        }
        if (code != 200) {
            finish(false, "The update server did not return the requested installer.");
            return;
        }
        const auto encoding = s.reply->rawHeader("Content-Encoding");
        if (!encoding.isEmpty() && encoding != "identity") {
            finish(false, "Unexpected encoded installer response.");
            return;
        }
        const auto length = s.reply->rawHeader("Content-Length");
        if (!length.isEmpty()) {
            bool ok = false;
            const auto bytes = length.toLongLong(&ok);
            if (!ok || bytes != s.expected) {
                finish(false, "The update size does not match the release metadata.");
                return;
            }
        }
    }
    if (s.redirect)
        return;
    while (!s.finishing && s.reply->bytesAvailable() > 0) {
        const auto block = s.reply->read(64 * 1024);
        if (block.isEmpty())
            break;
        if (block.size() > s.expected - s.received || s.file->write(block) != block.size()) {
            finish(false, "The update is too large or could not be saved.");
            return;
        }
        s.hash.addData(block);
        s.received += block.size();
        s.idle.restart();
    }
}

void update_download::finished() {
    auto& s = *state_;
    consume();
    if (s.finishing)
        return;

    if (s.redirect) {
        const auto url = s.logical_url.resolved(
            s.reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl());
        if (++s.redirects > 3 || !trusted_asset_url(url)) {
            finish(false, "The update redirected to an untrusted address.");
            return;
        }
        // Release the old connection before requesting the next allowed hop.
        disconnect(s.reply, nullptr, this, nullptr);
        QTimer::singleShot(0, this, [this, url] {
            if (state_->finishing)
                return;
            state_->manager.reset();
            state_->reply = nullptr;
            request(url);
        });
        return;
    }

    if (!s.header || s.reply->error() != QNetworkReply::NoError || s.received != s.expected ||
        s.hash.result().toHex() != s.asset["sha256"].toString().toLatin1()) {
        finish(
            false,
            "The update is incomplete or failed its SHA-256 check. Try again or open the downloads page.");
        return;
    }

    if (!s.file->flush()) {
        finish(false, "Could not flush the verified update.");
        return;
    }
    s.file->close();
    if (!s.file->rename(s.path) || !verify_update_file(s.root, s.path, s.asset)) {
        finish(false, "The saved update failed verification.");
        return;
    }
    s.file->setAutoRemove(false);
    finish(true);
}

// Converge success, failure and cancellation on one deferred cleanup path.
// Disconnect first to prevent re-entrant completion during abort. The callback
// runs only after the manager, partial file and cache lock have been released.
void update_download::finish(bool ok, const QString& error) {
    auto& s = *state_;
    if (s.finishing)
        return;
    s.finishing = true;
    s.timer.stop();
    if (s.reply)
        disconnect(s.reply, nullptr, this, nullptr);

    QTimer::singleShot(0, this, [this, ok, error] {
        auto& current = *state_;
        if (current.reply && !current.reply->isFinished())
            current.reply->abort();
        current.manager.reset();
        current.reply = nullptr;
        current.file.reset();
        current.lock.reset();
        current.done(current.cancelled, ok && !current.cancelled, ok ? current.path : QString{},
                     error);
    });
}
}
