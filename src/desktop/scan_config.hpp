#pragma once
#include "cc_scan.hpp"
#include <QString>

namespace cc::ui {
// UI adapter for native options. validate enforces scanner policy plus the
// dropdown's supported sample counts. read_fixture reads bounded JSON and
// rejects invalid explicit values rather than silently replacing them.
struct scan_config {
    std::vector<cc::scanning::endpoint> targets;
    cc::scanning::options settings;
    static scan_config read_fixture(const QString& path);
    void validate() const;
};
}
