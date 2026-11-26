#pragma once
#include "scan_config.hpp"
#include <QString>
#include <optional>
#include <algorithm>
#include <stdexcept>

namespace cc::ui {
// Normal startup uses LocalAppData; tests supply a disposable base directory.
// open_* creates the native profile directory. checked_* rejects redirected or
// non-local paths and invalid leaf names; an absent leaf is allowed for creation.
// Failures throw. These checks do not lock paths against later changes: storage
// operations still own their locking and atomic publication requirements.
QString open_profile_root();
QString open_profile_root_under(const QString& base);
QString checked_profile_root(const QString& root);
QString checked_profile_file(const QString& root, const QString& filename);

inline void validate_profile_channels(const std::vector<cc::scanning::endpoint>& targets,
                                      const std::optional<scan_config>& authorized) {
    const scan_config input{targets, authorized ? authorized->settings : cc::scanning::options{}};
    input.validate(); // Enforce the bundled or fixture endpoint policy for saved drafts too.
    if (!authorized)
        return; // Offline drafting never grants permission to scan.
    for (const auto& target : targets) {
        const auto& approved = authorized->targets;
        if (!std::any_of(approved.begin(), approved.end(), [&](const auto& endpoint) {
                return endpoint.host == target.host && endpoint.port == target.port;
            }))
            throw std::invalid_argument(
                "That address and port is not in the explicitly supplied scan configuration.");
    }
}
}
