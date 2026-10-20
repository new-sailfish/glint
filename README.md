# Glint

中文安装步骤见 [使用说明](README.zh-CN.md)。

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

## Install and connect a browser

Run `Glint.exe` from a folder you plan to keep. The first launch opens the
**连接浏览器** guide; you can reopen it from the search window or tray menu.
The bundled extension connects without copying or entering an extension ID.

1. **Choose Chrome or Edge.** Click **下一步：安装扩展**. Glint extracts its
   bundled extension to `%LOCALAPPDATA%\Glint\extension` and registers the
   native host for the selected browser under your Windows user account.
2. **Open the extensions page and load Glint.** Click
   **打开浏览器并复制页面地址**. In the browser, use the profile avatar to select
   the profile whose tabs you want to search. In that profile's window, press
   `Ctrl+L`, then `Ctrl+V`, then `Enter` to visit the copied extensions-page
   address: `chrome://extensions/` for Chrome or `edge://extensions/` for Edge.

   Return to Glint and click **复制扩展文件夹路径**, then return to the browser.

   In **Chrome**, turn on **开发者模式** / **Developer mode** at the upper right,
   then click **加载已解压的扩展程序** / **Load unpacked**. In **Edge**, turn on
   **开发人员模式** / **Developer mode** in the left sidebar, then click
   **加载解压缩的扩展** / **Load unpacked**.

   In the folder chooser, press `Alt+D` to focus its address bar, `Ctrl+V` to
   paste the copied directory, and `Enter` to open it. Finally, click
   **选择文件夹** / **Select Folder**. Select the `extension` folder itself.
3. **Check the connection.** Return to Glint and click
   **我已加载扩展，检查连接**. Wait for Glint to receive that browser's tab
   snapshot, then click **开始搜索**. Use **连接另一个浏览器** to add the other
   browser. Later, `Ctrl+Shift+Space` opens search (unless you changed the hotkey).

The button opens the selected browser; you paste the page address to enter its
extensions manager. If the browser cannot be opened automatically, open it
yourself and enter `chrome://extensions/` or `edge://extensions/` in the target
profile's address bar. Copy **页面地址** for the browser address bar, then use
**复制扩展文件夹路径** for the folder chooser; these are two separate copy steps.

Chrome and Edge require you to confirm **Load unpacked**; a regular desktop
program cannot silently install an unpacked extension. Load the extension in
every profile whose tabs should be searchable. The extension's options page
opens after its first installation and can later be reopened from the toolbar
icon. It offers **重试连接** and an optional profile label such as “工作” or
“个人”. A bridge port opening alone does not prove that Glint received the
tabs; the desktop guide confirms the selected browser's synchronization.

The bundled extension uses the same fixed ID in Chrome and Edge:
`pecljnjkgandbbelbajkppllibpehhpe`. Its manifest contains an RSA public key
that fixes the ID across unpacked folder locations; it contains no signing
private key. Each generated native-host manifest allows only its registered
extension ID. Loading the repository/package `extension` directory is also
possible during development.

**Upgrading an older installation:** Glint refreshes its extracted extension
files, but an already installed extension must be reloaded in the browser's
extensions page. If it still shows its old ID instead of the fixed ID above,
remove the old extension and use **Load unpacked** to load the new directory
shown by the Glint guide. Then run the guide for that browser again. Removing
an extension may reset its optional profile label. If you move `Glint.exe`, run
the guide again to update the host executable path; the extracted extension
directory stays in the same location.

The extension sends a complete snapshot after every connection and rebuilds
the index after a browser or desktop restart. Closing a tab, moving it between
windows, changing its title/URL, and activating it are sent as incremental
updates. Activation restores a minimized window before selecting the tab.

## Advanced: other extension versions and native-host registration

For a store version or another extension ID, open the desktop guide's browser
selection page, select the matching browser, and click
**其他版本：手动填写扩展 ID**. The extension options page keeps **复制 ID** under
**高级排查：旧版或其他版本扩展**. Enter that ID
for the matching browser. IDs must contain exactly 32 lowercase `a`–`p`
characters. The normal bundled-extension flow above fills the ID automatically.

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

The C++ tests cover strict JSON, UTF-8, partial Native Messaging frames,
connection isolation, duplicate tab IDs, snapshots, search, deletion and
disconnect, plus isolated native-host registration, bundled-file extraction,
browser launching, and the real Win32 guide's navigation, connection gating,
keyboard focus and 100/150/200% DPI layout. GitHub Actions builds Release x64 on `windows-latest`, runs both
test suites, checks imports for dynamic VC++ or unexpected third-party DLLs,
and uploads a zip containing `Glint.exe`, the extension and instructions.

Manual validation is still required with real Chrome/Edge profiles: extension
loading, multiple windows and profiles, Chinese IME input, minimized-window
activation, browser restart/reconnect, and Explorer restart tray recovery.

## Source layout

- `include/glint/`, `src/core.cpp`: JSON, framing, search and tab index.
- `src/win_app.cpp`, `native_host.cpp`, `win_common.cpp`: desktop and pipe bridge.
- `src/setup_window.cpp`, `browser_launch.cpp`, `registration.cpp`,
  `extension_bundle.cpp`: native connection guide and embedded extension setup.
- `extension/`: shared Chrome/Edge MV3 extension and its tests.
- `tests/`: C++ core and native Windows regression tests.
- `.github/workflows/`, `scripts/`: build, import checks and packaging.
- `docs/`: architecture, protocol and dated validation records.
