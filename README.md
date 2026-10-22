<p align="center">
  <img src="docs/assets/glint-banner.svg" alt="Glint — Find your tab. Keep your flow." width="800">
</p>

中文安装步骤见 [使用说明](README.zh-CN.md)。

Glint is a small Windows 10/11 x64 resident search window for open Chrome and
Edge tabs. The first release uses one C++20 executable in two modes:

* Desktop mode owns the tray icon, customizable global hotkey (default
  `Ctrl+Shift+Space`), native
  Unicode search window, in-memory index and activation requests.
* Native-host mode runs as a browser-owned `GlintBridge.exe` copy of the same
  executable, installed automatically in a per-user, SHA-256-versioned directory.
  It is launched by a Manifest V3 extension and forwards
  length-prefixed UTF-8 JSON over a current-user named pipe to the desktop
  process. It never writes diagnostics to protocol stdout.

Exiting from the tray stops the desktop `Glint.exe`, its window and hotkey.
One bridge per connected browser profile remains idle, waiting for the desktop
pipe. Starting Glint again automatically rebuilds the tab index; no connection
button is needed. Browser shutdown or disabling the extension closes its bridge.
To stop every background process, disable the extension and exit the desktop.
The single EXE download prepares its bridge copy; no second download is needed.

The extension only requests `tabs`, `nativeMessaging` and `storage`. Install it
in every Chrome or Edge profile whose tabs should be searchable. Profile names
are not inferred from the browser; the extension options page provides an
optional local label.

## Keyboard controls and custom shortcut

In the query, Up keeps focus in the query and Down selects the first result.
Pressing Up on the first result returns focus to the query, keeping its text
and caret. Home and PageUp also return from the first result, including Fn+Up
when a keyboard maps it to either key. Elsewhere in the list they retain normal
navigation; in the query they retain normal editing behavior.
Enter activates a selected result; Escape hides search.

Right-click the tray icon and choose **配置全局热键**. Focus the recording field,
press a combination such as `Ctrl+Alt+K`, then **保存**. Letters, numbers and other
ordinary keys require Ctrl, Alt or Win; Shift can be added. F1–F24 can be used
alone or with modifiers, including Shift; F12 is reserved.
**恢复默认** restores the default draft; Save commits it. In this settings window,
Tab/Shift+Tab move focus between the recording field and buttons, Enter saves the
current shortcut, and Escape closes the window and discards unsaved changes.

Save checks availability with Windows before replacing the current registration.
An occupied or reserved combination shows an inline error and leaves the old
shortcut working. Cancellation or a failed save keeps the existing configuration.
Settings persist per Windows user and legacy preset choices are retained.

## Build on Windows

Use PowerShell 7 in a VS 2022 x64 developer environment with CMake 3.21+ and
Node.js 22+. The extension has no external packages to install:

```powershell
.\scripts\check-release.ps1
```

This is the complete local Release gate: it configures and builds, runs CTest,
the extension tests, the desktop/bridge lifecycle smoke, import checks, and
then publishes and verifies `dist/` and the ZIP. During focused iteration you
can skip completed steps explicitly, for example
`.\scripts\check-release.ps1 -SkipConfigure -SkipBuild -SkipCtest -SkipLifecycle -SkipImports -SkipPackage`.
Skipping a validation check requires `-SkipPackage`, so a focused run cannot
accidentally publish a release. Stop the desktop before the lifecycle smoke.

### Automatic product versions

Desktop builds increment the patch version: `0.1.0` → `0.1.1` → `0.1.2`.
`VERSION` sets the manual version floor. Every build that includes the `glint`
target consumes the next number, including an otherwise up-to-date build;
configure-only, test-only targets, and repackaging do not. Failed builds can
leave gaps. The local counter lives in ignored `.local/build-version/`, shared
by Debug and Release, and survives deleting `build/`. Preserve that directory
to preserve your local numbering. Invalid or exhausted counters fail explicitly.

Official CI builds have an independent counter. After script checks pass,
GitHub Actions reserves the next `build-version/0.1.N` tag atomically; reruns
reserve another number. These tags record allocations and are not Releases.
Do not delete them if you want CI numbering to remain continuous. Pull requests
use an isolated local preview counter and have no tag-writing permission.
Only `v*` tags trigger the existing release-publishing workflow.

The product version appears in the window title, tray tooltip and EXE properties.
Windows stores the numeric file version as `0.1.N.0`; the release manifest records
both forms directly from the packaged EXE. Local and CI versions can share a
number, so use the manifest's commit and file hashes to identify the exact build.
The browser extension's version and Native Messaging protocol version are separate.
Generated version files stay under `build/` and do not modify tracked sources.

For a quick pull-request feedback loop, list tests selected by the current diff:

```powershell
.\scripts\test-changed.ps1 -List
.\scripts\test-changed.ps1
# Explicit scope (after rebuilding changed C++ code):
.\scripts\test-changed.ps1 src/hotkey.cpp extension/options.js
```

Changed-test selection is an acceleration aid; the Release gate still runs the
complete CTest suite before packaging. The selector requires an existing Release
build and does not compile it. `-Base <ref>` or `TEST_BASE` overrides the Git
comparison base. With no override it checks the repository's main/master refs.

After validation, always package the release before handing it over. The package
script updates `dist/Glint.exe`, the extension files and documentation, refreshes
`Glint-win-x64.zip`, and verifies each published file against its source.
Use `dist/Glint.exe` as the local release entry point. `dist/release-manifest.json`
records the commit, dirty working-tree state, and SHA-256 of every packaged
source file. Obsolete files are removed from the managed release folders;
the `dist/browser-extension` path is preserved for installed profiles.

Release uses `/MT`, so the executable does not require the VC++ runtime
redistributable. The core library has no third-party dependency. The generated
manifest embeds `asInvoker`, PerMonitorV2 DPI awareness, and Common Controls
v6. The executable is built as a Windows GUI application and writes no normal
desktop output; native-host protocol output is reserved for frames.

## Install and connect a browser

Download **Glint.exe** from the release assets and keep it at a stable location.
That single executable is sufficient to distribute and run Glint: it embeds
the five browser-extension runtime files and four icon assets; the connection guide extracts them
to `%LOCALAPPDATA%\Glint\browser-extension`. You do not need to download an
extension folder separately. The optional **Glint-win-x64.zip** includes the same EXE,
extension files and documentation for users who want the full bundle.

Run `Glint.exe`. The search window shows a browser connection table with
`chrome-{profile}` and `edge-{profile}` rows, connection states and tab counts.
Click **连接新的浏览器** to open the illustrated connection guide; the tray menu
and `Glint.exe --setup` also open it. Unnamed profiles get a session-local number.
Profiles observed during the current desktop session remain in the table as
disconnected after closing their browser, without stale tab counts.
The bundled extension connects without copying or entering an extension ID.

1. **Choose Chrome or Edge.** Click **下一步：打开扩展页**. Glint extracts its
   bundled extension to `%LOCALAPPDATA%\Glint\browser-extension` and registers the
   native host for the selected browser under your Windows user account.
2. **Open the extensions page.** Click
   **打开浏览器并复制页面地址**. In the browser, use the profile avatar to select
   the profile whose tabs you want to search. In that profile's window, press
   `Ctrl+L`, then `Ctrl+V`, then `Enter` to visit the copied extensions-page
   address: `chrome://extensions/` for Chrome or `edge://extensions/` for Edge.

3. **Enable developer mode.** In **Chrome**, turn on **开发者模式** / **Developer
   mode** at the upper right. In **Edge**, turn on **开发人员模式** / **Developer
   mode** in the left sidebar. Return to Glint and continue to the next step.
4. **Load Glint.** Click **复制扩展文件夹路径**, then return to the browser.
   Click **加载已解压的扩展程序** in Chrome or **加载解压缩的扩展** in Edge
   (**Load unpacked**).

   In the folder chooser, click the **文件夹** / **Folder** input near the
   bottom, paste the complete copied path with `Ctrl+V`, and click
   **选择文件夹** / **Select Folder** directly. You do not need to enter the
   directory first. The path must end in `Glint\browser-extension`, which contains
   `manifest.json`; selecting its parent `Glint` directory will not load Glint.
   If you have already entered `browser-extension` and the chooser does not
   accept the current folder, go up one level, single-click the
   `browser-extension` folder to select it, then click **Select Folder**.
5. **Check the connection.** Return to Glint and click **已加载，检查连接**.
   After Glint receives that browser's snapshot, the page shows **已连接**, profile
   and tab counts, and **开始搜索**. Use **连接新的浏览器** to add another browser
   or profile. A disconnected page shows **未连接**; **连接遇到问题** opens repair
   separately. Later, `Ctrl+Shift+Space` opens search (unless you changed the hotkey).

The button opens the selected browser; you paste the page address to enter its
extensions manager. If the browser cannot be opened automatically, open it
yourself and enter `chrome://extensions/` or `edge://extensions/` in the target
profile's address bar. Copy **页面地址** for the browser address bar, then use
**复制扩展文件夹路径** for the folder chooser; these are two separate copy steps.

Chrome and Edge require you to confirm **Load unpacked**; a regular desktop
program cannot silently install an unpacked extension. Load the extension in
every profile whose tabs should be searchable. After loading it, return to the
desktop guide to check the connection. Installing the extension does not open
an extra browser tab. Click the Glint toolbar icon to open its connection-status
popup, retry the connection or set an optional profile name such as “工作” or
“个人”. The extension's **Details → Extension options** entry keeps the full
page available for advanced troubleshooting. A bridge port opening alone does
not prove that Glint received the tabs; the desktop guide confirms the selected
browser's synchronization.

The extracted directory must remain on disk because Chrome/Edge load unpacked
extensions from a directory. Glint prepares and refreshes that directory from
the EXE; this does not silently install the extension. A future store release
could simplify browser installation, but no store installation is provided here.

The bundled extension's public-key ID is the same in Chrome and Edge:
`pecljnjkgandbbelbajkppllibpehhpe`. Its manifest contains an RSA public key
that fixes the ID across unpacked folder locations; it contains no signing
private key. Automatic registration restricts `allowed_origins` to two explicit
extension IDs: this public-key ID and the legacy path-derived ID of the former
`%LOCALAPPDATA%\Glint\extension` directory. The second origin supports older
unpacked installations that already received an ID based on that directory;
it does not allow arbitrary extension IDs or use wildcards. Manual registration
still allows only the one ID supplied for that browser. Loading the
repository/package `extension` directory is also possible during development.

The ID can be known before installation because Chrome/Edge derive it from
the manifest's public key. The same public key yields the same ID on different
machines. Unpacked extensions without a key generally derive their identity
from their directory, so a different machine or path may yield a different ID.
A store release must use its actual assigned identity. The fixed ID above is
specific to the key bundled here; the guide accepts the actual extension URL
when an installed copy has a different identity.

**Upgrading to extension 0.1.1:** Run the guide again and **Load unpacked** from
`%LOCALAPPDATA%\Glint\browser-extension` once in each browser profile. If
Glint's Chrome card has disappeared, load this new directory directly. In Edge,
load the new directory once too to receive the toolbar popup. For an existing
copy with the same fixed public-key ID, loading the new directory updates its
source path; you do not need to remove it first. Glint preserves the old
`%LOCALAPPDATA%\Glint\extension` directory without overwriting or deleting it.

After loading the stable directory, close and reopen the browser normally and
confirm that Glint's card remains installed.
An old copy without a public key that remains visible can optionally be removed;
removal may reset its optional profile name. For an old extension loaded from
another directory, the guide's troubleshooting page still accepts its actual URL.
Ordinary desktop startup refreshes the embedded extension files and existing
valid host registrations while preserving their allowed origins. Moving
`Glint.exe` therefore needs no ID change; an update failure is reported and
the guide can repair it.

**Upgrading to extension 0.1.6:** Exit the old Glint from its tray menu, start
the updated EXE, then click **Reload** on the existing Glint card in each
browser's extensions manager to apply the unified icon and refreshed status
panel. The extension ID and existing unpacked-extension path stay the same.

**Upgrading to extension 0.1.5:** Exit the old Glint from its tray menu, start
the updated EXE, then click **Reload** on the existing Glint card in each
browser's extensions manager. This fixes the activation handoff that could
briefly raise the requested window before another window covered it. Activation
now waits for stable focus and retries briefly if focus is lost during the
handoff. The extension ID and existing unpacked-extension path stay the same.

**Upgrading to extension 0.1.4:** Start the updated EXE, then click **Reload**
on the existing Glint card once in each browser's extensions manager. No
reinstallation or ID change is needed; subsequent desktop stops and starts
reconnect automatically. The popup is green only after desktop confirmation.
An older background worker or missing status fields instead shows **扩展需要重新加载**;
reload the card to activate the updated background code.

When upgrading to extension 0.1.3, reload its existing card once in each browser.
This one-time upgrade step does not change its ID. Future desktop starts
connect automatically, including after closing the desktop while the browser
stays open.

The extension sends a complete snapshot after every connection and rebuilds
the index after a browser or desktop restart. Closing a tab, moving it between
windows, changing its title/URL, and activating it are sent as incremental
updates. Activation selects the tab, restores a minimized window and confirms
the target window is actually focused.

Chrome/Edge 105 or newer is required so an open Native Messaging port keeps
the Manifest V3 worker alive while its bridge waits for the desktop.

## Advanced: other extension versions and native-host registration

If connection keeps waiting, the final connection step accepts the extension
options page's complete `chrome-extension://…/options.html`
address or its ID directly. Click **保存 ID 并重试**, then **重试连接** on the
extension page. A standalone ID must contain exactly 32 lowercase `a`–`p`
characters. Saving registration does not enable search until a real snapshot arrives.
The browser selection page also provides **其他版本：手动填写扩展 ID** for
registering a store version or another ID before installation.
Advanced and CLI registration write a single allowed origin for each supplied
browser ID; they do not add the automatic flow's path-derived compatibility ID.

The CLI remains available. For example, these IDs register the bundled extension
for both browsers; substitute the actual ID when using another extension:

```powershell
build\release\Release\Glint.exe --register `
  --chrome-id pecljnjkgandbbelbajkppllibpehhpe `
  --edge-id pecljnjkgandbbelbajkppllibpehhpe
```

Supply only `--chrome-id` or `--edge-id` to update one browser and preserve the
other browser's registration. Manifests are written below
`%LOCALAPPDATA%\Glint`; the corresponding registry keys are
`HKCU\Software\Google\Chrome\NativeMessagingHosts\com.glint.host` and
`HKCU\Software\Microsoft\Edge\NativeMessagingHosts\com.glint.host`.
Registration needs no administrator rights. If the executable path changes,
register again. The checked-in `extension/host-manifest.chrome.json` and
`.edge.json` are templates for manual registration; replace the path and ID
before using them.

Remove both browsers' native-host registrations:

```powershell
build\release\Release\Glint.exe --unregister
```

Reopen the guide after choosing **稍后设置**:

```powershell
build\release\Release\Glint.exe --setup
```

## Tests and packaging

The JavaScript bridge tests use Node's built-in runner:

```powershell
npm --prefix extension test
```

`scripts/test-changed.ps1` selects the relevant CTest targets and extension
tests from changed paths, always retaining a small smoke set and falling back
to that set for shared files with broad impact. Use `-List` to inspect the
selection and its reasons; pass explicit paths when reviewing a staged change.

On Windows, with the desktop stopped, verify three tray exits and automatic
reconnections through the same native bridge process:

```powershell
.\scripts\test-desktop-lifecycle.ps1 -Binary .\build\release\Release\Glint.exe `
  -ReportPath .\build\diagnostics\desktop-lifecycle-smoke.json
```

This test starts only its own desktop and bridge processes; it does not operate
browser windows or replace real Chrome/Edge testing.

The C++ tests cover strict JSON, UTF-8, partial Native Messaging frames,
connection isolation, duplicate tab IDs, snapshots, search, deletion and
disconnect, plus isolated native-host registration, bundled-file extraction,
browser launching, and the real Win32 guide's navigation, connection gating,
keyboard focus and 100/150/200% DPI layout. Real isolated Windows pipe tests
cover duplex forwarding, invalid/fragmented frames, EOF, disconnect, shutdown
races, independent clients, bounded outbound queues and stalled-peer failures.
GitHub Actions builds Release x64 on `windows-2022` with VS 2022, matching the
CMake preset instead of following changes to `windows-latest`. Pull requests and
official builds run script checks, non-desktop CTest targets (`-LE desktop`), and
extension tests. Official builds also check imports, package and upload the EXE,
ZIP and hash manifest. Window geometry, DPI, focus, global-hotkey and desktop
lifecycle checks run in the complete local `check-release.ps1` gate; CI does not
depend on a virtual desktop's display settings. Documentation-only changes skip CI, and newer
runs cancel obsolete runs of the same ref.
Tag releases attach both files directly, so users can choose the EXE without
downloading or arranging a package folder.

Manual validation is still required with real Chrome/Edge profiles: extension
loading, multiple windows and profiles, Chinese IME input, minimized-window
activation, browser restart/reconnect, and Explorer restart tray recovery.

The workflow scripts have small fixture-based regression tests, without extra
dependencies. Run the relevant harness when changing a development script:

```powershell
pwsh -NoProfile -File scripts/tests/test-changed.test.ps1
pwsh -NoProfile -File scripts/tests/check-release.test.ps1
pwsh -NoProfile -File scripts/tests/package-release.test.ps1
pwsh -NoProfile -File scripts/tests/build-version.test.ps1
pwsh -NoProfile -File scripts/tests/reserve-ci-version.test.ps1
```

## Source layout

- `include/glint/`, `src/core.cpp`: JSON, framing, search and tab index.
- `src/win_app.cpp`, `pipe_server.cpp`, `native_host.cpp`, `win_common.cpp`:
  desktop, current-user pipe server and Native Messaging bridge.
- `src/bridge_installation.cpp`: content-versioned native bridge preparation.
- `src/setup_window.cpp`, `browser_launch.cpp`, `registration.cpp`,
  `extension_bundle.cpp`: native connection guide and embedded extension setup.
- `extension/`: shared Chrome/Edge MV3 extension and its tests.
- `tests/`: C++ core and native Windows regression tests.
- `.github/workflows/`, `scripts/`: build, import checks and packaging.
- `VERSION`, `cmake/`, `app.manifest.in`: automatic product-version generation.
- `docs/`: public architecture, protocol, branding and README artwork.

Release documentation is explicitly selected in `scripts/package-release.ps1`.
Local discussion drafts, browser snapshots and dated validation records are
excluded from Git and release packages. Add new public documentation to the
packaging list when it should ship with the release.
