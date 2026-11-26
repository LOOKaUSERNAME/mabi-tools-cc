#pragma once
#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <optional>

namespace cc::notice_cache {
// Application-owned cache format, stored separately from WPF profile data.
struct snapshot {
    QDateTime fetched;
    QJsonObject event;
    bool uncertain{};
};
enum class state { absent, fresh, stale, unknown };

struct read_result {
    state status;
    std::optional<snapshot> value;
};
enum class write_result { saved, kept_newer, refused };

// Bounded, atomic profile cache. read reports absent/unknown instead of using
// invalid data; stale snapshots still retain their last confirmed event. save
// locks across processes and refuses damaged input or an older fetch, preserving
// a newer record. A failed refresh must not be saved as an empty replacement.
class store {
public:
    explicit store(const QString& profile_root);
    read_result read(const QDateTime& now) const;
    write_result save(const snapshot&, const QDateTime& now) const;

private:
    QString root_, path_;
};
}
