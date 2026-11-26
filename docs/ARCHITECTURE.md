# Channel Checker architecture

This tree contains the Windows x64 C++20 and Qt Widgets application. The
portable ZIP and installer both contain `MabiTools.ChannelChecker.exe`,
built by the CMake target `cc_app`. Its settings and history use the
application profile; explicit WPF imports use a separate read-only archive.
The bundled icons and Erinn channel list are in `assets/`.

## Where to look

Paths below are relative to the repository root. Private headers live beside
implementation files; the public measurement headers keep their existing names.

| Path | Responsibility |
|---|---|
| `src/app/` | Entry point, launch modes, application wiring and the Windows executable resource. |
| `src/desktop/` | Qt Widgets windows, layout, dialogs, cards, styling and view models. `window_services.cpp` binds controls to service state. |
| `src/desktop/replay_model.*`, `src/desktop/scan_config.*` | Adapts observations and options for the UI: display text, report data, dropdown limits and fixture input. |
| `src/measurement/analysis/` | Portable statistics, jitter, recommendations and history comparisons. Public headers are `cc_core.hpp` and `cc_history.hpp`. |
| `src/measurement/scan/` | Scan API, endpoint parsing and rules, bounded probe rounds, Winsock transport and report encoding. The catalogue header is generated from `assets/erinn-na.csv` during configuration and exposed by `cc_scan_production`. |
| `src/profile/` | Profile paths, saved channel selections, history storage, retention and explicit WPF import. `history_replay` decodes saved reports and prepares comparisons; the desktop renders the data. WPF import keeps its read-only archive viewer alongside the importer. |
| `src/services/` | Maintenance notices, notice cache, update policy, download verification and HTTPS transport. |
| `tests/analysis/` | Portable numerical invariants. |
| `tests/desktop/` | Window, layout, history and service-binding checks. |
| `tests/services/` | Update and HTTPS checks, shared local HTTP server and JSON corpus under `fixtures/`. Fixture executables are excluded from player packages. |
| `examples/scan-consumer/` | Independent Windows host using the Qt-free production scanner. |
| `assets/`, `installer/` | Shared artwork, bundled endpoints, help content and Windows packaging. |

## Contracts worth preserving

- The UI thread owns the model. A worker reports immutable snapshots, which
  Qt queues for the window. Scan generations reject late updates. Closing
  the app waits for the worker and queued history work.
- The progress number is measured, not interpolated. Display effects share
  a timer and honour the Windows animation preference. An animation does
  not change any observation or result.
- Retention preview is read-only. Export keeps the selected originals in a
  separate file. Apply recalculates the selection under the profile lock
  and refuses to use an out-of-date confirmation. Retention never rewrites
  the WPF archive. If removals fail partway through, the store reports the
  bytes actually left.
- Native updates use their own feed identity and endpoint. Metadata and installer
  downloads share the service coordinator's scan drain barrier. Closing cancels
  and drains work without resuming it. Isolated and imported profiles never
  contact a feed. See [update policy](UPDATES.md).

## Build and test targets

Configure the repository's top-level `CMakeLists.txt` with Windows x64
MSVC and Qt 6.10.2. Target `cc_app` produces `MabiTools.ChannelChecker.exe`.
With `BUILD_TESTING` enabled, `cc_window_tests` produces the standalone
window test executable, `cc_update_tests` exercises policy and streamed HTTP
downloads, and `cc_native_tests` builds the core tests.
The app links `cc_scan_production`, the Qt-free Windows static scanner, and
`cc_domain`, the portable statistics/recommendations library. The native build
generates the production catalogue from `assets/erinn-na.csv`; the Qt resource
embeds that same file. The fixture executable instead links `cc_endpoints` and
`cc_scan` with loopback-only policy. No executable links both scanner variants.
`CC_BUILD_QT=OFF` exposes the production library on Windows x64 MSVC without
finding Qt, and preserves domain-only builds elsewhere. See the
[consumer example](../BUILDING.md#native-c-consumer) for the host contract.

The packaging runner handles the required Qt DLLs, corresponding source,
notices and checksums. Use the runner, rather than an IDE-only build, when
preparing binaries for distribution.
