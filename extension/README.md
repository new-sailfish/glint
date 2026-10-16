# Glint browser extension

This directory contains the shared Manifest V3 connector used by Chrome and Edge. It only reads tab metadata (`id`, `windowId`, title, URL and active state); it does not inject scripts or read page contents. Install it once in every browser profile whose tabs should appear in Glint.

## Load during development

1. Build the desktop executable and register its Native Messaging host with the current user (see the repository README and `host-manifest.*.json` templates).
2. In Chrome, open `chrome://extensions`, enable **Developer mode**, choose **Load unpacked**, and select this `extension` directory. In Edge use `edge://extensions` and the same **Load unpacked** action.
3. Copy the generated extension ID into the corresponding `allowed_origins` entry in the host manifest. The unpacked ID is stable while the extension's key and path remain unchanged; a release build may use a different ID.
4. Open the extension's **Details → Extension options** page and optionally set a profile label such as `Work`. The label is local metadata and is sent in the `hello` message.

Repeat step 2 for each Chrome/Edge profile. A browser profile that has not installed and enabled the connector cannot be indexed.

## Native Messaging protocol

The service worker opens a persistent `runtime.connectNative("com.glint.host")` connection. All messages are JSON objects with `version: 1`; the browser's native-messaging framing is supplied by the browser and host bridge.

Browser → desktop:

- `hello`: `{type, version, sessionId, browser: "chrome"|"edge", profileId, profileLabel?}`. `sessionId` is unique to one native connection; `profileId` is generated once and stored in `storage.local`; it is intentionally independent from Chrome/Edge's private profile name.
- `snapshot`: `{type, version, tabs: [{tabId, windowId, title, url, active}]}` after every initial connection/reconnection.
- `upsert`: `{type, version, tab: {tabId, windowId, title, url, active}}` for create/update/activate/attach/detach events.
- `remove`: `{type, version, tabId}` when a tab closes or can no longer be read.

Desktop → browser:

- `activate`: `{type, version, requestId, tabId, windowId}`. The connector focuses the window (restoring a minimized window to `normal`) and then calls `tabs.update(tabId, {active: true})`.

Browser → desktop activation result:

- `activate_result`: `{type, version, requestId, ok, error?}`. Errors are reported when either the window or tab no longer exists, so the desktop UI can avoid claiming a false success.

During a snapshot, tab events are queued. Once the snapshot is sent, each queued event re-reads the current tab before replaying it. This prevents a stale `tabs.query` result from overwriting a title, URL, window move or active state that changed during synchronization.

A dropped native connection is retried with exponential backoff (500 ms through 30 s). Reconnection always sends a new `hello` and complete snapshot. The index is memory-only and is rebuilt after restart.

## Tests

From the repository root:

```powershell
npm --prefix extension test
```

The tests use Node's built-in test runner and cover metadata normalization, snapshot/event race handling, activation of a minimized window, bounded reconnect backoff and stable profile IDs.

## Host manifest templates

`host-manifest.chrome.json` and `host-manifest.edge.json` are templates for the same native host name. Replace the executable path and extension ID before registering them under the current user's Native Messaging registry keys. The desktop installer/registration helper should generate these files so a changed executable path or a release extension ID can be registered again without administrator rights.

Keep protocol stdout reserved for Native Messaging frames in the host executable; diagnostics belong on stderr or the desktop log.

