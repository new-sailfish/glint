# Glint native messaging protocol

The native messaging bridge and the desktop process exchange one UTF-8 JSON
message per frame. A frame is a four-byte unsigned little-endian payload length
followed by that many bytes. Empty payloads, invalid UTF-8, and payloads over
1 MiB are rejected. `FrameReader` accepts partial reads and can emit more than
one message for one read.

Every browser profile owns a bridge connection. The desktop assigns a local
`connectionId`; `tabId` and `windowId` are only unique inside that connection.
The bridge sends a new `hello` after every reconnect, followed by a complete
`snapshot`.

The browser-owned native host remains alive while the desktop is stopped.
Native host → extension control frames use:

```json
{"type":"desktop_status","version":1,"state":"waiting"}
{"type":"desktop_status","version":1,"state":"connected","generation":1}
```

`waiting` keeps the native port alive and sends no tab metadata. Each
`connected` session requires a fresh hello and snapshot. The extension attaches
`desktopGeneration` to its outgoing messages; the host forwards only the current
generation and discards offline/stale messages instead of queuing them. This
field is local to the bridge and is separate from desktop connection IDs.
The extension UI distinguishes a native port from a connected desktop.

```json
{"type":"hello","version":1,"sessionId":"runtime-session",
 "profileId":"profile-id","browser":"Chrome","profileLabel":"Default"}
```

`profileLabel` is optional because an extension cannot reliably discover the
browser profile's display name. The bridge may provide a configured label.

```json
{"type":"snapshot","tabs":[
 {"tabId":3,"windowId":1,"index":0,"title":"Docs",
  "url":"https://example.test/docs","active":true,"pinned":false}]}
```

An incremental update uses the same tab fields in a nested `tab` object:

```json
{"type":"upsert","tab":{"tabId":3,"windowId":1,"index":0,
 "title":"Docs","url":"https://example.test/docs","active":true}}
{"type":"remove","tabId":3}
```

The desktop requests activation through the bridge (the request may include
`requestId`, `tabId`, and `windowId`). The bridge reports the result as:

```json
{"type":"activate_result","requestId":"42","ok":true}
{"type":"activate_result","requestId":"42","ok":false,
 "error":"tab no longer exists"}
```

`TabIndex` requires `hello` and then `snapshot` before accepting `upsert` or
`remove`. A malformed message is rejected without changing the existing
connection state. `disconnect(connectionId)` removes all tabs for that
connection.
