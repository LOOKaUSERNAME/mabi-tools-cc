#pragma once
#include "cc_endpoints.hpp"
#include <QString>

namespace cc::ui {
// Bounded channel-list persistence; parse success does not grant scan permission.
// Callers must validate the loaded/drafted list against their scan configuration.
// load_profile returns empty for an absent file and throws for invalid content,
// leaving the original bytes untouched. Saves replace atomically or throw.
// Fixture load instead returns defaults plus a warning on unusable input.
struct channel_settings {
    static QString fixture_root(const QString& path);
    static std::vector<cc::scanning::endpoint> bundled();
    static std::vector<cc::scanning::endpoint> load(const QString& fixture, QString& warning);
    static std::vector<cc::scanning::endpoint> load_profile(const QString& root);
    static void save_profile(const QString& root,
                             const std::vector<cc::scanning::endpoint>& endpoints);
    static void save(const QString& fixture, const std::vector<cc::scanning::endpoint>& endpoints);
};
}
