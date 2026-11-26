#include "notice_cache.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <stdexcept>

namespace cc::notice_cache {
namespace {
constexpr qint64 max_bytes = 16 * 1024;

bool ordinary_root(const QString& path) {
    const QFileInfo info(path);
    return info.isDir() && !info.isSymLink() &&
           QDir::cleanPath(info.canonicalFilePath())
                   .compare(QDir::cleanPath(path),
#ifdef Q_OS_WIN
                            Qt::CaseInsensitive
#else
                            Qt::CaseSensitive
#endif
                            ) == 0;
}

bool ordinary_file(const QFileInfo& info) {
    return !info.isSymLink() && (!info.exists() || info.isFile());
}

QDateTime event_time(const QJsonObject& value) {
    return QDateTime::fromString(value.value("completedUtc").toString(), Qt::ISODate);
}

bool valid_pair(const snapshot& value, const QDateTime& now) {
    if (!now.isValid() || !value.fetched.isValid() || value.fetched.date().year() < 2004 ||
        value.fetched > now || (value.event.isEmpty() && !value.uncertain))
        return false;
    if (value.event.isEmpty())
        return true;
    const auto date = event_time(value.event);
    static const QRegularExpression id(R"(\A[0-9]{1,32}\z)");
    return value.event.size() == 2 && date.isValid() && date.offsetFromUtc() == 0 &&
           date.date().year() >= 2004 && date <= value.fetched &&
           id.match(value.event.value("sourceId").toString()).hasMatch();
}
}

store::store(const QString& root) : root_(QDir(root).absolutePath()) {
    if (!ordinary_root(root_))
        throw std::invalid_argument("Expected an ordinary profile cache directory.");
    path_ = QDir(root_).filePath("native-notice-test.json");
}

// Treat missing, malformed and stale cache data separately. A valid stale value
// may retain a confirmed boundary; callers can display it while refreshing.
// Reads never repair or overwrite unknown bytes.
read_result store::read(const QDateTime& now) const {
    const QFileInfo info(path_);
    if (!now.isValid() || !ordinary_root(root_) || !ordinary_file(info))
        return {state::unknown, {}};
    if (!info.exists())
        return {state::absent, {}};
    if (info.size() > max_bytes)
        return {state::unknown, {}};
    QFile file(path_);
    if (!file.open(QIODevice::ReadOnly))
        return {state::unknown, {}};
    const auto body = file.read(max_bytes + 1);
    if (body.size() > max_bytes || !file.atEnd() || file.error() != QFile::NoError)
        return {state::unknown, {}};
    QJsonParseError error;
    const auto obj = QJsonDocument::fromJson(body, &error).object();
    const auto item = obj.value("event");
    if (error.error != QJsonParseError::NoError || obj.size() != 4 ||
        obj.value("schema").toInt() != 1 || !obj.value("uncertain").isBool() ||
        !(item.isObject() || item.isNull()))
        return {state::unknown, {}};
    const snapshot result{
        QDateTime::fromString(obj.value("fetchedUtc").toString(), Qt::ISODateWithMs),
        item.toObject(), obj.value("uncertain").toBool()};
    if (!valid_pair(result, now))
        return {state::unknown, {}};
    return {result.fetched.msecsTo(now) >= 3'600'000 ? state::stale : state::fresh, result};
}

// Serialise writers with a non-waiting profile lock, then compare against disk
// again. Refuse unknown data, retain newer fetches and publish through QSaveFile
// so a failed write cannot leave a truncated cache.
write_result store::save(const snapshot& value, const QDateTime& now) const {
    if (!valid_pair(value, now) || !ordinary_root(root_) ||
        !ordinary_file(QFileInfo(path_ + ".lock")))
        return write_result::refused;
    QLockFile lock(path_ + ".lock");
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0))
        return write_result::refused;
    const auto old = read(now);
    if (old.status == state::unknown)
        return write_result::refused;
    if (old.value && old.value->fetched >= value.fetched)
        return write_result::kept_newer;
    // An uncertain fetch must not erase a confirmed event. A different event
    // older than the cached boundary cannot move history comparison backwards.
    auto event = value.event;
    if (old.value && !old.value->event.isEmpty() &&
        (event.isEmpty() || (event.value("sourceId") != old.value->event.value("sourceId") &&
                             event_time(event) < event_time(old.value->event))))
        event = old.value->event;
    const auto json =
        QJsonDocument(QJsonObject{{"schema", 1},
                                  {"fetchedUtc", value.fetched.toUTC().toString(Qt::ISODateWithMs)},
                                  {"event", event.isEmpty() ? QJsonValue() : QJsonValue(event)},
                                  {"uncertain", value.uncertain}})
            .toJson(QJsonDocument::Compact);
    QSaveFile out(path_);
    out.setDirectWriteFallback(false);
    if (!out.open(QIODevice::WriteOnly) || out.write(json) != json.size() || !out.commit())
        return write_result::refused;
    return write_result::saved;
}
}
