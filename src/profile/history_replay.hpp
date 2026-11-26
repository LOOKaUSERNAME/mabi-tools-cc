#pragma once
#include "cc_history.hpp"
#include <QString>
#include <QDateTime>

namespace cc::ui {
struct history_replay {
    std::vector<cc::history_row> rows;
    cc::history_summary summary;
    QString endpoint_csv;
    static history_replay read(const QString& fixture);
    static QDateTime date(std::int64_t ticks);
};
}
