// Parse channel rows and compare address identities without DNS or connections.
// Numeric-address and whitespace handling retain the legacy catalogue contract;
// original host spelling is kept for persistence. Scan permission is checked
// separately by the scanner, not granted by successful parsing.

#include <winsock2.h>
#include <ws2tcpip.h>
#include "cc_endpoints.hpp"
#include <array>
#include <algorithm>
#include <charconv>
#include <limits>
#include <set>
#include <tuple>

namespace cc::catalog {
namespace {
using text = std::u32string;
using view = std::u32string_view;

[[noreturn]] void argument(std::string message) {
    throw error("ArgumentException", std::move(message) + " (Parameter 'endpoints')");
}

[[noreturn]] void invalid_endpoint() {
    argument("Each channel needs a number from 1–99, an IP address and a port from 1–65535.");
}

// Decode to Unicode scalars before applying the catalogue whitespace rules.
// Reject incomplete sequences, overlong encodings and surrogate values so a
// malformed byte sequence cannot acquire a different address identity.
text decode(std::string_view bytes) {
    text result;
    for (std::size_t i = 0; i < bytes.size();) {
        const auto first = static_cast<unsigned char>(bytes[i++]);
        char32_t value{};
        int extra{};
        char32_t minimum{};
        if (first < 0x80)
            value = first;
        else if (first >= 0xc2 && first <= 0xdf) {
            value = first & 0x1f;
            extra = 1;
            minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            value = first & 0x0f;
            extra = 2;
            minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            value = first & 7;
            extra = 3;
            minimum = 0x10000;
        } else
            invalid_endpoint();
        while (extra-- > 0) {
            if (i == bytes.size())
                invalid_endpoint();
            const auto next = static_cast<unsigned char>(bytes[i++]);
            if ((next & 0xc0) != 0x80)
                invalid_endpoint();
            value = (value << 6) | (next & 0x3f);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
            invalid_endpoint();
        result.push_back(value);
    }
    return result;
}

std::string encode(view value) {
    std::string result;
    for (char32_t code : value) {
        if (code < 0x80)
            result.push_back(static_cast<char>(code));
        else if (code < 0x800) {
            result.push_back(static_cast<char>(0xc0 | (code >> 6)));
            result.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        } else if (code < 0x10000) {
            result.push_back(static_cast<char>(0xe0 | (code >> 12)));
            result.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
            result.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        } else {
            result.push_back(static_cast<char>(0xf0 | (code >> 18)));
            result.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3f)));
            result.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
            result.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        }
    }
    return result;
}

bool whitespace(char32_t value) {
    // Char.IsWhiteSpace / string.Trim use Unicode whitespace, not the current
    // Windows locale. BOM is deliberately absent; only leading BOMs are removed.
    return (value >= 9 && value <= 13) || value == 0x20 || value == 0x85 || value == 0xa0 ||
           value == 0x1680 || (value >= 0x2000 && value <= 0x200a) || value == 0x2028 ||
           value == 0x2029 || value == 0x202f || value == 0x205f || value == 0x3000;
}

view trim(view value) {
    while (!value.empty() && whitespace(value.front()))
        value.remove_prefix(1);
    while (!value.empty() && whitespace(value.back()))
        value.remove_suffix(1);
    return value;
}

bool decimal(view value, std::uint32_t maximum, std::uint32_t& result) {
    // The .NET integer parser permits trailing NULs with NumberStyles.None.
    // Scope identity uses that same conversion, without field whitespace trim.
    while (!value.empty() && value.back() == 0)
        value.remove_suffix(1);
    if (value.empty())
        return false;
    result = 0;
    for (char32_t code : value) {
        if (code < U'0' || code > U'9')
            return false;
        const auto digit = static_cast<std::uint32_t>(code - U'0');
        if (result > (maximum - digit) / 10)
            return false;
        result = result * 10 + digit;
    }
    return true;
}

bool ipv4(view value, std::array<unsigned char, 16>& bytes) {
    // IPAddress.TryParse retains inet_aton-style numeric forms. The final
    // component fills the remaining octets; preceding components each use one.
    // Identity uses bytes, never the original spelling used for display/storage.
    std::array<std::uint32_t, 4> parts{};
    std::size_t count = 0, start = 0;
    for (;;) {
        const auto dot = value.find(U'.', start);
        auto part = value.substr(start, dot == view::npos ? view::npos : dot - start);
        if (count == parts.size() || part.empty())
            return false;
        unsigned base = 10;
        if (part.size() > 1 && part.front() == U'0') {
            part.remove_prefix(1);
            base = 8;
            if (part.front() == U'x' || part.front() == U'X') {
                base = 16;
                part.remove_prefix(1);
                if (part.empty())
                    return false;
            }
        }
        std::uint64_t number = 0;
        for (char32_t code : part) {
            unsigned digit =
                code >= U'0' && code <= U'9'   ? static_cast<unsigned>(code - U'0')
                : code >= U'a' && code <= U'f' ? static_cast<unsigned>(code - U'a' + 10)
                : code >= U'A' && code <= U'F' ? static_cast<unsigned>(code - U'A' + 10)
                                               : 255;
            if (digit >= base)
                return false;
            number = number * base + digit;
            if (number > 0xffffffffULL)
                return false;
        }
        parts[count++] = static_cast<std::uint32_t>(number);
        if (dot == view::npos)
            break;
        start = dot + 1;
    }
    std::uint32_t address = 0;
    for (std::size_t i = 0; i + 1 < count; ++i) {
        if (parts[i] > 255)
            return false;
        address |= parts[i] << (24 - 8 * i);
    }
    const std::uint64_t last_limit = std::uint64_t{1} << (8 * (5 - count));
    if (parts[count - 1] >= last_limit)
        return false;
    address |= parts[count - 1];
    for (int i = 0; i < 4; ++i)
        bytes[static_cast<std::size_t>(i)] = static_cast<unsigned char>(address >> (24 - 8 * i));
    return true;
}

struct identity {
    int family{};
    std::array<unsigned char, 16> bytes{};
    std::uint32_t scope{};
    auto operator<=>(const identity&) const = default;
};

bool address_identity(const std::string& host, identity& result) {
    auto decoded = decode(host);
    view address = decoded;
    if (address.empty())
        return false;
    if (address.find(U':') == view::npos) {
        result.family = 4;
        return ipv4(address, result.bytes);
    }
    result.family = 6;
    if (address.front() == U'[') {
        const auto close = address.find(U']');
        if (close == view::npos)
            return false;
        const auto suffix = address.substr(close + 1);
        if (!suffix.empty()) {
            if (suffix.front() != U':')
                return false;
            auto port = suffix.substr(1);
            bool hexadecimal = false;
            if (port.size() >= 2 && port[0] == U'0' && port[1] == U'x') {
                hexadecimal = true;
                port.remove_prefix(2);
            }
            // IPAddress's bracket suffix is syntax only, not the endpoint Port.
            // It does not impose a numeric range or contribute to IP identity.
            for (char32_t code : port)
                if (!(code >= U'0' && code <= U'9') &&
                    !(hexadecimal &&
                      ((code >= U'a' && code <= U'f') || (code >= U'A' && code <= U'F'))))
                    return false;
        }
        address = address.substr(1, close - 1);
    }
    if (auto percent = address.find(U'%'); percent != view::npos) {
        const auto scope = address.substr(percent + 1);
        if (scope.find_first_of(U"/]") != view::npos)
            return false;
        if (!decimal(scope, 0xffffffffU, result.scope)) {
            // Windows .NET accepts textual/overflowing zone identifiers but
            // retains only an unsigned decimal scope. Others have scope zero.
            result.scope = 0;
        }
        address = address.substr(0, percent);
    }
    for (char32_t code : address)
        if (code == 0 || code > 127)
            return false;
    auto literal = encode(address);
    if (address.find(U'.') != view::npos) {
        if (address.find_last_of(U':') > address.find(U'.'))
            return false;
        // InetPton on Windows accepts leading zeroes in some middle dotted
        // components, while .NET's IPv6 parser rejects them before each dot.
        // Its final octet does accept a leading zero; preserve that asymmetry.
        // This also differs from standalone IPv4, which accepts octal spellings.
        auto tail = address.substr(address.find_last_of(U':') + 1);
        for (;;) {
            const auto dot = tail.find(U'.');
            const auto octet = tail.substr(0, dot);
            if (dot == view::npos) {
                if (octet.size() > 1 && octet[0] == U'0' && octet[1] == U'0')
                    return false;
                std::uint32_t final_octet{};
                if (!decimal(octet, 255, final_octet))
                    return false;
                // .NET also permits four characters such as .0255. InetPton
                // bounds the spelling more strictly, so decode this last octet
                // as decimal before passing a private canonical copy to it.
                literal = encode(address.substr(0, address.find_last_of(U'.') + 1)) +
                          std::to_string(final_octet);
                break;
            }
            if (octet.size() > 1 && octet.front() == U'0')
                return false;
            tail.remove_prefix(dot + 1);
        }
    }
    IN6_ADDR bytes{};
    if (InetPtonA(AF_INET6, literal.c_str(), &bytes) != 1)
        return false;
    std::copy(std::begin(bytes.u.Byte), std::end(bytes.u.Byte), result.bytes.begin());
    return true;
}
}

void validate(const std::vector<scanning::endpoint>& endpoints) {
    if (endpoints.empty() || endpoints.size() > 30)
        argument("Choose between 1 and 30 channels.");
    std::set<int> numbers;
    std::set<std::pair<identity, int>> addresses;
    for (const auto& endpoint : endpoints) {
        identity address;
        if (endpoint.number < 1 || endpoint.number > 99 || endpoint.port < 1 ||
            endpoint.port > 65535 || !address_identity(endpoint.host, address))
            invalid_endpoint();
        if (!numbers.insert(endpoint.number).second)
            argument("Channel " + std::to_string(endpoint.number) + " appears more than once.");
        if (!addresses.emplace(address, endpoint.port).second)
            argument("Channels must have distinct IP address and port pairs.");
    }
}

std::vector<scanning::endpoint> parse(std::string_view utf8_text) {
    const auto decoded = decode(utf8_text);
    view remaining = decoded;
    while (!remaining.empty() && remaining.front() == 0xfeff)
        remaining.remove_prefix(1);
    std::vector<scanning::endpoint> endpoints;
    int line_number = 0;
    while (!remaining.empty()) {
        ++line_number;
        const auto newline = remaining.find_first_of(U"\r\n");
        auto line = trim(remaining.substr(0, newline));
        if (newline == view::npos)
            remaining = {};
        else {
            const bool crlf = remaining[newline] == U'\r' && newline + 1 < remaining.size() &&
                              remaining[newline + 1] == U'\n';
            remaining.remove_prefix(newline + (crlf ? 2 : 1));
        }
        if (line.empty() || line.front() == U'#')
            continue;
        const auto comma = line.find(U',');
        const auto second = comma == view::npos ? view::npos : line.find(U',', comma + 1);
        std::uint32_t channel{}, port{};
        const auto numeric_field = [](view field, std::uint32_t& value) {
            field = trim(field);
            return decimal(field, 0x7fffffff, value);
        };
        if (comma == view::npos || second == view::npos ||
            line.find(U',', second + 1) != view::npos ||
            !numeric_field(line.substr(0, comma), channel) ||
            !numeric_field(line.substr(second + 1), port))
            throw error("FormatException",
                        "Line " + std::to_string(line_number) +
                            ": use channel,IP,port with numeric channel and port values.");
        endpoints.push_back({static_cast<int>(channel),
                             encode(trim(line.substr(comma + 1, second - comma - 1))),
                             static_cast<int>(port)});
    }
    validate(endpoints);
    return endpoints;
}

std::string format(const std::vector<scanning::endpoint>& endpoints) {
    validate(endpoints);
    std::string result;
    for (const auto& endpoint : endpoints) {
        if (!result.empty())
            result += "\r\n";
        result += std::to_string(endpoint.number) + ',' + endpoint.host + ',' +
                  std::to_string(endpoint.port);
    }
    return result;
}
}
