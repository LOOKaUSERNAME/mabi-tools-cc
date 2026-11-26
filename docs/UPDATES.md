# Native updates

The native stable endpoint is
`https://raw.githubusercontent.com/LOOKaUSERNAME/mabi-tools-releases/main/channels/cc/native-stable-v1.json`.
The fallback is the GitHub contents API for that same path on `main`.
Neither endpoint is the legacy C# feed or the Hotkey Manager feed. Metadata
redirects are refused. Merely compiling the endpoint does not publish a feed.

The document uses schema 1, product `cc`, channel `stable`, feed identity
`cc-native-stable-v1`, architecture `windows-x64`, and installer AppId
`{B81A179F-E1D0-471B-B274-B521D71C8088}`. It requires a calendar version,
matching `cc-v<version>` hub tag, matching annotated source tag and object ID,
source commit, and exactly one installer and one portable asset. Asset names,
release URLs, positive byte counts up to 256 MiB, and SHA-256 hashes are checked.
Only a strictly newer version becomes available; equal-version rebuilds do not.

Normal startup checks once. Setup detection requires all three of the exact
per-user install path, installer-written `install-kind.ini`, and the existing
AppId's uninstall registration. Other copies use portable mode. Portable mode
offers only the validated ZIP URL and never downloads or launches Setup.

An installed copy streams to an exclusive app-owned `.part` file under the
validated profile. Downloads allow at most three HTTPS redirects, restricted
to the hub's GitHub release paths, `release-assets.githubusercontent.com`, and
`objects.githubusercontent.com`. TLS validation remains enabled. The transfer
has a 256 MiB ceiling, a 30-second idle limit and a 15-minute total limit.
Exact size and SHA-256 are required before the cache becomes ready. Cache
reuse repeats verification. Installation requires confirmation, closes and
drains the app, then rehashes the file under a Windows handle that denies
write/delete access through process creation. No metadata-supplied arguments,
shell launch, or silent Setup switches are accepted.

Metadata, downloads and maintenance are cancelled and fully drained before
measured probes start. Pending work resumes once after the scan; close never
resumes it. A cancelled partial is removed. Completed cache files are never
trusted solely because they exist. Updates preserve the native AppId,
installation directory, history format and preferences.

The application is built only with its fixed native HTTPS transport. It has no
fixture-port constructor, URL override, or runtime switch for a test feed.
CMake rejects obsolete feed-selection options. Separate regression executables
compile loopback transports; these are excluded from the package. Ordinary
self-checks, `--isolated-profile`, and `--legacy-profile` cannot use live feeds.
Production transport configuration is recorded in `manifest.json` and
`result.json`. The explicit `cc_https_tests --live-native-feed <result.json>`
diagnostic uses the same HTTPS code and never downloads or launches Setup.

`tools/New-NativeFeed.ps1` prepares a local feed from a build result, checking
the actual assets before writing it. Run `cc_update_tests --validate-feed
<file> <installed-version>` to apply the application's parser. No release IDs,
publication timestamps or upload status are inferred. Publishing a feed is a
separate operation after the corresponding assets and tags are available.
