#include "history_replay.hpp"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QRegularExpression>
#include <QTimeZone>
#include <cmath>
#include <stdexcept>

namespace cc::ui {
namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

std::int64_t ticks(const QJsonValue& value) {
    const auto text = value.toString();
    const auto date = QDateTime::fromString(text, Qt::ISODateWithMs);
    require(date.isValid() && QRegularExpression(R"((Z|[+-]\d\d:\d\d)$)").match(text).hasMatch(),
            "History timestamp requires an explicit offset");
    // Qt keeps milliseconds; retain the remaining four .NET tick digits so cutoff
    // and ordering comparisons do not collapse observations in the same millisecond.
    const auto match = QRegularExpression(R"(\.(\d{1,7})(?:Z|[+-]\d\d:\d\d)$)").match(text);
    auto fraction = match.hasMatch() ? match.captured(1).leftJustified(7, '0').toInt() : 0;
    return 621355968000000000LL + date.toMSecsSinceEpoch() * 10000 + fraction % 10000;
}

std::optional<double> timing(const QJsonValue& value) {
    if (value.isNull())
        return {};
    require(value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() >= 0,
            "Invalid saved timing");
    return value.toDouble();
}

void same(std::optional<double> actual, QJsonValue expected) {
    const auto reference = timing(expected);
    require(actual.has_value() == reference.has_value(), "History timing presence differs");
    if (actual)
        require(std::abs(*actual - *reference) <= 1e-10 * std::max(1., *reference),
                "History timing differs");
}
}

history_replay history_replay::read(const QString& fixture) {
    QFile file(fixture);
    require(file.open(QIODevice::ReadOnly) && file.size() <= 4 * 1024 * 1024,
            "Cannot open bounded history fixture");
    QJsonParseError error;
    auto document = QJsonDocument::fromJson(file.readAll(), &error).object();
    require(error.error == QJsonParseError::NoError, "Invalid history fixture JSON");
    history_replay result;
    QMap<QString, int> identities;
    auto token = [&](const QString& value) {
        if (!identities.contains(value))
            identities[value] = static_cast<int>(identities.size()) + 1;
        return identities[value];
    };
    auto records = document["Records"].toArray();
    require(records.size() <= 1000, "Too many saved scans");
    for (const auto& value : records) {
        auto record = value.toObject();
        auto options = record["Options"].toObject();
        auto channels = record["Channels"].toArray();
        require(channels.size() <= 30, "Too many saved channels");
        for (const auto& channel_value : channels) {
            auto channel = channel_value.toObject();
            auto cached = channel["Summary"].toObject();
            // Recompute the saved summary before using it. This read-only
            // adapter accepts only the supported history schema and method.
            require(record["SchemaVersion"].toInt() == 1 &&
                        record["SummaryRevision"].toInt() == 1 &&
                        record["MeasurementMethod"].toString() == "tcp-opening-four-bytes-v1",
                    "Unsupported history format");
            std::vector<cc::observation> observations;
            const auto samples = channel["Samples"].toArray();
            require(samples.size() <= 120, "Too many saved samples");
            for (auto sample_value : samples) {
                auto sample = sample_value.toObject();
                observations.push_back({sample["Round"].toInt(), timing(sample["LatencyMs"]),
                                        timing(sample["ConnectMs"]),
                                        !sample["Failure"].toString().trimmed().isEmpty()});
            }
            const auto measured = cc::summarize_history_channel(observations);
            for (auto pair : {std::pair{measured.connection, cached["Connection"].toObject()},
                              std::pair{measured.reply, cached["Reply"].toObject()},
                              std::pair{measured.total, cached["Total"].toObject()}}) {
                same(pair.first.mean, pair.second["Mean"]);
                same(pair.first.minimum, pair.second["Minimum"]);
                same(pair.first.maximum, pair.second["Maximum"]);
            }
            same(measured.total_variation, cached["TotalVariation"]);
            require(measured.attempted == cached["Attempted"].toInt() &&
                        measured.successful == cached["Successful"].toInt() &&
                        measured.failed == cached["Failed"].toInt() &&
                        measured.adjacent_pairs == cached["AdjacentPairs"].toInt(),
                    "Saved summary counts disagree");
            cc::history_row row{};
            row.schema = 1;
            row.revision = 1;
            row.completed = record["State"].toString() == "Completed";
            row.method = token(record["MeasurementMethod"].toString());
            row.region = token(channel["Region"].toString());
            row.host = token(channel["Host"].toString());
            row.channel = channel["Channel"].toInt();
            row.port = channel["Port"].toInt();
            row.timeout_ms = options["TimeoutMs"].toInt();
            row.interval_ms = options["IntervalMs"].toInt();
            row.concurrency = options["MaxConcurrency"].toInt();
            row.started_ticks = ticks(record["StartedAtUtc"]);
            row.completed_ticks = ticks(record["CompletedAtUtc"]);
            auto id = record["ScanId"].toString().remove('-');
            require(id.size() == 32, "Invalid scan ID");
            for (int i = 0; i < 4; ++i) {
                bool valid = false;
                row.scan_key[static_cast<std::size_t>(i)] = id.mid(i * 8, 8).toUInt(&valid, 16);
                require(valid, "Invalid scan ID digits");
            }
            row.mean_total_ms = measured.total.mean;
            row.variation_ms = measured.total_variation;
            row.successful = measured.successful;
            row.attempted = measured.attempted;
            row.failed = measured.failed;
            result.rows.push_back(row);
        }
    }
    auto endpoint = document["Endpoint"].toObject();
    auto options = document["Options"].toObject();
    result.endpoint_csv = QString("%1,%2,%3")
                              .arg(endpoint["Channel"].toInt())
                              .arg(endpoint["Host"].toString())
                              .arg(endpoint["Port"].toInt());
    // Explicit fixture restriction: this is not a production settings/history loader.
    // Only the normalized IPv4 loopback identity generated by the oracle is admitted.
    require(endpoint["Host"].toString() == "127.0.0.1",
            "History replay requires the loopback fixture identity");
    cc::history_query query{};
    query.method = token("tcp-opening-four-bytes-v1");
    query.region = token("NA-Erinn");
    query.host = token("127.0.0.1");
    query.channel = endpoint["Channel"].toInt();
    query.port = endpoint["Port"].toInt();
    query.timeout_ms = options["TimeoutMs"].toInt();
    query.interval_ms = options["IntervalMs"].toInt();
    query.concurrency = options["MaxConcurrency"].toInt();
    query.now_ticks = ticks(document["NowUtc"]);
    query.seven_days_ago_ticks = query.now_ticks - 7LL * 86400 * 10000000;
    result.summary = cc::compare_history(result.rows, query);
    const auto expected = document["Expected"].toObject();
    const auto& summary = result.summary;
    require(static_cast<int>(summary.indices.size()) == expected["ScanCount"].toInt() &&
                summary.cutoff_ticks == ticks(expected["CutoffUtc"]) &&
                summary.timing_scans == expected["TimingScanCount"].toInt() &&
                summary.variation_scans == expected["VariationScanCount"].toInt(),
            "History comparison bounds differ");
    same(summary.typical_total_ms, expected["TypicalTotal"]);
    same(summary.typical_variation_ms, expected["TypicalVariation"]);
    require(summary.successful == expected["SuccessfulChecks"].toInteger() &&
                summary.attempted == expected["AttemptedChecks"].toInteger() &&
                summary.failed == expected["FailedChecks"].toInteger(),
            "History pooled counts differ");
    const auto points = expected["Points"].toArray();
    for (std::size_t i = 0; i < summary.indices.size(); ++i) {
        const auto& row = result.rows[static_cast<std::size_t>(summary.indices[i])];
        const auto point = points[static_cast<qsizetype>(i)].toObject();
        require(row.completed_ticks == ticks(point["CompletedAtUtc"]), "History order differs");
        same(row.mean_total_ms, point["MeanTotal"]);
    }
    return result;
}

QDateTime history_replay::date(std::int64_t ticks) {
    return QDateTime::fromMSecsSinceEpoch((ticks - 621355968000000000LL) / 10000, QTimeZone::UTC)
        .toLocalTime();
}
}
