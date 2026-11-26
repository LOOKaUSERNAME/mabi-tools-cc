// GUI-owned adapter from native observations and compatible reports to widget
// state. Live progress has no exportable report; completion serializes and checks
// the derived values. Presentation-only tie wording does not change native ranks.

#include "replay_model.hpp"
#include "cc_report.hpp"
#include "cc_endpoints.hpp"
#include "report_text.hpp"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <cmath>
#include <algorithm>
#include <array>
#include <stdexcept>

namespace cc::ui {
#ifdef CC_PROFILE_HISTORY
namespace {
QJsonArray exact_overall_ties(const replay_model& model) {
    const auto best = model.summary.picks.overall;
    if (best < 0)
        return {};
    const auto best_index = static_cast<std::size_t>(best);
    const auto& metrics = model.summary.channels;
    std::vector<int> numbers{model.channels[best_index].number};
    for (std::size_t i = 0; i < model.channels.size(); ++i) {
        if (i == best_index || metrics[i].success_rate != metrics[best_index].success_rate ||
            metrics[i].score_ms != metrics[best_index].score_ms)
            continue;
        // Reuse the legacy selector's half-scan eligibility and matching
        // rounds rather than promoting a merely equal but ineligible score.
        const std::array pair{model.channels[best_index], model.channels[i]};
        const std::array comparison{metrics[best_index], metrics[i]};
        if (cc::select(pair, comparison).overall >= 0)
            numbers.push_back(model.channels[i].number);
    }
    std::ranges::sort(numbers);
    QJsonArray result;
    if (numbers.size() > 1)
        for (const auto number : numbers)
            result.append(number);
    return result;
}
}
#endif
replay_model replay_model::ready_scan(const scan_config& config) {
    config.validate();
    return ready_channels(config.targets, config.settings);
}

replay_model replay_model::ready_channels(const std::vector<cc::scanning::endpoint>& targets,
                                          cc::scanning::options settings) {
    cc::catalog::validate(targets);
    replay_model model;
    model.state = "ready";
    model.target_samples = settings.samples;
    cc::scanning::snapshot empty;
    for (const auto& target : targets)
        empty.channels.push_back({target, {}});
    std::vector<cc::channel> channels;
    for (const auto& target : targets)
        channels.push_back({target.number, {}});
    empty.summary = cc::summarize(channels);
    model.document = {
        {"Scope", cc::scanning::report_text::Scope},
        {"Result",
         QJsonObject{{"Options", QJsonObject{{"Samples", settings.samples},
                                             {"TimeoutMs", settings.timeout_ms},
                                             {"IntervalMs", settings.interval_ms},
                                             {"MaxConcurrency", settings.max_concurrency}}}}}};
    model.apply_progress(empty);
    model.state = "ready";
    return model;
}

void replay_model::apply_progress(const cc::scanning::snapshot& snapshot) {
    original_json.clear();
    channels.clear();
    summary = snapshot.summary;
    QJsonArray endpoints;
    for (const auto& channel : snapshot.channels) {
        cc::channel observations{channel.target.number, {}};
        for (const auto& sample : channel.samples)
            observations.samples.push_back(
                {sample.round, sample.total, sample.connection, !sample.failure.empty()});
        channels.push_back(std::move(observations));
        endpoints.append(QJsonObject{
            {"Endpoint", QJsonObject{{"Channel", channel.target.number},
                                     {"Host", QString::fromStdString(channel.target.host)},
                                     {"Port", channel.target.port}}}});
    }
    auto result = document["Result"].toObject();
    result["Channels"] = endpoints;
    result.remove("Recommendations");
    result.remove("CompletedAtUtc");
    document["Result"] = result;
}

replay_model replay_model::completed_native(const cc::scanning::result& scan,
                                            cc::scanning::options settings) {
    replay_model model;
    model.target_samples = settings.samples;
    model.state = "completed";
    model.apply_progress(*scan.values);
#ifdef CC_PROFILE_HISTORY
    model.original_json = QByteArray::fromStdString(
        cc::scanning::report_json(scan, settings, {application_version, CC_SOURCE_COMMIT}));
#else
    model.original_json = QByteArray::fromStdString(cc::scanning::report_json(scan, settings));
#endif
    model.document = QJsonDocument::fromJson(model.original_json).object();
    model.verify_reference();
#ifdef CC_PROFILE_HISTORY
    const auto tied = exact_overall_ties(model);
    if (tied.size() > 1) {
        // Keep the legacy BestOverall field replayable. These optional fields
        // record the distinct suggestion actually displayed by this app.
        QJsonArray suggested;
        if (tied.size() == 2)
            suggested = tied;
        else
            suggested.append(tied.at(1));
        auto result = model.document["Result"].toObject();
        auto recommendations = result["Recommendations"].toObject();
        recommendations["SuggestedChannelNumbers"] = suggested;
        recommendations["ExactOverallTiedChannels"] = tied;
        result["Recommendations"] = recommendations;
        model.document["Result"] = result;
        // Extend the native recommendation object without re-encoding its
        // measurements or canonical property order.
        const auto fields = QJsonDocument(QJsonObject{{"SuggestedChannelNumbers", suggested},
                                                      {"ExactOverallTiedChannels", tied}})
                                .toJson(QJsonDocument::Compact);
        if (!model.original_json.endsWith("}}}"))
            throw std::runtime_error("Unexpected native report ending.");
        model.original_json.chop(3);
        model.original_json.append(',').append(fields.mid(1, fields.size() - 2)).append("}}}");
    }
#endif
    return model;
}

namespace {
std::optional<double> duration(const QJsonValue& value) {
    if (value.isNull() || value.isUndefined())
        return {};
    if (!value.isDouble() || !std::isfinite(value.toDouble()) || value.toDouble() < 0)
        throw std::invalid_argument("Invalid duration in replay report");
    return value.toDouble();
}

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void same(std::optional<double> actual, QJsonValue expected) {
    auto reference = duration(expected);
    require(actual.has_value() == reference.has_value(),
            "Native/reference timing presence differs");
    // Adapter arithmetic adds no operations: this permits only round-trip JSON noise.
    if (actual)
        require(std::abs(*actual - *reference) <= 1e-10 * std::max(1., std::abs(*reference)),
                "Native/reference timing differs");
}

QString status(cc::recommendation_status value) {
    switch (value) {
    case cc::recommendation_status::clear_lead:
        return "ClearLead";
    case cc::recommendation_status::best_measured:
        return "BestMeasured";
    case cc::recommendation_status::not_enough_data:
        return "NotEnoughData";
    default:
        return "NoClearWinner";
    }
}
}

replay_model replay_model::read(const QString& path, const QString& state) {
    require(QStringList{"ready", "completed", "clear", "tie", "insufficient", "active", "cancelled"}
                .contains(state),
            "Unknown replay state");
    QFile file(path);
    require(file.open(QIODevice::ReadOnly) && file.size() <= 4 * 1024 * 1024,
            "Cannot read bounded replay fixture");
    replay_model model;
    model.state = state;
    model.original_json = file.readAll();
    QJsonParseError error;
    model.document = QJsonDocument::fromJson(model.original_json, &error).object();
    require(error.error == QJsonParseError::NoError && model.document["SchemaVersion"].toInt() == 7,
            "Expected a schema-7 replay fixture");
    auto result = model.document["Result"].toObject();
    model.target_samples = result["Options"].toObject()["Samples"].toInt();
    require(model.target_samples >= 10 && model.target_samples <= 120,
            "Invalid target sample count");
    const auto channels = result["Channels"].toArray();
    require(!channels.empty() && channels.size() <= 30, "Invalid channel count");
    for (const auto& value : channels) {
        auto source = value.toObject();
        cc::channel channel{source["Endpoint"].toObject()["Channel"].toInt(), {}};
        const auto samples = source["Samples"].toArray();
        require(samples.size() <= 120, "Too many observations");
        for (const auto& sample_value : samples) {
            auto sample = sample_value.toObject();
            channel.samples.push_back({sample["Round"].toInt(), duration(sample["LatencyMs"]),
                                       duration(sample["ConnectMs"]),
                                       !sample["Failure"].toString().trimmed().isEmpty()});
        }
        model.channels.push_back(std::move(channel));
    }
    model.summary = cc::summarize(model.channels);
    model.verify_reference();
    return model;
}

void replay_model::verify_reference() const {
    auto result = document["Result"].toObject();
    auto expected = result["Channels"].toArray();
    for (std::size_t i = 0; i < summary.channels.size(); ++i) {
        const auto& actual = summary.channels[i];
        auto ref = expected[static_cast<qsizetype>(i)].toObject();
        require(actual.attempted == ref["Attempted"].toInt() &&
                    actual.successful == ref["Successful"].toInt() &&
                    actual.failed == ref["Failed"].toInt(),
                "Native/reference counts differ");
        require(QString::fromUtf8(cc::status_text(actual.status)) == ref["Status"].toString(),
                "Native/reference channel status differs");
        require(actual.eligible() == ref["IsEligible"].toBool(),
                "Native/reference eligibility differs");
        same(actual.success_rate, ref["SuccessRate"]);
        same(actual.average_connection_ms, ref["AverageMs"]);
        same(actual.minimum_connection_ms, ref["MinimumMs"]);
        same(actual.maximum_connection_ms, ref["MaximumMs"]);
        same(actual.p95_connection_ms, ref["P95Ms"]);
        same(actual.connection_jitter_ms, ref["JitterMs"]);
        same(actual.average_total_ms, ref["AverageResponseMs"]);
        same(actual.average_reply_ms, ref["AverageReplyWaitMs"]);
        same(actual.total_jitter_ms, ref["ResponseJitterMs"]);
        same(actual.score_ms, ref["ScoreMs"]);
    }
    auto recommendations = result["Recommendations"].toObject();
    auto check = [&](int index, cc::recommendation_status outcome, const char* key,
                     const char* status_key) {
        const auto picked = recommendations[key];
        require(index < 0 ? picked.isNull()
                          : channels[static_cast<std::size_t>(index)].number ==
                                picked.toObject()["Endpoint"].toObject()["Channel"].toInt(),
                "Native/reference channel pick differs");
        require(status(outcome) == recommendations[status_key].toString(),
                "Native/reference recommendation status differs");
    };
    const auto& picks = summary.picks;
    check(picks.fastest, picks.fastest_status, "LowestLatency", "LowestLatencyStatus");
    check(picks.consistent, picks.consistent_status, "MostStable", "MostStableStatus");
    check(picks.overall, picks.overall_status, "BestOverall", "BestOverallStatus");
    require(picks.overall_results_similar == recommendations["OverallResultsSimilar"].toBool(),
            "Native/reference similarity differs");
}

QString replay_model::endpoint(std::size_t index) const {
    auto endpoint = document["Result"]
                        .toObject()["Channels"]
                        .toArray()[static_cast<qsizetype>(index)]
                        .toObject()["Endpoint"]
                        .toObject();
    // A scope identifier can contain %1 or %2. Never feed inserted address text
    // into a later QString::arg call, which would treat it as a placeholder.
    return QString::number(endpoint["Channel"].toInt()) + ',' + endpoint["Host"].toString() + ',' +
           QString::number(endpoint["Port"].toInt());
}

QString replay_model::completed_time() const {
    return document["Result"].toObject()["CompletedAtUtc"].toString().mid(11, 8);
}

QString replay_model::suggested_label() const {
    const auto items = document["Result"]
                           .toObject()["Recommendations"]
                           .toObject()["SuggestedChannelNumbers"]
                           .toArray();
    if (items.size() == 2)
        return QString("Ch. %1 or %2").arg(items.at(0).toInt()).arg(items.at(1).toInt());
    if (items.size() == 1)
        return QString("Ch. %1").arg(items.at(0).toInt());
    return summary.picks.overall < 0
               ? QString("Not enough data")
               : QString("Ch. %1").arg(
                     channels[static_cast<std::size_t>(summary.picks.overall)].number);
}

qsizetype replay_model::exact_tie_count() const {
    return document["Result"]
        .toObject()["Recommendations"]
        .toObject()["ExactOverallTiedChannels"]
        .toArray()
        .size();
}

QString replay_model::suggested_note() const {
    const auto tied = exact_tie_count();
    if (tied > 1)
        return QString("%1 channels tied").arg(tied);
    return summary.picks.overall_results_similar ? QString("Several channels look similar")
                                                 : QString("Based on this connection check");
}

bool replay_model::suggests(std::size_t index) const {
    const auto items = document["Result"]
                           .toObject()["Recommendations"]
                           .toObject()["SuggestedChannelNumbers"]
                           .toArray();
    if (items.isEmpty())
        return summary.picks.overall >= 0 &&
               index == static_cast<std::size_t>(summary.picks.overall);
    for (const auto& number : items)
        if (channels[index].number == number.toInt())
            return true;
    return false;
}

QString replay_model::text_report() const {
    QStringList lines;
    const auto& picks = summary.picks;
    auto recommendation = [&](int index, bool stable) {
        if (index < 0)
            return QString("Not enough data");
        const auto i = static_cast<std::size_t>(index);
        return QString("Ch. %1   (%2 ms %3)")
            .arg(channels[i].number)
            .arg(milliseconds(stable ? summary.channels[i].total_jitter_ms
                                     : summary.channels[i].average_connection_ms),
                 stable ? "Jitter" : "Connection time");
    };
    QString overall = suggested_label();
    if (picks.overall >= 0 && exact_tie_count() < 2) {
        auto i = static_cast<std::size_t>(picks.overall);
        overall = QString("Ch. %1   (Connection time %2 ms / Reply wait %3 ms)")
                      .arg(channels[i].number)
                      .arg(milliseconds(summary.channels[i].average_connection_ms),
                           milliseconds(summary.channels[i].average_reply_ms));
    }
    const auto commit = document["BuildCommit"].toString("unknown");
    const auto completed =
        document["Result"].toObject()["CompletedAtUtc"].toString().left(19).replace('T', ' ');
    lines << "MABITOOLS · CHANNEL CHECKER" << "Build commit: " + commit
          << "Completed: " + completed + " UTC" << QString(76, '=') << "RESULTS"
          << "FASTEST CONNECTION: " + recommendation(picks.fastest, false)
          << "MOST CONSISTENT   : " + recommendation(picks.consistent, true)
          << "SUGGESTED CHANNEL : " + overall;
    if (picks.overall >= 0)
        lines << suggested_note();
    lines << QString(76, '=') << document["Scope"].toString()
          << "Close results still get a practical pick; small differences may not matter in play."
          << "";
    for (std::size_t i = 0; i < channels.size(); ++i) {
        const auto& s = summary.channels[i];
        const auto address = document["Result"]
                                 .toObject()["Channels"]
                                 .toArray()[static_cast<qsizetype>(i)]
                                 .toObject()["Endpoint"]
                                 .toObject();
        const auto measurements =
            QString(
                "  Connection time %1 ms / Reply wait %2 ms  Total time %3 ms  Jitter %4 ms  Connection variation %5 ms  Connection P95 %6 ms  %7/%8 successful checks  %9")
                .arg(milliseconds(s.average_connection_ms), milliseconds(s.average_reply_ms),
                     milliseconds(s.average_total_ms), milliseconds(s.total_jitter_ms),
                     milliseconds(s.connection_jitter_ms), milliseconds(s.p95_connection_ms),
                     QString::number(s.successful), QString::number(s.attempted),
                     QString::fromUtf8(cc::status_text(s.status)));
        // Keep literal scope identifiers out of subsequent placeholder expansion.
        lines << QString("Ch. %1  ").arg(channels[i].number, 2) + address["Host"].toString() + ':' +
                     QString::number(address["Port"].toInt()) + measurements;
    }
    lines
        << ""
        << "Failed checks can reflect blocked traffic or outdated addresses; they do not prove a channel is offline."
        << "Bundled channel list: " + document["PresetSource"].toString() + " · revision " +
               document["PresetRevision"].toString()
        << "";
    return lines.join("\r\n");
}

QString milliseconds(std::optional<double> value, int decimals) {
    return value ? QString::number(*value, 'f', decimals) : QString::fromUtf8("—");
}

QString variation(std::optional<double> value) {
    return !value                      ? QString::fromUtf8("—")
           : *value > 0 && *value < .1 ? "<0.1 ms"
                                       : milliseconds(value, 1) + " ms";
}

QString compact(double value) {
    auto text = QString::number(value, 'f', 2);
    while (text.endsWith('0'))
        text.chop(1);
    if (text.endsWith('.'))
        text.chop(1);
    return text;
}
}
