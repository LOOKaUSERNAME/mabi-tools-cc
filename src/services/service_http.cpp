#include "service_http.hpp"
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <stdexcept>
#ifdef CC_HTTPS_SERVICES
#include <QSslConfiguration>
#include <QSslSocket>
#endif

namespace cc::service_flow {
#ifndef CC_HTTPS_SERVICES
service_http::service_http(quint16 fixture_port, QString update_root)
    : port_(fixture_port), update_root_(std::move(update_root)) {
    if (!port_)
        throw std::invalid_argument("Expected the listener's assigned loopback port.");
}
#else
service_http::service_http(QString update_root) : update_root_(std::move(update_root)) {
}
#endif
service_http::~service_http() {
    // Do not publish during owner teardown, including when Qt aborts replies.
    for (auto& entry : requests_)
        QObject::disconnect(entry.second.reply, nullptr, this, nullptr);
}

void service_http::bind(coordinator& flow, std::function<void()> refresh_clock) {
    if (flow_ || !refresh_clock)
        throw std::logic_error("Expected a single coordinator and clock.");
    flow_ = &flow;
    refresh_clock_ = std::move(refresh_clock);
}

void service_http::start(request_id id, source feed) {
    if (!flow_ || requests_.contains(id))
        throw std::logic_error("Invalid HTTP request ownership.");
    QUrl url;
#ifdef CC_HTTPS_SERVICES
    {
        // These are the exact shipping metadata endpoints. No destination
        // comes from CLI input, cache, metadata, or HTTP Location.
        if (feed == source::maintenance) {
            url = QUrl(
                "https://api.steampowered.com/ISteamNews/GetNewsForApp/v2/?appid=212200&count=20&maxlength=0&feeds=steam_community_announcements");
        } else if (feed == source::update_fallback) {
            url = QUrl(
                "https://api.github.com/repos/LOOKaUSERNAME/mabi-tools-releases/contents/channels/cc/native-stable-v1.json?ref=main");
        } else {
            url = QUrl(
                "https://raw.githubusercontent.com/LOOKaUSERNAME/mabi-tools-releases/main/channels/cc/native-stable-v1.json");
        }
    }
#else
    {
        const QString path = feed == source::update_primary    ? "/update/primary"
                             : feed == source::update_fallback ? "/update/fallback"
                                                               : "/maintenance";
        url.setScheme("http");
        url.setHost("127.0.0.1");
        url.setPort(port_);
        url.setPath(path);
    }
#endif
    auto manager = std::make_unique<QNetworkAccessManager>();
#ifndef CC_HTTPS_SERVICES
    manager->setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
#endif
    QNetworkRequest request_spec(url);
#ifdef CC_HTTPS_SERVICES
    {
        auto tls = QSslConfiguration::defaultConfiguration();
        tls.setProtocol(QSsl::TlsV1_2OrLater);
        tls.setPeerVerifyMode(QSslSocket::VerifyPeer);
        request_spec.setSslConfiguration(tls);
        request_spec.setRawHeader("User-Agent", "MabiTools-ChannelChecker/1");
        if (feed == source::update_fallback) {
            request_spec.setRawHeader("Accept", "application/vnd.github.raw+json");
            request_spec.setRawHeader("X-GitHub-Api-Version", "2026-03-10");
        }
    }
#endif
    request_spec.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                              QNetworkRequest::ManualRedirectPolicy);
    request_spec.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    request_spec.setAttribute(QNetworkRequest::AutoDeleteReplyOnFinishAttribute, false);
    request_spec.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request_spec.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    if (!request_spec.hasRawHeader("Accept"))
        request_spec.setRawHeader("Accept", "application/json");
    request_spec.setRawHeader("Connection", "close");
    request_spec.setRawHeader("Cache-Control", "no-cache");
    auto* reply = manager->get(request_spec);
    reply->setReadBufferSize(16 * 1024);
    requests_.emplace(id, request{std::move(manager), reply, false, false, false});
    QObject::connect(reply, &QNetworkReply::metaDataChanged, this, [this, id] { consume(id); });
    QObject::connect(reply, &QNetworkReply::readyRead, this, [this, id] { consume(id); });
    QObject::connect(reply, &QNetworkReply::finished, this, [this, id] { finished(id); });
}

void service_http::abort(request_id id) {
    if (download_ && download_id_ == id) {
        download_cancelled_ = true;
        download_->cancel();
        return;
    }
    auto it = requests_.find(id);
    if (it == requests_.end() || it->second.cancelled)
        throw std::logic_error("Invalid HTTP abort.");
    it->second.cancelled = true;
    // abort() may emit finished synchronously. Avoid calling it on the
    // coordinator's call stack, including while handling headers or body.
    QTimer::singleShot(0, this, [this, id] {
        const auto entry = requests_.find(id);
        if (entry == requests_.end())
            return;
        auto* reply = entry->second.reply;
        if (!reply->isFinished())
            reply->abort();
        if (reply->isFinished())
            dispose(id, false);
    });
}

void service_http::start_download(request_id id, const QJsonObject& asset) {
    if (!flow_ || download_)
        throw std::logic_error("Invalid download ownership.");
    download_id_ = id;
    download_cancelled_ = false;
    download_ = std::make_unique<update_download>(
        update_root_, asset,
#ifndef CC_HTTPS_SERVICES
        port_,
#endif
        [this, id](bool cancelled, bool verified, QString path, QString error) {
            QTimer::singleShot(0, this, [this, id, cancelled, verified, path, error] {
                // Release the downloader outside its own completion callback.
                // A cancelled request acknowledges drainage, never readiness to
                // install, even if verification raced with the cancellation.
                const bool retired = cancelled || download_cancelled_;
                download_.reset();
                download_id_ = 0;
                refresh_clock_();
                if (retired)
                    flow_->drained(id);
                else
                    flow_->download_complete(id, verified, path, error);
            });
        });
    // Starting a cached download may finish immediately; callbacks remain queued.
    QTimer::singleShot(0, this, [this, id] {
        if (download_ && download_id_ == id)
            download_->start();
    });
}

// Deliver validated headers before bounded body chunks. Refreshing the clock
// can expire and cancel this request, so recheck cancellation before handing
// bytes to the coordinator. Disposal stays deferred until outside these signals.
void service_http::consume(request_id id) {
    auto it = requests_.find(id);
    if (it == requests_.end() || it->second.cancelled)
        return;
    refresh_clock_();
    auto& item = it->second;
    if (item.cancelled)
        return;
    if (!item.headers_sent) {
        const auto status = item.reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        if (!status.isValid())
            return;
        std::optional<qint64> length;
        const auto advertised = item.reply->rawHeader("Content-Length");
        if (!advertised.isEmpty()) {
            bool ok = false;
            const auto bytes = advertised.toLongLong(&ok);
            length = ok ? bytes : -1;
        }
        item.headers_sent = true;
        flow_->headers(id, status.toInt(), length);
    }
    while (!item.cancelled && item.reply->bytesAvailable() > 0) {
        const auto bytes = item.reply->read(8 * 1024);
        if (bytes.isEmpty())
            break;
        refresh_clock_();
        if (!item.cancelled) {
            received_ += bytes.size();
            flow_->body(id, bytes);
        }
    }
}

void service_http::finished(request_id id) {
    const auto it = requests_.find(id);
    if (it == requests_.end())
        return;
    consume(id);
    dispose(id, !it->second.cancelled && it->second.reply->error() == QNetworkReply::NoError);
}

void service_http::dispose(request_id id, bool ok) {
    const auto it = requests_.find(id);
    if (it == requests_.end() || it->second.dispose_pending)
        return;
    it->second.dispose_pending = true;
    // Free the reply and its private connection manager outside Qt's signal
    // handler before emitting the ownership acknowledgement. The fixture
    // independently witnesses the peer socket disconnect.
    QTimer::singleShot(0, this, [this, id, ok] {
        auto handle = requests_.extract(id);
        if (handle.empty())
            return;
        auto item = std::move(handle.mapped());
#ifdef CC_HTTPS_SERVICES
        {
            diagnostics_.append(QJsonObject{
                {"url", item.reply->url().toString()},
                {"host", item.reply->url().host()},
                {"httpStatus",
                 item.reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()},
                {"qtError", static_cast<int>(item.reply->error())},
                {"aborted", item.cancelled}});
        }
#endif
        QObject::disconnect(item.reply, nullptr, this, nullptr);
        item.manager.reset();
        // All queued signals have either been consumed or disconnected. Only
        // now can a retired operation unblock retry or scan admission.
        if (item.cancelled)
            flow_->drained(id);
        else
            flow_->complete(id, ok);
    });
}
}
