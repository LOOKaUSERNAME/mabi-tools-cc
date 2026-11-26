#include "service_policy.hpp"
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QTimeZone>
#include <QUrl>
#include <array>
#include <stdexcept>

namespace cc::service_policy {
namespace {
[[noreturn]] void invalid() {
    throw std::invalid_argument("Invalid service document");
}

void require(bool condition) {
    if (!condition)
        invalid();
}

QRegularExpression re(const QString& pattern) {
    return QRegularExpression(pattern, QRegularExpression::CaseInsensitiveOption);
}

bool matches(const QString& value, const QString& pattern) {
    return re(pattern).match(value).hasMatch();
}

QString text(const QJsonObject& object, const QString& key) {
    const auto value = object.value(key);
    require(value.isString());
    return value.toString();
}

QJsonObject object(const QJsonValue& value) {
    require(value.isObject());
    return value.toObject();
}

qint64 integer(const QJsonValue& value) {
    require(value.isDouble());
    const auto number = value.toInteger(-1);
    require(number >= 0);
    return number;
}

void bound_depth(const QJsonValue& value, int remaining) {
    if (!value.isObject() && !value.isArray())
        return;
    require(remaining > 0);
    if (value.isObject()) {
        const auto obj = value.toObject();
        for (auto it = obj.constBegin(); it != obj.constEnd(); ++it)
            bound_depth(it.value(), remaining - 1);
    } else {
        for (const auto& item : value.toArray())
            bound_depth(item, remaining - 1);
    }
}

QJsonObject document(const QByteArray& body, qsizetype maximum, int depth) {
    require(body.size() <= maximum);
    QJsonParseError error;
    const auto parsed = QJsonDocument::fromJson(body, &error);
    require(error.error == QJsonParseError::NoError && parsed.isObject());
    const auto root = parsed.object();
    bound_depth(root, depth);
    return root;
}

using date_version = std::array<int, 4>;

date_version parse_version(const QString& value) {
    static const QRegularExpression format(
        R"(\A([0-9]{4})\.([1-9]|1[0-2])\.([0-9]{2})(?:\.([1-9][0-9]{0,4}))?\z)");
    const auto parts = format.match(value);
    require(parts.hasMatch());
    date_version result{};
    for (int i = 0; i < 4; ++i)
        result[static_cast<std::size_t>(i)] = parts.captured(i + 1).toInt();
    require(QDate(result[0], result[1], result[2]).isValid() && result[0] >= 1 &&
            result[3] <= 65535);
    return result;
}

bool trusted_steam(const QString& value) {
    const QUrl url(value, QUrl::StrictMode);
    if (!url.isValid() || url.scheme() != "https" || url.authority().contains('@') ||
        url.port(443) != 443 || url.hasQuery() || url.hasFragment())
        return false;
    const auto path = url.adjusted(QUrl::NormalizePathSegments).path(QUrl::FullyEncoded);
    return (url.host() == "steamstore-a.akamaihd.net" &&
            path.startsWith("/news/externalpost/steam_community_announcements/")) ||
           (url.host() == "store.steampowered.com" && path.startsWith("/news/app/212200/view/")) ||
           (url.host() == "steamcommunity.com" &&
            path.startsWith("/games/212200/announcements/detail/"));
}

QString plain(QString value) {
    // Do not construct HTML objects or fetch embedded resources. Only bounded
    // recognition text is used; no original markup enters a rendered widget.
    const auto strip =
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption;
    value.replace(QRegularExpression(R"(<(script|style)\b[^>]*>.*?</\1>)", strip), " ");
    value.replace(QRegularExpression(R"(\[(?:img|url)[^\]]*\].*?\[/(?:img|url)\])", strip), " ");
    value.replace(QRegularExpression(R"(<[^>]*>|\[[^\]]*\])"), " ");
    // Decode a bounded vocabulary of named entities plus numeric entities;
    // unrecognised entity text remains unchanged.
    static const QRegularExpression entity(
        R"(&(?:#([0-9]+)|#x([0-9a-fA-F]+)|(nbsp|amp|quot|apos|lt|gt));)");
    auto entities = entity.globalMatch(value);
    QString decoded;
    qsizetype offset = 0;
    while (entities.hasNext()) {
        const auto m = entities.next();
        decoded += value.mid(offset, m.capturedStart() - offset);
        bool ok = false;
        uint code =
            m.captured(1).isEmpty() ? m.captured(2).toUInt(&ok, 16) : m.captured(1).toUInt(&ok, 10);
        if (const auto name = m.captured(3); !name.isEmpty()) {
            ok = true;
            if (name == "nbsp")
                code = 160;
            else if (name == "amp")
                code = 38;
            else if (name == "quot")
                code = 34;
            else if (name == "apos")
                code = 39;
            else if (name == "lt")
                code = 60;
            else
                code = 62;
        }
        if (ok && code <= 0x10ffff && !(code >= 0xd800 && code <= 0xdfff)) {
            const auto scalar = static_cast<char32_t>(code);
            decoded += QString::fromUcs4(&scalar, 1);
        } else
            decoded += m.captured();
        offset = m.capturedEnd();
    }
    decoded += value.mid(offset);
    return decoded.simplified();
}

bool non_game(const QString& title) {
    return matches(title, R"(\b(website|web\s*site|cash\s*shop|shop|launcher|windows)\b)");
}

const QString months =
    "January February March April May June July August September October November December";
const QString date_pattern = "(?<month>" + QString(months).replace(' ', '|') +
                             R"()\s+(?<day>\d{1,2})(?:st|nd|rd|th)?(?:,?\s+(?<year>20\d{2}))?)";
const QString game =
    R"(\bMabinogi will have (?:a |an )?(?:(?:scheduled|unscheduled|emergency)\s+)?maintenance on\b)";
const QString clock_pattern =
    R"((?<time>\d{1,2}:\d{2})\s*(?<meridiem>AM|PM)\s+(?<zone>PST|PDT|UTC|GMT)\b)";

QDateTime at(const QDate& date, const QRegularExpressionMatch& parts) {
    const auto clock = parts.captured("time").split(':');
    if (clock.size() != 2)
        return {};
    const int h = clock[0].toInt(), m = clock[1].toInt();
    if (h < 1 || h > 12 || m < 0 || m > 59)
        return {};
    const bool pm = parts.captured("meridiem").toUpper() == "PM";
    const auto zone = parts.captured("zone").toUpper();
    return QDateTime(date, QTime(h % 12 + (pm ? 12 : 0), m),
                     QTimeZone::fromSecondsAheadOfUtc(zone == "PST"   ? -28800
                                                      : zone == "PDT" ? -25200
                                                                      : 0))
        .toUTC();
}

bool valid_date(const QDate& date) {
    return date.isValid() && date.year() >= 2004 && date.year() <= 9998;
}

// Recognise only an explicit completed game-maintenance notice. Establish game
// context, one completion time and an unambiguous date before converting to UTC.
// Missing/conflicting evidence returns no boundary instead of guessing a reset.
QDateTime completion(QString title, QString content, const QString& url,
                     const QDateTime& publication, const QDateTime& now) {
    if (title.size() > 1000 || content.size() > 200000 || !publication.isValid() ||
        publication > now || !trusted_steam(url))
        return {};
    title = plain(title);
    content = plain(content);
    if (!matches(title, R"(\bmaintenance\b)") || non_game(title))
        return {};
    const bool outage =
        matches(content, game) && matches(content, R"(\bthe game will be unavailable\b)");
    const bool reopened =
        matches(content, R"(\bMabinogi (?:game )?servers?\b)") &&
        matches(content, R"(\bservers are (?:available once more|now (?:available|open))\b)");
    if (!outage && !reopened)
        return {};
    const QString prefix =
        R"(\bmaintenance (?:has )?(?:concluded|completed|ended)(?: as of| at)\s+)";
    const auto match = re(prefix + clock_pattern + R"((?<tail>[^.!?]{0,120}))").match(content);
    if (!match.hasMatch() ||
        matches(match.captured(), R"(\b(?:expected|scheduled|planned|will)\b)"))
        return {};
    auto mentions = re(prefix + R"(\d)").globalMatch(content);
    if (!mentions.hasNext())
        return {};
    mentions.next();
    if (mentions.hasNext())
        return {};
    const auto explicit_date = re(R"(\bon\s+)" + date_pattern).match(match.captured("tail"));
    const auto title_date = re(date_pattern).match(title);
    const auto event_date = re(game + R"(\s+)" + date_pattern).match(content);
    const auto chosen = explicit_date.hasMatch() ? explicit_date : title_date;
    if (!chosen.hasMatch())
        return {};
    if (!explicit_date.hasMatch() &&
        (!event_date.hasMatch() ||
         title_date.captured("month").compare(event_date.captured("month"), Qt::CaseInsensitive) !=
             0 ||
         title_date.captured("day") != event_date.captured("day") ||
         (!title_date.captured("year").isEmpty() && !event_date.captured("year").isEmpty() &&
          title_date.captured("year") != event_date.captured("year"))))
        return {};
    const int month =
        static_cast<int>(months.toLower().split(' ').indexOf(chosen.captured("month").toLower())) +
        1;
    const int day = chosen.captured("day").toInt();
    int year = chosen.captured("year").toInt();
    // A missing year is inferred only when exactly one neighbouring year places
    // the stated date within seven days of publication. Ambiguity is rejection.
    if (!year) {
        for (int candidate = publication.date().year() - 1;
             candidate <= publication.date().year() + 1; ++candidate) {
            const QDate date(candidate, month, day);
            if (!valid_date(date) ||
                qAbs(QDateTime(date, QTime(12, 0), QTimeZone::utc()).secsTo(publication)) >
                    7 * 86400)
                continue;
            if (year)
                return {};
            year = candidate;
        }
    }
    const QDate date(year, month, day);
    if (!valid_date(date))
        return {};
    const auto finished = at(date, match);
    if (!finished.isValid() || finished > now || finished < publication.addDays(-1))
        return {};
    if (!explicit_date.hasMatch()) {
        if (matches(content, R"(\b(?:overnight|next day|following day|extended (?:until|to))\b)"))
            return {};
        const auto start =
            re(R"(\bmaintenance is expected to begin at\s+)" + clock_pattern).match(content);
        const auto beginning = at(date, start);
        if (!beginning.isValid() || finished < beginning)
            return {};
    }
    return finished;
}
}

namespace {
QJsonObject checked_update(const QByteArray& body, const QString& installed, const QString& commit,
                           bool allow_install) {
    const auto root = document(body, 64 * 1024, 16);
    require(integer(root["schemaVersion"]) == 1 && text(root, "productId") == "cc" &&
            text(root, "channel") == "stable");
    const auto release = object(root["release"]);
    const auto candidate = text(release, "version");
    const auto version = parse_version(candidate);
    const auto current = parse_version(installed);
    const QString tag = "cc-v" + candidate,
                  hub = "https://github.com/LOOKaUSERNAME/mabi-tools-releases/releases/";
    require(text(release, "centralTag") == tag &&
            text(release, "releaseUrl") == hub + "tag/" + tag);
    require(release["assets"].isArray());
    QJsonObject installer;
    for (const auto& asset : release["assets"].toArray()) {
        const auto entry = asset.toObject();
        if (entry.value("role").toString() != "installer")
            continue;
        require(installer.isEmpty());
        installer = entry;
    }
    const QString name = "MabiTools-Channel-Checker-" + candidate + "-Setup-x64.exe";
    require(text(installer, "name") == name &&
            text(installer, "downloadUrl") == hub + "download/" + tag + "/" + name);
    const auto bytes = integer(installer["size"]);
    require(
        bytes > 0 && bytes <= 256 * 1024 * 1024 &&
        QRegularExpression(R"(\A[0-9a-fA-F]{64}\z)").match(text(installer, "sha256")).hasMatch());
    QString source;
    if (release.contains("source")) {
        const auto metadata = object(release["source"]);
        source = text(metadata, "commit");
        require(text(metadata, "repository") == "LOOKaUSERNAME/mabi-tools-cc" &&
                text(metadata, "tag") == "v" + candidate &&
                QRegularExpression(R"(\A[0-9a-f]{40}\z)").match(source).hasMatch());
    }
    const bool available = version > current ||
                           (!allow_install && version == current && !commit.isEmpty() &&
                            !source.isEmpty() && commit.compare(source, Qt::CaseInsensitive) != 0);
    QJsonObject result{{"accepted", true},
                       {"available", available},
                       {"version", candidate},
                       {"sourceCommit", source.isEmpty() ? QJsonValue() : QJsonValue(source)}};
    if (allow_install && available) {
        // A GitHub tag and installer name are not proof that a download is valid.
        // Preserve the verified feed constraints for the downloader to enforce.
        require(!source.isEmpty());
        result.insert("installer", QJsonObject{{"name", name},
                                               {"url", text(installer, "downloadUrl")},
                                               {"size", bytes},
                                               {"sha256", text(installer, "sha256").toLower()}});
    }
    return result;
}
} // namespace

QJsonObject update(const QByteArray& body, const QString& installed, const QString& commit) {
    return checked_update(body, installed, commit, false);
}

QJsonObject installable_update(const QByteArray& body, const QString& installed) {
    return checked_update(body, installed, {}, true);
}

QJsonObject native_update(const QByteArray& body, const QString& installed) {
    const auto root = document(body, 64 * 1024, 16);
    require(text(root, "feedId") == "cc-native-stable-v1" &&
            text(root, "architecture") == "windows-x64" &&
            text(root, "installerAppId") == "{B81A179F-E1D0-471B-B274-B521D71C8088}");
    auto result = installable_update(body, installed);
    const auto release = object(root["release"]);
    const auto metadata = object(release["source"]);
    require(
        QRegularExpression(R"(\A[0-9a-f]{40}\z)").match(text(metadata, "tagObject")).hasMatch());
    const auto version = result["version"].toString();
    QJsonObject portable;
    for (const auto& value : release["assets"].toArray()) {
        const auto entry = object(value);
        const auto role = text(entry, "role");
        require(role == "installer" || role == "portable");
        if (role == "portable") {
            require(portable.isEmpty());
            portable = entry;
        }
    }
    require(release["assets"].toArray().size() == 2);
    const QString name = "MabiTools-Channel-Checker-" + version + "-win-x64.zip";
    const QString url =
        "https://github.com/LOOKaUSERNAME/mabi-tools-releases/releases/download/cc-v" + version +
        "/" + name;
    const auto bytes = integer(portable["size"]);
    require(
        text(portable, "name") == name && text(portable, "downloadUrl") == url && bytes > 0 &&
        bytes <= 256 * 1024 * 1024 &&
        QRegularExpression(R"(\A[0-9a-fA-F]{64}\z)").match(text(portable, "sha256")).hasMatch());
    result["releaseUrl"] = text(release, "releaseUrl");
    result["portableUrl"] = url;
    return result;
}

QJsonObject maintenance(const QByteArray& body, const QDateTime& now) {
    require(now.isValid());
    const auto news = object(document(body, 1024 * 1024, 20)["appnews"]);
    require(integer(news["appid"]) == 212200 && news["newsitems"].isArray());
    const auto items = news["newsitems"].toArray();
    require(items.size() <= 100);
    QSet<QString> seen;
    QJsonArray events;
    bool uncertain = false;
    static const QRegularExpression id_pattern(R"(\A[0-9]{1,32}\z)");
    for (const auto& value : items) {
        const auto item = object(value);
        if (text(item, "feedname") != "steam_community_announcements")
            continue;
        const auto id = text(item, "gid"), title = text(item, "title");
        if (!id_pattern.match(id).hasMatch() || seen.contains(id)) {
            uncertain = true;
            continue;
        }
        seen.insert(id);
        if (!matches(title, R"(\bmaintenance\b)") || non_game(title))
            continue;
        const auto seconds = integer(item["date"]);
        require(seconds <= 253402300799);
        const auto url = text(item, "url");
        const auto finished =
            completion(title, text(item, "contents"), url,
                       QDateTime::fromSecsSinceEpoch(seconds, QTimeZone::utc()), now);
        if (!finished.isValid()) {
            uncertain = true;
            continue;
        }
        events.append(
            QJsonObject{{"sourceId", id}, {"completedUtc", finished.toString(Qt::ISODate)}});
    }
    return {{"accepted", true}, {"events", events}, {"uncertain", uncertain}};
}
}
