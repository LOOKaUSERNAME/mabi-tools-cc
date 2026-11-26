#include "cc_core.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>

void require(bool condition) {
    if (!condition)
        throw std::runtime_error("Native invariant failed.");
}

int main() {
    try {
        cc::channel a{2, {}}, b{1, {}};
        // Eleven rounds split 5/6. Exact ties choose the lower channel number;
        // the failure below makes the early half ineligible despite a good mean.
        for (int round = 1; round <= 11; ++round) {
            a.samples.push_back({round, 20, 10, false});
            b.samples.push_back({round, 20, 10, false});
        }
        const auto tie = cc::summarize(std::vector{a, b});
        require(tie.picks.overall == 1 &&
                tie.picks.overall_status == cc::recommendation_status::best_measured);
        require(tie.picks.overall_results_similar);
        // A failed reply retains its connection time but contributes neither to
        // successful timing means nor to jitter pairs on either side of the gap.
        a.samples[2] = {3, std::nullopt, 99, true};
        const auto gap = cc::calculate(a.samples);
        require(gap.successful == 10 && gap.adjacent_pairs == 8 &&
                *gap.average_connection_ms == 10);
        require(cc::summarize(std::vector{a, b}).picks.overall ==
                -1); // Failed five-sample early half.
        require(!cc::calculate({}).average_total_ms);
        require(*cc::calculate(std::vector<cc::observation>{{1, 0, 0, false}}).average_total_ms ==
                0);
        bool rejected = false;
        try {
            (void)cc::calculate(std::vector<cc::observation>{{1, 2, 3, false}});
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected);
        std::cout
            << "PASS native null/zero, jitter gaps, odd halves, tie order and rejection checks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
