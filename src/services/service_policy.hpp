#pragma once
#include <QByteArray>
#include <QDateTime>
#include <QJsonObject>
#include <QString>

namespace cc::service_policy {
// In-memory policy only. No transport, cache, installer, process launch or
// profile access. Callers must never treat metadata availability as ready to install.
// Malformed or mismatched documents throw invalid_argument. Native discovery
// validates product/channel/feed, source tag and exact asset identities before
// returning available=true for a strictly newer version. Equal/older versions
// carry no installable asset. Maintenance accepts confirmed completion times,
// not estimates; the coordinator retains the prior event when a fetch fails.
QJsonObject update(const QByteArray& body, const QString& installed, const QString& commit);
// Installer discovery accepts strictly newer releases with source provenance.
// Still requires a verified download and explicit consent before installation.
QJsonObject installable_update(const QByteArray& body, const QString& installed);
QJsonObject native_update(const QByteArray& body, const QString& installed);
QJsonObject maintenance(const QByteArray& body, const QDateTime& now);
}
