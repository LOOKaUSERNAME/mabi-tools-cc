#pragma once
#include "cc_scan.hpp"
#include <string_view>

namespace cc::catalog {
// Parsing and duplicate identity are separate from connection permission. These
// functions never connect, resolve a hostname, or authorize a scan.
// Input is well-formed UTF-8 (as produced by QString::toUtf8); malformed encoding
// is rejected. Host strings retain the user's spelling; parse trims only C#
// field whitespace. Address equality is private and never replaces stored text.
class error : public std::invalid_argument {
public:
    error(const char* kind, std::string message)
        : std::invalid_argument(std::move(message)), kind_(kind) {}

    const char* kind() const noexcept { return kind_; }

private:
    const char* kind_;
};

// parse/validate throw catalog::error on invalid fields or duplicate identities.
// Passing syntax validation does not imply passing scanning::validate policy.
std::vector<scanning::endpoint> parse(std::string_view utf8_text);
void validate(const std::vector<scanning::endpoint>& endpoints);
// Windows ChannelCatalog.Format: original host, invariant integer spelling,
// CRLF between records and no trailing newline. Input order is unchanged.
std::string format(const std::vector<scanning::endpoint>& endpoints);
}
