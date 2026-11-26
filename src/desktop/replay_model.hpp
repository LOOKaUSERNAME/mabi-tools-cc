#pragma once
#include "cc_core.hpp"
#include "history_replay.hpp"
#include "scan_config.hpp"
#include <QJsonObject>
#include <QString>

namespace cc::ui {
#ifndef CC_APP_VERSION
#define CC_APP_VERSION "2026.11.26"
#endif
inline constexpr char application_version[] = CC_APP_VERSION;

// The adapter owns observations and completed report bytes. Live snapshots have
// no export bytes. Widgets hold a const reference to this stable model object;
// all model replacement happens on the GUI thread, never a networking worker.
struct replay_model {
    QByteArray original_json;
    QJsonObject document;
    std::vector<cc::channel> channels;
    cc::scan_summary summary;
    std::optional<history_replay> saved_history;
    QString state;
    int target_samples{40};
    static replay_model read(const QString& path, const QString& state);
    static replay_model ready_scan(const scan_config&);
    static replay_model ready_channels(const std::vector<cc::scanning::endpoint>&,
                                       cc::scanning::options);
    static replay_model completed_native(const cc::scanning::result&, cc::scanning::options);
    void apply_progress(const cc::scanning::snapshot&);
    void verify_reference() const;
    QString endpoint(std::size_t index) const;
    QString completed_time() const;
    QString text_report() const;
    QString suggested_label() const;
    QString suggested_note() const;
    bool suggests(std::size_t index) const;
    qsizetype exact_tie_count() const;
};

QString milliseconds(std::optional<double> value, int decimals = 2);
QString variation(std::optional<double> value);
QString compact(double value);
}
