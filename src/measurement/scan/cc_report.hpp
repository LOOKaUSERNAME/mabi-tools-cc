#pragma once
#include "cc_scan.hpp"

namespace cc::scanning {
// Omitting metadata uses the compatibility default version 2026.11.23 with no
// source commit; it does not discover the caller's build identity. The desktop
// build supplies its own metadata. Library consumers must do the same to avoid
// reporting this fallback as the provenance of their own build.
struct report_metadata {
    std::string version = "2026.11.23";
    std::optional<std::string> build_commit;
};

// Fresh schema-7 JSON from a completed native scan. Preserves C# property/array
// order and full double precision. Whitespace/escaping may differ from .NET;
// values, nulls, units, calculated fields and recommendation identities may not.
// Partial scans are rejected. Pass the same options used by scan; validation
// requires complete ordered rounds and recomputes summaries from observations.
// Invalid results/options throw invalid_argument. No filesystem or Qt dependency
// is involved. Callers must supply their own build metadata for provenance.
std::string report_json(const result&, options, const report_metadata& = {});
std::string utc_text(std::chrono::system_clock::time_point);
}
