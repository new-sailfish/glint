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
