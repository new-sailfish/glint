# Glint v0.1 design

Glint is a resident Windows x64 application with one executable. Desktop mode
owns the in-memory tab index, tray icon, global hotkey and search window. Native
Messaging mode is a short-lived bridge process started by Chrome or Edge; it
copies browser frames to a per-user named pipe and copies responses back to
browser stdout. The bridge never participates in the desktop single-instance
mutex, so several browser profiles can connect concurrently.

The core library is Windows-independent C++20 and contains a strict bounded
UTF-8 JSON implementation, incremental Native Messaging framing, and a
connection-isolated tab index. The UI and bridge communicate through callbacks;
index updates happen on pipe worker threads and UI refreshes are posted to the
window message queue. Search is performed on a snapshot under the index mutex,
so the UI thread never reads a pipe or waits on a browser.

The first release supports only Chrome/Edge tabs supplied by the MV3
extension. Registration writes HKCU Native Messaging host keys for both
browsers and generates a host manifest with explicit allowed extension origins.
All runtime dependencies are Windows system DLLs; Release uses `/MT`.

## Browser connection guide

The native guide has three stages: choose one browser, load its bundled
extension, and confirm that the selected browser sent a full snapshot. The
extension has a checked-in public key, so its unpacked ID is stable across
directories. Preparing the second stage extracts the five extension files and
registers only the selected browser with that ID. Other extension identities
remain available in the advanced page and command-line registration.

Chrome 155 filters privileged internal URLs such as `chrome://extensions/`
supplied by external command lines (Chromium's
`startup::ValidateLaunchUrlWebUnsafe`). The guide therefore opens the selected
browser at `about:blank`, copies the management-page address, and asks the user
to paste it in the browser address bar. A separate copy action supplies the
extension directory to the folder chooser. Browser launch success is never
reported as extension installation or connection success.

The final stage counts only synchronized connections from the selected
browser. A hello message alone, or a snapshot from the other browser, does not
enable search. Back navigation preserves automatic/manual flow; disconnect
disables search and moves keyboard focus to an enabled control. The native
regression suite exercises those paths in an isolated HKCU hive and temporary
LOCALAPPDATA directory, and checks controls and text at 100/150/200% DPI.
