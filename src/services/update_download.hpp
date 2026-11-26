#pragma once
#include <QObject>
#include <QJsonObject>
#include <QUrl>
#include <functional>
#include <memory>

namespace cc::service_flow {
// URL policy alone is not file verification. Asset descriptors must first pass
// native feed validation. File checks also require an app-owned cache path and
// exact size/hash; a successful check does not remain valid after file changes.
bool trusted_asset_url(const QUrl& url);
bool is_setup_install();
bool verify_update_file(const QString& root, const QString& path, const QJsonObject& asset);
// Call only after consent and application work has drained. Rechecks Setup
// identity and hashes under a file handle denying writes/deletion, then launches
// interactive Setup. Returns false with error text on refusal or launch failure.
bool launch_verified_update(const QString& root, const QString& path, const QJsonObject& asset,
                            QString& error);

// One bounded stream and its private connection manager. Completion is emitted
// only after both are disposed, so the coordinator can admit measured probes.
// Use once: start, then optional cancel, and retain the object until completion.
// Calls and completion run on its QObject thread. Streaming uses an app-owned
// partial file, exact declared bytes/SHA-256, 256 MiB maximum, a 15-minute total
// deadline and 30-second idle limit. A cached file must pass the same check.
class update_download final : public QObject {
public:
    using completion =
        std::function<void(bool cancelled, bool verified, QString path, QString error)>;
    update_download(QString root, QJsonObject asset,
#ifndef CC_HTTPS_SERVICES
                    quint16 fixture_port,
#endif
                    completion done);
    ~update_download() override;
    void start();
    void cancel();

private:
    struct state;
    std::unique_ptr<state> state_;
    void request(const QUrl&);
    void consume();
    void finished();
    void finish(bool ok, const QString& error = {});
};
}
