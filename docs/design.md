# Glint v0.1 design

Glint is a resident Windows x64 application distributed as one executable.
Its update controller checks the official GitHub Release feed asynchronously,
validates version-pinned EXE bytes against the release manifest, and keeps the
download pending until an explicit restart-and-update action. Feed/transport,
controller, native update window, and replacement helper are separate modules.
The helper is a trusted copy of the currently running EXE. It waits on the
original process handle, replaces the target with a rollback backup, and waits
for the new desktop's message loop and named pipe to acknowledge startup.
It never terminates browser processes or browser-owned bridges. Numeric version
comparison is identical for all builds; local builds normally use `999.0.N`.

Desktop mode owns the in-memory browser-tab and desktop-window indexes, tray
icon, global hotkey and search window. Registration installs the same bytes as a
per-user `GlintBridge.exe` under a SHA-256-versioned directory, avoiding
replacement of an in-use image.
Native Messaging mode is a browser-owned process started by Chrome or Edge; it
copies browser frames to a per-user named pipe and copies responses back to
browser stdout. The bridge never participates in the desktop single-instance
mutex, so several browser profiles can connect concurrently. Exiting the desktop
does not spawn it again. Each bridge waits on a cancellable, bounded-backoff
pipe retry while continuously consuming and discarding offline metadata. The
browser's stdin EOF terminates the bridge even while no desktop is present.

After a desktop pipe connection, the bridge emits a `desktop_status` message
with `state: connected` and a new generation. The extension replies with a new
hello and complete snapshot tagged with that generation. Old/offline metadata
is discarded. Pipe loss emits `state: waiting`, keeping the native port alive;
desktop restart automatically resynchronizes without an extension action.
Genuine native-port failures still use exponential reconnect backoff, reset
only after the bridge confirms a real desktop pipe connection.

The core library is Windows-independent C++20 and contains a strict bounded
UTF-8 JSON implementation, incremental Native Messaging framing, and a
connection-isolated tab index. The UI and bridge communicate through callbacks;
index updates happen on pipe worker threads and UI refreshes are posted to the
window message queue. Search is performed on a snapshot under the index mutex,
so the UI thread never reads a pipe or waits on a browser. Desktop window and
UI Automation discovery runs on a background target worker, so search does not
wait on native UI Automation calls.

Desktop window search covers all eligible visible top-level windows as
window-level results. Eligibility excludes Glint and Chrome/Edge windows, which
are represented through extension connections, as well as hidden, cloaked, tool
and system-shell windows. A window is identified by its visible top-level title
and application name; activating a result restores a minimized window and
requests foreground activation for the window. Unsupported applications
therefore remain searchable without requiring an extension or application
plugin.

Windows 11 Notepad receives additional tab-level indexing only when its UI
Automation tree exposes tab entries as `TabItem` controls with
`SelectionItemPattern`. Glint requires a complete set of uniquely identified,
titled and selectable tab entries before replacing the single Notepad window
result with individual tab results. It uses the selection pattern to choose the
requested tab before activating and foregrounding its window. A retryable UIA
scan or activation failure recreates the client once and retries the bounded
operation. Transient system hosts such as `TextInputHost.exe` are excluded while
`ApplicationFrameWindow` and otherwise eligible `CoreWindow` surfaces remain
available as desktop windows. Sublime Text's
custom-drawn tabs are not exposed through UI Automation, so Sublime contributes
one window-level result rather than tab results. Glint does not ship a Sublime
plugin, read arbitrary application content, or claim generic
arbitrary-application tab expansion.

The named-pipe server and native bridge open their duplex desktop handles with
`FILE_FLAG_OVERLAPPED`. Each read and write owns an independent event and
operation; a pending read must not serialize the reverse-direction write.
Cancellation drains the operation before freeing its storage. The desktop
accept loop also uses cancellable overlapped I/O. A per-connection stop event
prevents a queued write from starting after a disconnect has cancelled older
operations. Handles close only after the relevant operations finish.

Activation sends enter a bounded queue (64 messages / 4 MiB), so the UI does
not perform pipe writes. One worker delivers queued messages with a two-second
write deadline and returns failures through the UI event queue. A failed peer
is disconnected without terminating other browser connections. Enqueue success
means accepted for delivery; browser activation errors are still reported by
the extension. Browser stdin/stdout remain synchronous, separate handles; the
bridge separates browser lifetime cancellation from each desktop pipe session.
Output status and relayed frames are serialized to avoid corrupting stdout.

Before sending an activation while Glint still owns the foreground, the UI
delegates foreground permission to the browser process for that connection with
`AllowSetForegroundWindow`. Target discovery runs on the pipe accept worker:
the real pipe client's PID must identify the same Glint EXE or an installed
bridge whose full path and content match its hash directory, with a Chrome/Edge
parent directly or through a system cmd wrapper. Process paths, parent links,
creation times and live process handles prevent unrelated or reused PIDs from
being selected. The UI does no process enumeration or IPC wait for this step.
After delegating permission, the UI hides the search window before queuing the
activation. Hiding a foreground window can activate the window beneath it;
queuing first would let that fallback race the browser's target activation.
An immediate queue failure restores search before showing the error.
The extension then selects the tab and uses its current window ID. It requires
four consecutive focus samples 50 ms apart before confirming tab activation.
If initial focus is lost during that handoff, it retries focus at most twice,
within twelve polls (at most 550 ms of explicit waits). The checks stop when
the desktop connection changes. Windows may still refuse foreground activation;
an API call resolving or a single focused sample is insufficient.

Tab removal updates the in-memory index and posts a UI refresh immediately.
The extension handles `onReplaced` by retiring the old ID and adding the new
one, and drains queued close events even when a snapshot query fails.

Search keyboard navigation uses a small native-control helper: Up in the query
keeps input focus, while Down selects the first result. Up from the first result
returns to the query while preserving its text and caret. Empty lists retain
input focus; selecting a result also scrolls it into view. Home (`VK_HOME`) and
PageUp (`VK_PRIOR`) also return from the first result, covering Fn+Up when a
keyboard maps it to either key. Elsewhere in the list they retain native
jump-to-first/page-up behavior; in the query they retain native editing behavior.

An empty query uses the same native list-view in tile mode, with up to six
targets successfully activated from Glint selected after merging all browser
and desktop rows.
The normal width uses two columns; narrow viewports use one. Tab/Shift+Tab cycle
cards, arrows move through the grid, and Enter or a card click uses the existing
activation path. Navigation keeps the native EDIT ready for typing. Text input
from a focused result transfers to the EDIT before IME processing; composing
keys always take precedence over application shortcuts. A nonempty query uses
the report view and selects its first match without taking the editing caret.
Ctrl+F selects the query, F6 moves between search and connection controls, and
F2 expands/collapses the connection details. Each invocation clears the query.

The optional protocol `lastAccessed` field carries epoch milliseconds and
defaults to zero for older extensions. The extension writes a timestamp only
after a Glint activation succeeds; browser activation and window-focus events,
browser-provided access times, and metadata refreshes do not create visits.
Desktop discovery likewise only attaches timestamps recorded after a successful
Glint activation. This history lives in the current process.
Unknown times sort after known visits with a deterministic identity fallback.

Result refreshes select the first match for a changed query or a newly opened
search. Background updates preserve the selected connection/tab identity when
it remains in the filtered rows. Selection never takes keyboard focus from the
edit; an emptied list returns focus to the edit with its caret intact. The
native edit subclass observes IME composition and candidate notifications. The
message loop dispatches IME-owned keys directly to the control before processing
application shortcuts or dialog navigation.

The search window retains its native border and uses a 32-DIP client title bar
with a real, accessible close button inset 6 DIP from the right. The cross stays
dark when the window loses activation; hover/press use red, and high contrast
uses system colors. Empty caption areas return HTCAPTION for native dragging.
Alt+Space and caption right-click open the system menu explicitly; its Close
command, Alt+F4, and the button all retain the hide-to-tray behavior.

Global shortcuts are recorded in a dedicated native settings window and stored
as one `HotkeyBinding` DWORD (high 16 bits modifiers, low 16 bits virtual key).
Legacy `Hotkey` preset indices remain readable. A candidate registers under the
alternate ID before persistence and retirement of the old ID; a conflict or
failed save leaves the previous binding live. Registration uses MOD_NOREPEAT
without persisting it. The recorder handles Alt system messages and forwards
the current registered shortcut's WM_HOTKEY notification into the field rather
than reopening search. Obsolete shortcut messages are rejected by ID and binding.

Chrome and Edge tab results are supplied by the MV3 extension. Desktop windows
are indexed directly by the desktop process, with Notepad tabs added only under
the UI Automation condition above. Registration writes HKCU Native Messaging
host keys for the requested browser or browsers and generates host manifests
with explicit allowed origins.
All runtime dependencies are Windows system DLLs; Release uses `/MT`.

## Browser connection guide

The search window normally shows a compact one-line connection status bar and
a secondary **Connect browser** action. Clicking the status or pressing F2
reveals the browser connection table, with one row per
profile named `chrome-{label}` or `edge-{label}`, its state and current tab count.
Unnamed profiles receive a stable number for the current desktop session.
Counts include only synchronized profiles. Profiles seen during this session
remain visible as disconnected after their browser closes, without stale tab
counts; reconnects replace the same browser/profile row rather than duplicating it.
Normal startup opens recent cards with connection details collapsed. The **连接新的浏览器** action opens the guide;
`--setup` and the tray action remain direct entry points.

The native guide has five illustrated stages: choose one browser, open its
extensions page, enable developer mode, load the extension folder, and confirm
that the selected browser sent a full snapshot. Each page has one task and a
vector illustration with browser-specific control positions. The
extension has a checked-in public key, so its public-key ID is stable across
directories. Preparing the second stage extracts the five extension runtime files
and four icon assets to
`%LOCALAPPDATA%\Glint\browser-extension` and
registers only the selected browser with two explicit `allowed_origins`: the
public-key ID and the legacy unpacked ID derived from the stable
`%LOCALAPPDATA%\Glint\extension` directory. The latter supports older
installations whose identity was assigned from that path. Registration does
not scan browser profiles or accept arbitrary origins or wildcards. Advanced
and command-line registration instead allow only the single extension ID
supplied for each browser.

The legacy `Glint\extension` directory is no longer overwritten. Earlier builds
changed its manifest from a path-derived identity to the public-key identity.
Chromium can reject an unpacked path at startup if a saved installation ID no
longer matches its manifest. This mechanism matches the reported disappearing
Chrome card, but the local browser's saved records have not been inspected to
prove that cause. Loading the new permanent directory once in each browser
isolates those old records. The public key and new path then remain stable.

The toolbar action opens the connection page as a popup. Installation, update
and browser startup connect in the service worker without opening a tab; closing
the popup does not end the Native Messaging connection. The same page remains
available through the extension's options entry for detailed troubleshooting.

Chrome 155 filters privileged internal URLs such as `chrome://extensions/`
supplied by external command lines (Chromium's
`startup::ValidateLaunchUrlWebUnsafe`). The guide therefore opens the selected
browser at `about:blank`, copies the management-page address, and asks the user
to paste it in the browser address bar. A separate copy action supplies the
extension directory to the folder chooser. Browser launch success is never
reported as extension installation or connection success.

The folder path points to the directory containing `manifest.json`. The guide
asks users to paste the full path into the chooser's bottom Folder input and
confirm directly, avoiding address-bar navigation and ambiguity about whether
the current folder or a selected child will be loaded.

The final stage counts only synchronized connections from the selected
browser. A hello message alone, or a snapshot from the other browser, does not
enable search. Connected confirmation shows the successful state, counts,
search and add-browser actions. Disconnected confirmation shows **未连接** and
retry/troubleshooting actions. **连接遇到问题** opens a separate repair page
that accepts either the actual 32-character ID or the full
`chrome-extension://…/options.html` URL. Saving
replaces only the selected browser's registration with that single origin and
asks the user to retry in the extension; registration success alone still does
not enable search. A synchronized snapshot received during repair returns to
the success page. Back navigation preserves automatic/manual flow; disconnect
changes the primary action back to checking the connection. Reopening a hidden
guide starts at browser selection; showing an already open guide preserves its
current step. The native
regression suite exercises those paths in an isolated HKCU hive and temporary
LOCALAPPDATA directory, and checks controls and text at 100/150/200% DPI.
