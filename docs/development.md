# Development and release guide

[English overview](../README.md) · [中文概览](../README.zh-CN.md) · [User guide](user-guide.md)

Glint is a C++20 Windows application with a shared Chrome/Edge Manifest V3
extension. The desktop and native bridge use the same executable bytes under
different names. The core library has no third-party dependencies; the extension
tests use Node's built-in runner.

## Toolchain and build

Use **PowerShell 7** in a **VS 2022 x64 developer environment**, with
**CMake 3.21+** and **Node.js 22+**. No `npm install` step is needed.

```powershell
cmake --preset release
cmake --build --preset release --parallel
```

The configured output is `build/release/Release/Glint.exe`. The Debug preset is
available for debugging, but release validation and test selection use Release.
Release links the C++ runtime statically with `/MT` and depends only on Windows
system DLLs. Its manifest uses `asInvoker`, PerMonitorV2 DPI awareness and
Common Controls v6. The desktop is a GUI application; native-host stdout is
reserved for Native Messaging frames.

## Focused development checks

After completing a focused change, configure/build Release, then run:

```powershell
.\scripts\test-changed.ps1 -List
.\scripts\test-changed.ps1
```

`-List` reports selected tests and the reasons for selecting them. The selector
requires an existing Release build and does not compile it; rebuild after
changing C++ sources. Use explicit repository-relative paths or a Git base when
needed:

```powershell
.\scripts\test-changed.ps1 src/hotkey.cpp extension/options.js
.\scripts\test-changed.ps1 -Base origin/master
```

`TEST_BASE` also sets the comparison base. Without an override, the selector
checks the repository's main/master refs. It runs a smoke set plus related
tests. Build and resource changes select the full CTest suite; broad shared
changes retain smoke coverage. Keep mappings and smoke coverage current when
adding targets, so unmapped code cannot silently skip checks.

Run relevant checks once after the code is complete. Documentation-only or
comment-only changes do not need another application test run.

### Extension tests

```powershell
npm --prefix extension test
```

These cover metadata normalization, snapshot/event races, tab closure,
activation and focus, profile identity, reconnect backoff, desktop generations,
status UI and the bundled public-key ID. No external test framework is needed.

### Development-script tests

When changing a development or release script, run its corresponding fixture
harness. The scripts create temporary fixtures and require no additional test
dependencies.

```powershell
pwsh -NoProfile -File scripts/tests/test-changed.test.ps1
pwsh -NoProfile -File scripts/tests/check-release.test.ps1
pwsh -NoProfile -File scripts/tests/package-release.test.ps1
pwsh -NoProfile -File scripts/tests/build-version.test.ps1
pwsh -NoProfile -File scripts/tests/reserve-ci-version.test.ps1
pwsh -NoProfile -File scripts/tests/publish-update-feed.test.ps1
```

CI runs these script checks before building.

## Full release validation

Stop the desktop, then run:

```powershell
.\scripts\check-release.ps1
```

This is the complete local Release gate. It configures and builds, runs the full
CTest suite and extension tests, checks desktop/bridge lifecycle behavior and
native imports, then packages and verifies `dist/` and the ZIP.

Runs that skip validation checks are focused checks, not release validation.
Skipping a validation check requires `-SkipPackage`. For example, the following runs
extension tests against an existing workspace without publishing a package:

```powershell
.\scripts\check-release.ps1 -SkipConfigure -SkipBuild -SkipCtest -SkipLifecycle -SkipImports -SkipPackage
```

### Desktop and browser coverage

The lifecycle smoke can also run independently with the desktop stopped:

```powershell
.\scripts\test-desktop-lifecycle.ps1 -Binary .\build\release\Release\Glint.exe -ReportPath .\build\diagnostics\desktop-lifecycle-smoke.json
```

It checks three desktop tray exits and automatic reconnections through the same
native bridge. The test starts only its own desktop and bridge processes; it
does not operate browser windows or replace real Chrome/Edge validation.

C++ tests cover strict JSON and UTF-8, partial framing, isolated tab indexes,
search and removal, registration, extension extraction and browser launch.
Windows tests cover the connection guide's state transitions, focus and
100/150/200% DPI layouts, plus duplex pipes, disconnects, cancellation, queue
limits and stalled peers.

Before release, also validate real Chrome/Edge profiles: extension loading,
multiple windows and profiles, Chinese IME input, minimized-window activation,
browser restart/reconnect and Explorer restart tray recovery. Keep tests that
depend on desktop geometry, DPI, focus or hotkeys labeled `desktop`.

## Packaging and release outputs

After a Release build passes its relevant checks, package it before handing it
over. The complete release gate already performs this step; to package an
already validated build directly:

```powershell
.\scripts\package-release.ps1
```

The script synchronizes:

- `dist/Glint.exe` and both READMEs.
- Public documentation and assets under `dist/docs/`.
- Extension source and supporting files under `dist/extension/`.
- Five extension runtime files and four icons under `dist/browser-extension/`.
- The root `Glint-win-x64.zip` archive and `dist/release-manifest.json`.

The stable `dist/browser-extension/` location must remain available because
existing browser profiles may load their extension from it. The script removes
obsolete files only from managed release paths and verifies published files and
archive entries against their source bytes.

The manifest records the packaged EXE's product/file versions, source commit,
working-tree dirty state and each packaged source file's SHA-256. It identifies
the actual executable being packaged even if version source files have since
advanced.

If a running process prevents replacement of a different `dist/Glint.exe`,
packaging fails explicitly. Do not terminate a user's process without
authorization or leave an old binary presented as the new release. An identical
running EXE does not need replacement, so documentation can be repackaged around
the same validated bytes.

Public release documents are explicitly listed in
`scripts/package-release.ps1`. Add new public documents and assets to that list
when they should ship. Local discussions, browser snapshots and dated validation
records are excluded from release packages.

## Product versions

`VERSION` sets the manually maintained official version floor. Each build that includes
the `glint` target allocates the next patch version, even when the source is
otherwise up to date. Configure-only runs, test-only targets and repackaging do
not allocate versions. Failed builds can leave gaps. Never edit generated
headers or binary metadata to change a version.

| Version stream | Storage and behavior |
| --- | --- |
| Local builds | Default to `999.0.N`, using `.local/build-version/999.0.txt`, shared by Debug and Release and preserved when `build/` is removed. Earlier counters remain untouched. Invalid or exhausted counters fail explicitly. |
| Official CI builds | An independent allocation recorded atomically in `build-version/0.1.N` tags after script checks pass. Each rerun reserves a new number. These allocation tags are not Releases and should be preserved. |
| Pull requests | The same high local numbering, without permission to write version-allocation tags or publish releases. |

The product version appears in the window title, tray tooltip and EXE
properties. For a product version such as `0.1.N`, the Windows numeric file
version is `0.1.N.0`. The release manifest reads both from the packaged EXE.
The updater compares the three numeric components only. A normal local
`999.0.N` build is above the official `0.1.N` sequence, so it will not replace
development work. There are no separate updater channels or build-origin flags.

### Test automatic updates with a low local version

Use the build-time `GLINT_LOCAL_VERSION` environment variable to generate a
low version. Do not edit generated headers, EXE resources, `VERSION`, or the
counter files. The override does not consume a local number and cannot be
combined with `GLINT_CI_VERSION`.

From a VS 2022 x64 PowerShell 7 developer shell:

```powershell
if (Test-Path Env:GLINT_CI_VERSION) { throw 'Use a local developer shell.' }
$previousLocalVersion = $env:GLINT_LOCAL_VERSION
try {
    $env:GLINT_LOCAL_VERSION = '0.0.0'
    cmake --preset release -B .local/update-test/build
    if ($LASTEXITCODE) { throw 'Configure failed.' }
    cmake --build .local/update-test/build --config Release --target glint --parallel
    if ($LASTEXITCODE) { throw 'Build failed.' }
} finally {
    $env:GLINT_LOCAL_VERSION = $previousLocalVersion
}
```

Use `.local/update-test/build/Release/Glint.exe` as a disposable test copy.
Exit any existing desktop through its tray menu first; the desktop singleton
otherwise brings that instance forward. Choose **Software updates** and verify
the newer version downloads, remains pending during normal use, and installs
only when **Restart and update** is clicked. Check that language, shortcut and
browser-profile settings remain intact, the desktop restarts once, and tabs
resynchronize. Reload browser extensions when the updated-files notice appears.

A full online restart test requires the selected GitHub release to include the
updater's startup acknowledgment; older pre-updater releases cannot confirm
completion. Offline process fixtures exercise the helper handoff and replacement
without relying on GitHub or changing the user's browser registrations.

If the updated process stays alive without confirming startup, the helper leaves
it running and retains `.glint-update-<nonce>.backup` beside the installed EXE.
The corresponding diagnostic record is
`%LOCALAPPDATA%/Glint/updates/<nonce>/transaction.txt`. Close Glint normally
before restoring that backup or replacing the EXE with a fresh official download;
check the backup's SHA-256 against the record's previous-file hash first. Do not
launch `helper.exe` manually or edit the transaction record to force success.

Also check a default `999.0.N` build reports no newer version, automatic checks
can be disabled, offline failures leave search usable, and corrupted staged
files cannot install. The focused CTest targets are `glint_update_schedule`,
`glint_update_feed`, `glint_update_install`, `glint_update_process`, `glint_update_controller` and
`glint_update_window`; window checks carry the `desktop` label.

Before final delivery, clear `GLINT_LOCAL_VERSION`, return to the normal Release
preset, and run `scripts/check-release.ps1`. This restores the normal high local
version and synchronizes the validated `dist/` and ZIP. Preserve all existing
local counters and official `build-version/*` allocation tags.

Extension versions and the Native Messaging protocol version are separate.
Generated version files remain under `build/` and do not modify tracked source
files.

## Continuous integration and publication

The [application build workflow](https://github.com/new-sailfish/glint/blob/master/.github/workflows/build.yml) uses
`windows-2022` and VS 2022, matching the `Visual Studio 17 2022` CMake
generator. Keep these aligned.

Pull requests and official builds run script checks, Release x64 builds,
non-desktop CTest targets (`-LE desktop`) and extension tests. Official builds
also verify native imports and package the release. Desktop geometry, DPI,
focus, global-hotkey and lifecycle checks stay in the full local release gate,
where the actual Windows desktop is available.

Successful `master`/`main` builds and manual official builds automatically
publish a `v<build-version>` GitHub Release containing `Glint.exe`,
`Glint-win-x64.zip` and `release-manifest.json`. No manual tag push is needed.
Failed builds and pull requests do not publish. The application build workflow
skips documentation-only changes; newer runs cancel obsolete runs of the same
ref.

### Update-source publishing

After publishing the GitHub Release, the official build runs
`scripts/publish-update-feed.ps1` to verify the packaged EXE against its manifest,
reserved version and source commit, then publish the `update-feed` branch in
`new-sailfish/glint`. That branch contains the latest `release-manifest.json`
at its root and immutable `v<Version>/release-manifest.json` and
`v<Version>/Glint.exe` pairs. Older versions remain available so a cached latest
manifest still identifies downloadable bytes with the correct hash. Builds
finishing out of order retain their pinned files without moving latest backward.

The updater tries the GitHub Release manifest directly, then these metadata
mirrors. The latest manifest is at each listed root; a selected version's
manifest is also available at `v<Version>/release-manifest.json` under that root.

| Source | Latest manifest | Executable route in updater |
| --- | --- | --- |
| GitHub raw | `https://raw.githubusercontent.com/new-sailfish/glint/update-feed/release-manifest.json` | Not used for executable downloads |
| GitHub Pages | `https://new-sailfish.github.io/glint/updates/release-manifest.json` | `https://new-sailfish.github.io/glint/updates/v<Version>/Glint.exe` |
| jsDelivr Gcore | `https://gcore.jsdelivr.net/gh/new-sailfish/glint@update-feed/release-manifest.json` | Not used for executable downloads |
| jsDelivr Cloudflare test endpoint | `https://testingcf.jsdelivr.net/gh/new-sailfish/glint@update-feed/release-manifest.json` | Not used for executable downloads |

If the direct manifest and mirrors fail, the client tries the GitHub API before
its last-resort GitHub Release proxies: `ghfast.top`, `gh-proxy.com`,
`ghfile.geekertao.top` and `ghproxy.net`. Executable downloads use the pinned
GitHub Release asset, the Pages version directory and the configured fallback
routes; raw GitHub and jsDelivr serve metadata for the client. These are the
configured publication paths, and become available after an official build
publishes the feed and Pages deployment completes. A local build does not
publish or establish the online availability of these mirrors.

The Pages workflow runs after a successful application build from the repository's
default branch, as well as its existing website-change and manual triggers. It
fetches `update-feed` and stages its verified files under `_site/updates/` beside
the product website. Before the first feed publication, it publishes the website
without an updates directory. Remote errors and invalid feed files fail the job.

Publishing uses a temporary Git repository and a normal fast-forward push; it
does not rewrite source branches or overwrite an existing version with different
bytes. `GH_TOKEN` (or `GITHUB_TOKEN`) supplies repository contents write permission
through an environment-based credential helper. The build fails visibly if feed
publication fails, even when the GitHub Release already exists. A subsequent
official build publishes a fresh reserved version. The fixture harness exercises
publication and Pages staging using temporary local Git remotes only.

## Source layout

| Location | Responsibility |
| --- | --- |
| `include/glint/`, `src/core.cpp` | Core JSON, framing, tab index and search. |
| `src/win_app.cpp`, `src/pipe_server.cpp`, `src/native_host.cpp`, `src/win_common.cpp` | Desktop application, current-user pipe server and Native Messaging bridge. |
| `src/bridge_installation.cpp` | Content-versioned native bridge preparation. |
| `src/setup_window.cpp`, `src/browser_launch.cpp`, `src/registration.cpp`, `src/extension_bundle.cpp` | Native setup guide, browser launch, registration and embedded extension preparation. |
| `extension/` | Shared Chrome/Edge extension and JavaScript tests. |
| `tests/` | Core C++ and native Windows regression tests. |
| `scripts/`, `.github/workflows/` | Development checks, version allocation, packaging and CI. |
| `VERSION`, `cmake/`, `app.manifest.in` | Build-time version generation and application manifest. |
| `docs/` | User/developer documentation, architecture, protocol, branding and artwork. |

For the implementation model, read the [design](design.md),
[protocol](protocol.md) and [extension documentation](../extension/README.md).
