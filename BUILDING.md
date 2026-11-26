# Build

Use Windows x64, MSVC, CMake, Qt 6.10.2 and Inno Setup 6. Build from a clean Git checkout at the annotated release tag. `build-release.ps1` verifies the tag, assembles dependencies and corresponding Qt source, runs the core and window fixtures, and creates the installer and portable ZIP.

```powershell
./build-release.ps1 -QtRoot C:\path\to\qt -QtSourceArchive C:\path\to\qtbase-everywhere-src-6.10.2.tar.xz -RuntimeRedist C:\path\to\VC_redist.x64.exe -OutputRoot C:\path\to\new-output
```

CMake target `cc_app` produces `MabiTools.ChannelChecker.exe`; `cc_window_tests` and `cc_update_tests` are separate regression executables excluded from packages. The original `assets/Assets/channel-checker.ico` is embedded through `src/app/app.rc.in` and used directly by Inno Setup.

Pass `-Version <calendar-version>` to build another annotated `v<calendar-version>` tag. The application always uses its fixed native HTTPS update feed. Loopback transports are compiled only into separate fixture executables and cannot be selected by the application or packaging runner. See [update policy](docs/UPDATES.md).

`cc_https_tests --configuration-check` verifies the production transport has no fixture-port constructor. Explicitly run `cc_https_tests --live-native-feed <result.json>` to inspect the real native HTTPS endpoints without downloading or installing anything. This opt-in check is separate from ordinary offline regressions and is excluded from the package.

The package opens without arguments. Offline self-checks use disposable profiles. `--isolated-profile` opens the application with a temporary profile for manual checks. The fixture executable supplies loopback endpoints; the player executable restricts scans to its bundled catalogue. Clean Windows guest testing of the final installer and ZIP is required separately.

For actual Windows cursor checks, run `cc_window_tests.exe tests/services/fixtures/service-corpus.json <existing-output-folder> --native-cursors` in an isolated Windows desktop. This opt-in test moves the system pointer and checks cursor handles through control, dialog and scan transitions. Repeat with `QT_SCALE_FACTOR=1.5` to cover higher display scaling; the normal fixture run does not require control of the pointer.

For core-only builds use `cmake -S . -B build-core -DCC_BUILD_QT=OFF`, build, then `ctest --test-dir build-core --output-on-failure`.

The redistributable must match `installer/runtime.json`. Setup embeds it, checks the installed x64 runtime, and requests elevation only when the runtime needs installation. It handles installation failure and restart-required results before completing. The runtime is shared and is not removed when Channel Checker is uninstalled.

## Native C++ consumer

`cc_domain` is the portable C++20 statistics/recommendations library. Windows
x64 MSVC builds also expose `cc_scan_production`, a static library containing
scan coordination, TCP transport, endpoint parsing and compatible JSON reports.
With `CC_BUILD_QT=OFF`, neither Qt nor the desktop application is configured.
Other platforms retain the domain-only build.

A host CMake project can use:

```cmake
set(CC_BUILD_QT OFF CACHE BOOL "Build the Windows Qt application")
set(BUILD_TESTING OFF CACHE BOOL "Build repository tests")
add_subdirectory(path/to/channel-checker channel-checker)
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE cc_scan_production)
```

The target carries its C++20 requirement, public include paths and Winsock link
dependency. Include `cc_scan.hpp` for typed options, progress and results,
`comparison_catalog.hpp` for `cc::scanning::comparison_catalog`, and
`cc_report.hpp` for report serialisation. The catalogue is generated from
`assets/erinn-na.csv`; validation permits only exact bundled entries (including
subsets and reordered lists). Parsing an address does not authorise a connection.
Do not link the loopback-only fixture targets `cc_scan`/`cc_endpoints` into a
production consumer, or link both scanner variants into one executable.

Call `cc::scanning::scan` on a host-owned worker. Keep a `tcp_transport` alive
through that call and adapt its `probe` method to the supplied `probe_function`.
The coordinator calls probes concurrently and joins each round before invoking
the observer on the calling worker. Progress owns an immutable snapshot that
can be queued to the host UI. Request cancellation through a `std::stop_source`;
`scan` throws `cc::scanning::cancelled` after workers drain, without returning a
partial result. Earlier progress snapshots remain valid. Successful return owns
the complete result; pass the same options and your own build metadata to
`report_json`. The host owns scheduling, UI handoff, settings and history; the
library includes no updater, profile storage or installer.

The independent consumer smoke check validates catalogue policy without opening
connections or constructing a transport:

```powershell
cmake -S examples/scan-consumer -B build-consumer -A x64 -DCC_BUILD_QT=OFF
cmake --build build-consumer --config Release --target scan_consumer
./build-consumer/Release/scan_consumer.exe
```

## Installation and data

Settings and history use `%LOCALAPPDATA%\MabiTools-ChannelChecker`. The installer uses `%LOCALAPPDATA%\Programs\MabiTools\Channel Checker`. The legacy WPF installation remains separate. Uninstall preserves saved data. Setup writes an install-kind marker; the updater also requires the matching per-user uninstall registration and exact install path before offering installation.

Optional legacy import is explicit: `MabiTools.ChannelChecker.exe --import-legacy <snapshot-directory>`, then `--legacy-profile` opens the separate imported profile under `%LOCALAPPDATA%\MabiTools-CC-Legacy-Import`. Import accepts only a sealed legacy snapshot with its verification record; see [legacy snapshot format](docs/LEGACY_IMPORT.md). Original WPF reports remain a read-only archive, excluded from current recommendations. Existing destinations are never replaced. Normal startup never imports data.

Windows titles, the executable description and the Start shortcut use `Channel Checker`; the in-app heading and product metadata retain the MabiTools brand. Setup always uses `MabiTools\Channel Checker` in Start, leaving the top-level `Channel Checker` shortcut to legacy WPF installations in either install order. Upgrades remove former native shortcuts only when their target matches this installation. Uninstall preserves shortcuts now owned by another application, including paths recorded by earlier native installers. The updater recognizes both the current and previous native display names while requiring the same AppId, install marker and path.
