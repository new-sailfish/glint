<p align="center">
  <img src="docs/assets/glint-logo.svg" alt="Glint logo" width="230" height="60">
</p>

# Glint · Desktop tab and window search

Find open Chrome and Edge tabs, eligible visible top-level Windows desktop
windows, and supported Notepad tabs from one search window on Windows 10/11 x64.

[![Latest release](https://img.shields.io/github/v/release/new-sailfish/glint?label=release&color=1677ff)](https://github.com/new-sailfish/glint/releases/latest)
![Windows 10/11 x64](https://img.shields.io/badge/Windows-10%2F11_x64-1677ff)
![Chrome 105+](https://img.shields.io/badge/Chrome-105%2B-4285F4?logo=googlechrome&logoColor=white)
![Edge 105+](https://img.shields.io/badge/Edge-105%2B-0078D7)
[![Build](https://github.com/new-sailfish/glint/actions/workflows/build.yml/badge.svg)](https://github.com/new-sailfish/glint/actions/workflows/build.yml)

[**Download Glint.exe**](https://github.com/new-sailfish/glint/releases/latest/download/Glint.exe) · [Project website](https://new-sailfish.github.io/glint/) · [中文](README.zh-CN.md) · [User guide](docs/user-guide.md)

## Project background

A page or document may be open in Chrome or Edge, in another application
window, or under a different browser profile. Remembering part of its title is
often easier than remembering where it is. Switching between windows to inspect
their tab bars adds another step to finding it.

Glint brings open browser tabs into one Windows search window, then extends the
same list to all eligible visible top-level desktop windows. Search for a title
or URL, then return to the existing tab or application window.

![Illustration of tabs spread across browsers, windows and profiles, brought into one Glint search](docs/assets/tab-overload-en.svg)

## Features and workflow

Press **Ctrl+Shift+Space** to see up to six open tabs or windows that you
successfully activated from a Glint result during this session. Opening or
focusing a target elsewhere does not add it to the recent cards.
Use **Tab / Shift+Tab** to select a card and **Enter** to switch. Start typing
part of a title or URL to show the search list. The first result is
selected automatically; press **Enter** to switch, or use the arrow keys to choose
another result. Glint activates the selected browser tab or desktop window and
brings it forward, restoring the window if it is minimized.

Recent cards and search results show the application icon and name. Expanded connection details also show browser icons and retain profile names.

The bottom status bar keeps browser connections compact. Click it or press
**F2** for details. **Ctrl+F** returns to search and **F6** moves to connection
controls; **Connect browser** stays available beside the status.

![Glint search window with sample tab data from Chrome and Edge profiles](docs/assets/search-en.png)

*Native interface render with sample tab data. [Image provenance](docs/assets/README.md).*

| Capability | Behavior |
| --- | --- |
| Search browsers and windows | One result list for connected Chrome and Edge profiles plus all eligible visible top-level windows from other applications. |
| Search desktop windows | Each unsupported top-level window appears as a window-level result carrying its visible title. Selecting it restores a minimized window and brings it to the foreground. |
| Search Windows 11 Notepad tabs | When Notepad exposes tabs through UI Automation as `TabItem` controls with `SelectionItemPattern`, those tabs replace the single Notepad window result and can be selected individually. Sublime Text's custom-drawn tabs stay window-level. |
| Keep results current | Browser tab changes and desktop window creation, closure or title changes update the index. |
| Use the keyboard | Global shortcut, arrow-key navigation, Enter to activate, and Esc to hide. The shortcut is configurable. |
| See connection status | A profile table shows connection state and tab counts. Optional profile labels distinguish work and personal browsers. |
| Work locally | Extensions send browser tab metadata through a local bridge; the desktop indexes native windows directly. The search index stays in memory and is rebuilt after restart. |

The extension reads browser tab titles, URLs, IDs and activation state. It does
not inject scripts or read page contents. Desktop windows are indexed by their
visible top-level titles and application names. Search covers connected browser
tabs, eligible window results, and supported Notepad tabs; it does not search
bookmarks or browsing history. Tab-level support beyond browsers is limited to
Notepad tabs exposed through UI Automation; Glint does not add a Sublime plugin
or expand arbitrary application UI trees into results.

## Install, configure and use

### Language

Glint supports English and Chinese. By default, a Chinese Windows display language selects Chinese; every other display language selects English. Choose **English / 中文** in the search window (or **Language / 语言** in the tray menu) to select **Follow system**, **English**, or **中文**. Your selection is saved and applies immediately.

All connected browser extensions follow the desktop language immediately, including when **Follow system** is selected. Reconnecting extensions receive the current language; offline extensions keep the last received language. Before their first connection, extensions follow the browser UI language. The product website keeps its own language selector. Browser-managed extension names and descriptions follow the browser language. Tab titles, URLs, and profile labels remain unchanged.

### Download

Download [**Glint.exe**](https://github.com/new-sailfish/glint/releases/latest/download/Glint.exe),
save it somewhere you will keep, and run it. Requirements are **Windows 10/11
x64**. Browser integration requires **Chrome or Edge 105+**; window search does
not require a browser extension. No administrator access or separate Visual C++
runtime installation is needed.

The EXE contains the browser extension and prepares it automatically. The
optional [full ZIP](https://github.com/new-sailfish/glint/releases/latest/download/Glint-win-x64.zip)
also includes extension files and documentation.

Glint checks for newer versions and downloads them in the background. Use
**Software updates** in the tray menu to view progress or disable automatic
downloads. Choose **Restart and update** when ready; ordinary exit does not
install an update. Reload the browser extension when prompted after its files
change. See the [update guide](docs/user-guide.md#updates-and-older-installations).

### Connect each browser profile

Click **Connect browser** in Glint and follow its five stages:

1. **Choose Chrome or Edge.** Click **Next: extensions**. Glint prepares
   `%LOCALAPPDATA%\Glint\browser-extension` and registers the local bridge for
   that browser under your Windows account. The bundled extension needs no ID entry.
2. **Open the extensions page.** Click **Open browser and copy address**. Select the
   intended browser profile, then press **Ctrl+L → Ctrl+V → Enter** to open
   the copied address: `chrome://extensions/` or `edge://extensions/`.
3. **Enable Developer mode.** In Chrome the switch is at the upper right; in
   Edge it is in the left sidebar. Return to Glint and continue.
4. **Load the extension.** Click **Copy extension folder path** in Glint, then **Load
   unpacked** in the browser. Paste the copied path into the folder chooser's
   **Folder** field and click **Select Folder**. Select `browser-extension`,
   the folder containing `manifest.json`, rather than its parent directory.
5. **Confirm synchronization.** Return to Glint and click **Loaded: check connection**.
   When the browser is shown as **connected** with the profile and tab counts, click **Start searching**.

| Choose a browser | Load the extension folder | Confirm the connection |
| --- | --- | --- |
| ![Glint browser selection](docs/assets/setup-browser-en.png) | ![Glint instructions for loading the extension folder](docs/assets/setup-load-en.png) | ![Glint confirms a synchronized browser connection](docs/assets/setup-connected-en.png) |

**Repeat the browser loading step in every profile whose tabs you want to
search.** Installation currently uses **Load unpacked**; no browser-store
installation is provided. Keep the extracted `browser-extension` directory
because the browser loads the extension from it. The page address goes into
the browser address bar; the folder path goes into the folder chooser.

If the browser does not open, open it yourself and enter its extensions-page
address. For connection errors, use **Connection help** in the guide or follow the
[troubleshooting steps](docs/user-guide.md#connection-troubleshooting).

### Search and configure the shortcut

| Key or action | Result |
| --- | --- |
| `Ctrl+Shift+Space` | Open search using the default global shortcut. |
| Type a title or URL fragment | Filter connected browser tabs, desktop window titles and application names, and supported Notepad tabs, then select the first result while keeping focus in the search field. |
| `↓` in the search field | Move focus to the first result. |
| `↑` on the first result | Return to the search field, retaining its text and caret. |
| `Enter` / `Esc` | Activate the selected browser tab or desktop window, including a supported Notepad tab / hide search. |
| Tray → **Configure shortcut** | Record and save a different shortcut. |

<img src="docs/assets/hotkey-en.png" alt="Glint global shortcut settings" width="560">

Click the Glint icon in the browser toolbar to view connection status or set an
optional profile label. Closing the search window keeps Glint in the tray.
**Exit Glint** in the tray menu stops the desktop and hotkey; browser-managed
bridges wait for the next launch. See the [user guide](docs/user-guide.md) for
shortcut rules, lifecycle details, upgrades, manual registration and uninstalling.

## Development

Glint uses **C++20 and Win32** for the desktop application and a shared
**Manifest V3 extension** for Chrome and Edge. Build with PowerShell 7 in a
VS 2022 x64 developer environment, CMake 3.21+ and Node.js 22+. The extension
uses Node's built-in test runner and needs no package installation.

![Local architecture: browser extensions communicate through GlintBridge with the desktop search window](docs/assets/architecture-en.svg)

Each browser profile runs the same extension. A browser-managed
`GlintBridge.exe` connects it to the desktop application through Native
Messaging and a current-user Windows named pipe. Glint prepares the bridge from
the downloaded EXE. See the [architecture](docs/design.md) and
[protocol](docs/protocol.md) for implementation details.

```powershell
cmake --preset release
cmake --build --preset release --parallel
.\scripts\test-changed.ps1
```

Before handing over a release, stop the desktop and run the full validation and
packaging gate:

```powershell
.\scripts\check-release.ps1
```

The usable release is `dist/Glint.exe`; the gate also produces
`Glint-win-x64.zip` and a version/hash manifest. The
[development guide](docs/development.md) covers test selection, version
allocation, CI, packaging and source layout. See also the
[extension documentation](extension/README.md), [design](docs/design.md) and
[wire protocol](docs/protocol.md).
