// Link and validate production policy without constructing a transport or
// opening a connection. This host deliberately has no Qt dependency.
#include "cc_scan.hpp"
#include "comparison_catalog.hpp"
#include <iostream>
#include <stdexcept>

int main() {
    try {
        using namespace cc::scanning;
        if (comparison_catalog.empty())
            throw std::runtime_error("Missing catalogue");
        const auto approved = comparison_catalog.front();
        validate({approved}, {});

        // A syntactically valid local address must not inherit the fixture
        // scanner's permission merely because the desktop build is disabled.
        auto unlisted = approved;
        unlisted.host = "127.0.0.1";
        bool rejected = false;
        try {
            validate({unlisted}, {});
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        if (!rejected)
            throw std::runtime_error("Unlisted endpoint was accepted");

        std::cout << "PASS Qt-free production consumer: bundled entry accepted, "
                     "unlisted entry rejected; no connections opened.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
