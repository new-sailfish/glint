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

Use a VS 2022 x64 developer prompt with CMake 3.21+:

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

The first desktop launch opens a native **连接浏览器** guide when no browser
has been configured. It explains the steps, opens the local extension folder,
accepts Chrome and Edge extension IDs, and registers the current-user host.
The same guide is available from the search window's **连接浏览器** button or
the tray menu. **开始搜索** becomes available after a browser profile sends its
first snapshot; a registered host alone does not mean that the browser is
connected.

You can also register from PowerShell. First load the unpacked extension in
each target profile (see the next section), then choose the extension IDs shown
on `chrome://extensions` and `edge://extensions` in Developer mode:

```powershell
build\release\Release\Glint.exe --register `
  --chrome-id abcdefghijklmnopqrstuvwxyzabcdef `
  --edge-id abcdefghijklmnopqrstuvwxyzabcdef
```

This writes manifests below `%LOCALAPPDATA%\Glint` and registers both
`HKCU\Software\Google\Chrome\NativeMessagingHosts\com.glint.host` and
`HKCU\Software\Microsoft\Edge\NativeMessagingHosts\com.glint.host`. If an
executable path changes, run the command again. To remove the registration:

```powershell
build\release\Release\Glint.exe --unregister
```

To reopen the desktop guide directly after choosing **稍后设置**:

```powershell
build\release\Release\Glint.exe --setup
```

The registration command does not need administrator rights. The checked-in
`extension/host-manifest.chrome.json` and `.edge.json` files are templates for
manual registration; replace the path and extension ID if using them directly.

## Load the extension during development

1. Build the executable and keep the repository's `extension` directory (or
   the packaged `extension` directory) beside it.
2. In the target Chrome profile, open `chrome://extensions`; in Edge, open
   `edge://extensions`.
3. Enable **Developer mode**, choose **Load unpacked** / **加载已解压的扩展程序**,
   and select the `extension` directory itself.
4. Open the extension's **Details → Extension options** page. The Glint page
   opens automatically after the first installation and can later be reopened
   from the pinned toolbar icon. Click **复制 ID**.
5. Return to Glint's **连接浏览器** guide, paste the ID into the matching
   Chrome or Edge field, and click **保存并注册**. This writes HKCU registry
   entries and a host manifest; no administrator permission is needed.
6. Return to the extension page and click **重试连接**. Confirm that Glint's
   search list shows the profile's tabs, then use `Ctrl+Shift+Space` (or the
   configured hotkey) to search.
7. Repeat steps 2–6 in every browser profile whose tabs should be searchable.
   A profile without the extension is intentionally absent from the index.

The extension only reads tab metadata. It never reads page contents. Chrome and
Edge may use different IDs, and a changed extension path or ID requires running
**保存并注册** again. For a release package, do not delete or move the
`extension` directory after loading it; the browser must be able to start the
same unpacked extension on the next launch.

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
