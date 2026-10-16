# Glint

Glint is a small Windows 10/11 x64 resident search window for open Chrome and
Edge tabs. The first release uses one C++20 executable in two modes:

* Desktop mode owns the tray icon, `Ctrl+Shift+Space` global hotkey, native
  Unicode search window, in-memory index and activation requests.
* `--native-host` mode is launched by a Manifest V3 extension. It forwards
  length-prefixed UTF-8 JSON over a current-user named pipe to the desktop
  process. It never writes diagnostics to protocol stdout.

The extension only requests `tabs`, `nativeMessaging` and `storage`. Install it
in every Chrome or Edge profile whose tabs should be searchable. Profile names
are not inferred from the browser; the extension options page provides an
optional local label.

## Build on Windows

Use a VS 2022 x64 developer prompt with CMake 3.21+ and Ninja:

```powershell
cmake --preset release
cmake --build --preset release
ctest --preset release --output-on-failure
```

Release uses `/MT`, so the executable does not require the VC++ runtime
redistributable. The core library has no third-party dependency. The generated
manifest embeds `asInvoker`, PerMonitorV2 DPI awareness, and Common Controls
v6. The executable is built as a Windows GUI application and writes no normal
desktop output; native-host protocol output is reserved for frames.

## Register the native host (current user)

After building, choose the extension IDs shown on `chrome://extensions` and
`edge://extensions` in Developer mode:

```powershell
build\release\Glint.exe --register `
  --chrome-id abcdefghijklmnopqrstuvwxyzabcdef `
  --edge-id abcdefghijklmnopqrstuvwxyzabcdef
```

This writes manifests below `%LOCALAPPDATA%\Glint` and registers both
`HKCU\Software\Google\Chrome\NativeMessagingHosts\com.glint.host` and
`HKCU\Software\Microsoft\Edge\NativeMessagingHosts\com.glint.host`. If an
executable path changes, run the command again. To remove the registration:

```powershell
build\release\Glint.exe --unregister
```

The registration command does not need administrator rights. The checked-in
`extension/host-manifest.chrome.json` and `.edge.json` files are templates for
manual registration; replace the path and extension ID if using them directly.

## Load the extension during development

1. Build and register the host.
2. Open `chrome://extensions` or `edge://extensions`.
3. Enable Developer mode and choose **Load unpacked**.
4. Select the repository's `extension` directory.
5. Repeat for each browser profile that should be indexed.
6. Optionally open the extension options page and set a profile label.

The extension sends a complete snapshot after every connection and rebuilds
the index after a browser or desktop restart. Closing a tab, moving it between
windows, changing its title/URL, and activating it are sent as incremental
updates. Activation restores a minimized window before selecting the tab.

## Tests and packaging

The JavaScript bridge tests use Node's built-in runner:

```powershell
npm --prefix extension test
```

The C++ tests cover strict JSON, UTF-8, partial Native Messaging frames,
connection isolation, duplicate tab IDs, snapshots, search, deletion and
disconnect. GitHub Actions builds Release x64 on `windows-latest`, runs both
test suites, checks imports for dynamic VC++ or unexpected third-party DLLs,
and uploads a zip containing `Glint.exe`, the extension and instructions.

Manual validation is still required with real Chrome/Edge profiles: extension
loading, multiple windows and profiles, Chinese IME input, minimized-window
activation, browser restart/reconnect, and Explorer restart tray recovery.
