#pragma once
#include "service_flow.hpp"
#include "update_download.hpp"
#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QObject>
#include <QUrl>
#include <functional>
#include <map>
#include <memory>

class QNetworkReply;

namespace cc::service_flow {
// Bounded HTTP transport. Fixtures supply an IPv4 loopback port; the
// application uses fixed HTTPS metadata destinations with redirects disabled.
class service_http final : public QObject, public transport {
public:
#ifdef CC_HTTPS_SERVICES
    explicit service_http(QString update_root = {});

    const QJsonArray& diagnostics() const { return diagnostics_; }
#else
    explicit service_http(quint16 fixture_port, QString update_root = {});
#endif
    ~service_http() override;
    // Bind before starting requests. flow and refresh_clock captures must remain
    // alive until requests drain. All calls/callbacks use this QObject's thread;
    // refresh_clock advances deadlines before delivering a received frame.
    void bind(coordinator& flow, std::function<void()> refresh_clock);
    void start(request_id id, source feed) override;
    void abort(request_id id) override;
    void start_download(request_id, const QJsonObject&) override;

    std::size_t live_requests() const { return requests_.size() + (download_ ? 1U : 0U); }

    qsizetype received_bytes() const { return received_; }

private:
    struct request {
        std::unique_ptr<QNetworkAccessManager> manager;
        QNetworkReply* reply{};
        bool headers_sent{}, cancelled{}, dispose_pending{};
    };
#ifndef CC_HTTPS_SERVICES
    const quint16 port_;
#endif
    QString update_root_;
    std::unique_ptr<update_download> download_;
    request_id download_id_{};
    bool download_cancelled_{};
    coordinator* flow_{};
    qsizetype received_{};
    std::function<void()> refresh_clock_;
    std::map<request_id, request> requests_;
#ifdef CC_HTTPS_SERVICES
    QJsonArray diagnostics_;
#endif
    void consume(request_id id);
    void finished(request_id id);
    void dispose(request_id id, bool ok);
};
}
