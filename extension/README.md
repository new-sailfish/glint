# Glint 浏览器扩展

Chrome 和 Edge 共用这个 Manifest V3 扩展。扩展只读取标签页元数据（ID、窗口 ID、标题、网址和激活状态），不会注入脚本或读取网页正文。每个需要搜索的浏览器配置文件都必须安装并启用扩展。

## 安装和连接

只需下载并运行单个 `Glint.exe`，在主窗口点击**连接新的浏览器**，按五步图示引导操作。主窗口状态表显示各个浏览器配置文件的连接状态和标签页数量。扩展的 5 个运行文件、4 个图标资源和 3 个语言资源已内嵌在 EXE 中，Glint 自动释放文件并注册内置扩展身份，默认无需复制或填写扩展 ID。无需另外下载扩展文件夹；完整 ZIP 是额外包含文档和扩展源码的可选包。

1. **选择浏览器。** 选择 Chrome 或 Edge，点击**下一步：打开扩展页**。Glint 会把内置扩展释放到 `%LOCALAPPDATA%\Glint\browser-extension`，并为所选浏览器注册当前用户的连接信息。无需管理员权限。
2. **打开扩展管理页。** 点击**打开浏览器并复制页面地址**。在浏览器的头像菜单选择要接入的配置文件（例如工作或个人），然后在该配置文件的窗口中按 `Ctrl+L` 选中地址栏，按 `Ctrl+V` 粘贴页面地址，再按 `Enter` 进入扩展管理页：Chrome 为 `chrome://extensions/`，Edge 为 `edge://extensions/`。
3. **开启开发者模式。** Chrome 开启页面右上角的**开发者模式**（Developer mode）；Edge 开启页面左侧的**开发人员模式**（Developer mode）。回到 Glint，继续下一步。
4. **加载 Glint 扩展。** 点击**复制扩展文件夹路径**，再回到浏览器。Chrome 点击**加载已解压的扩展程序**（Load unpacked）；Edge 点击**加载解压缩的扩展**（Load unpacked）。

   在文件夹选择窗口，点击底部的**文件夹**（Folder）输入框，按 `Ctrl+V` 粘贴完整路径，直接点击**选择文件夹**（Select Folder），无需先进入该目录。路径末尾应为 `Glint\browser-extension`，其中包含 `manifest.json`；不要选择上一级 `Glint` 目录。

   如果已经进入 `browser-extension`，而窗口无法确认当前目录，请返回上一级，单击选中 `browser-extension` 文件夹，再点击**选择文件夹**。
5. **确认连接。** 回到 Glint，点击**已加载，检查连接**。页面显示**已连接**后，点击**开始搜索**。需要接入另一个浏览器时，点击**连接新的浏览器**。显示**未连接**时，可点击**连接遇到问题**进入独立修复页。以后按默认热键 `Ctrl+Shift+Space` 呼出搜索框，输入标题或网址，按回车切换标签页。

按钮会打开所选浏览器，扩展管理页需要按上面的步骤粘贴地址进入。如果浏览器无法自动打开，请自行启动对应浏览器，在目标配置文件的地址栏输入 `chrome://extensions/` 或 `edge://extensions/`。**页面地址**用于浏览器地址栏，**文件夹路径**用于加载扩展时的文件夹选择窗口；请按顺序分别复制。

Chrome 和 Edge 要求用户在每个需要搜索的配置文件中确认加载未打包扩展，普通桌面程序无法代为静默安装。Glint 把内嵌资源释放到稳定目录，但浏览器仍从该目录加载扩展，请保留该目录。未来商店版可提供相应的商店安装入口；当前版本使用向导准备目录，再由用户加载。

首次加载不会强制打开新的浏览器标签页，直接回到 Glint 完成连接检查即可。点击工具栏中的 Glint 图标，会显示连接状态面板，可**重试连接**或设置配置文件名称。完整页面仍可从扩展详情页的**扩展程序选项**打开。

多个浏览器配置文件需要分别加载扩展。可在扩展页展开**配置文件名称**，设置“工作”“个人”等名称，方便区分搜索结果来源。扩展不能自动获取浏览器配置文件的显示名称。自行构建时，先按项目根目录 README 生成 EXE；也可以手动加载安装包或仓库中的 `extension` 文件夹。

再次查看连接状态：点击浏览器工具栏里的 Glint 图标（如果未固定，可先打开浏览器的扩展菜单）。需要完整页面或复制选项页地址时，从扩展详情页进入**扩展程序选项**。安装、浏览器启动和扩展更新都不会自动打开引导标签页。

## 旧版升级与其他版本

**升级到 0.1.6：** 桌面端与扩展统一为明亮蓝视觉和 Glint 图标。退出旧版后启动新版 `Glint.exe`，再在每个浏览器的扩展管理页对已有 Glint 卡片点击一次**重新加载**。扩展显示的 `Ctrl+Shift+Space` 已标明为默认快捷键；修改后请使用桌面端设置的组合键。

**升级到 0.1.5：** 从托盘退出旧版 Glint 后启动新版 `Glint.exe`，再在每个浏览器的扩展管理页对已有 Glint 卡片点击一次**重新加载**。本次修复目标窗口短暂前置后又被其他窗口盖住的问题；切换会检查焦点保持稳定，并在交接期间丢失焦点时有限重试。扩展 ID 和现有加载路径不变。

**升级到 0.1.4：** 启动新版 `Glint.exe` 后，在每个浏览器的扩展管理页对已有 Glint 卡片点击一次**重新加载**。无需重装或修改 ID；此后 Glint 启停会自动恢复连接。状态面板只有收到桌面确认才显示绿色，旧后台或缺少状态字段时会提示重新加载。

内置扩展在 Chrome 和 Edge 中的公钥 ID 相同：`pecljnjkgandbbelbajkppllibpehhpe`。`manifest.json` 中的 RSA 公钥用于固定身份，扩展目录改变不会导致该公钥 ID 改变；这里不包含签名私钥。自动注册的 `allowed_origins` 只包含两个明确的扩展来源：该公钥 ID，以及旧目录 `%LOCALAPPDATA%\Glint\extension` 对应的旧版路径 ID。后者兼容已按该目录获得身份的旧版未打包扩展，不使用通配符，也不接纳任意 ID。

扩展自 0.1.1 起释放到新目录 `%LOCALAPPDATA%\Glint\browser-extension`。重新运行连接向导，并在每个浏览器配置文件中从新目录**加载已解压的扩展程序**一次。Chrome 中 Glint 卡片已消失时，直接加载新目录；Edge 如需新的工具栏状态面板，也要加载新目录一次。同一固定公钥 ID 的现有扩展会更新加载路径，无需先移除。Glint 保留旧 `%LOCALAPPDATA%\Glint\extension` 目录，不覆盖、不删除。

Chrome 加载后正常、重启后消失的现象，强烈支持同一路径的旧路径 ID 记录与新公钥 ID 冲突这一解释；尚未读取浏览器配置文件记录确证。新目录使当前扩展使用独立加载路径。若无公钥的旧版扩展仍可见，可选择移除；移除可能清除已设的配置文件名称。若旧扩展来自其他目录，仍可在向导的修复页粘贴其实际扩展地址来注册。

连接一直等待或使用其他 ID 的扩展时，可复制当前扩展选项页的完整地址（`chrome-extension://…/options.html`），或点击扩展页的**复制 ID**。在 Glint 向导的确认连接页面点击**连接遇到问题**，将地址粘贴到修复输入框，点击**保存 ID 并重试**，再回扩展页点击**重试连接**。无需手动截取地址中的 ID。手动注册只允许所填写的单个 ID；收到真实标签页前仍会保持等待状态。

## 连接状态与排查

- **已连接 Glint**（绿色）：通信桥已确认连接到桌面程序。请回到 Glint 搜索当前浏览器的标签页，确认同步结果；端口打开本身不会显示绿色。
- **扩展需要重新加载**：状态面板与后台版本不一致，或后台缺少桌面连接状态。请在当前浏览器的扩展管理页找到 Glint 卡片，点击**重新加载**，再打开面板。
- **Glint 未运行，等待自动连接**：浏览器通信桥仍在等待桌面程序。启动 `Glint.exe` 后会自动连接并重新同步标签页，无需点击重试或重新加载扩展。退出桌面程序不会终止浏览器管理的 `GlintBridge.exe`；它在本机等待下一次启动，浏览器关闭后退出，不会自行启动 Glint 桌面窗口。
- **尚未建立桥接通道**：在 Glint 向导中选择当前浏览器以完成注册。页面会显示浏览器提供的原始错误。修复后点击**重试连接**，也可以等待自动重连（最多间隔 30 秒）。旧版升级请先按上一节从新目录加载扩展。
- 更换 `Glint.exe` 的位置后，重新运行连接向导以更新通信桥注册。注册指向当前用户应用目录中的 `GlintBridge.exe`，不直接指向桌面 EXE；内置扩展目录保持在 `%LOCALAPPDATA%\Glint\browser-extension`。
- 只接入了一个配置文件时，Glint 无法看到其他尚未安装扩展的配置文件中的标签页。
- 注册操作只写当前 Windows 用户的 HKCU 项，无需管理员权限。命令行注册和注销方式见[使用指南](../docs/user-guide.md)。

当前扩展申请 `tabs`、`nativeMessaging`、`storage` 权限；`storage` 只保存配置文件身份、可选标签和语言选择。页面不加载外部脚本、字体或其他依赖。

## 界面语言 / Interface language

扩展面板支持 English 和中文，语言统一跟随 Glint 桌面端。在桌面端语言菜单中选择**跟随系统**、**English** 或**中文**后，已连接扩展的面板和工具栏提示立即更新。首次连接桌面端之前，浏览器界面语言为中文时使用中文，其它语言使用英文；连接后会缓存桌面端最后传来的语言，离线或浏览器重启后继续使用。浏览器扩展管理页的扩展名称和说明仍由浏览器按其界面语言选择，浏览器不允许桌面端修改该显示语言。

The panel supports English and Chinese and follows the Glint desktop app. Choose **Follow system**, **English**, or **中文** in the desktop language menu to update connected extension panels and toolbar tooltips immediately. Before the first desktop connection, a Chinese browser UI selects Chinese and other languages select English. After connecting, the extension caches the last desktop language for offline use and browser restarts. The browser still controls the extension name and description on its extensions page; the desktop app cannot change that display language.

## Native Messaging protocol

The service worker opens a persistent `runtime.connectNative("com.glint.host")` connection to the browser-managed `GlintBridge.exe`. Chrome/Edge 105 or newer is required for Native Messaging to keep the MV3 worker alive. All messages are JSON objects with `version: 1`; the browser's native-messaging framing is supplied by the browser and host bridge.

Bridge → extension control messages:

- `desktop_status` with `state: "waiting"`: keep the existing native port open, send no tab metadata, and wait for Glint to start. The bridge retries only the local named pipe, with a bounded delay; it does not relaunch the desktop or repeatedly start native processes.
- `desktop_status` with `state: "connected", generation: N`: a desktop pipe is connected. The extension sends a new `hello` and complete snapshot, including when the same native port reconnects after a desktop restart. Only this acknowledgement resets native-port reconnect backoff.
- `language_settings` with `version: 1`, `preference: "system"|"en"|"zh-CN"`, and `language: "en"|"zh-CN"`: sent after each `hello` and whenever desktop language settings change. The extension accepts it only while the desktop is connected and only when fields are valid; an explicit preference must match the resolved language. The resolved language updates live panels and the toolbar immediately, then is cached with its preference as `storage.local.desktopLanguage`. Cache writes are serialized, and a failed write never blocks tab synchronization. A new connection refreshes the cached choice.

Every extension → bridge message includes `desktopGeneration: N`, copied from the latest connected status. The bridge drops offline or mismatched messages instead of forwarding buffered metadata to a new desktop session. Asynchronous tab reads, snapshots, queued events and activation requests are also isolated by the extension's connection epoch.

Browser → desktop:

- `hello`: `{type, version, sessionId, browser: "chrome"|"edge", profileId, profileLabel?}`. `sessionId` is unique to one native connection; `profileId` is generated once and stored in `storage.local`; it is intentionally independent from Chrome/Edge's private profile name.
- `snapshot`: `{type, version, tabs: [{tabId, windowId, title, url, active}]}` after every initial connection/reconnection.
- `upsert`: `{type, version, tab: {tabId, windowId, title, url, active}}` for create/update/activate/attach/detach events.
- `remove`: `{type, version, tabId}` when a tab closes or can no longer be read.

Desktop → browser:

- `activate`: `{type, version, requestId, tabId, windowId}`. The connector activates the tab first, uses its current window ID (which may differ after a move), then focuses that window. A minimized window is restored to `normal`; an already maximized window keeps its state.

Browser → desktop activation result:

- `activate_result`: `{type, version, requestId, ok, error?, errorCode?}`. Success requires a focused browser window and an active tab in that window. Focus is checked at most 12 times, with 50 ms between checks, and must remain stable for four consecutive samples. Up to two focus retries are allowed; a resolved API promise alone is insufficient. Closed tabs/windows and refused foreground focus are reported as failures. Controlled failures carry a stable `errorCode` (`invalid_tab_id`, `activation_failed`, `invalid_window_id`, `window_not_focused`, or `activation_superseded`) so the desktop can show its selected language. Browser-provided errors retain their original diagnostic text without an invented code.

During a snapshot, tab events are queued. Once the snapshot is sent, each queued event re-reads the current tab before replaying it. This prevents a stale `tabs.query` result from overwriting a title, URL, window move or active state that changed during synchronization.

Close events are replayed even when the snapshot query fails. `tabs.onReplaced`
retires the old tab ID before indexing its replacement, so closing that
replacement cannot leave the old ID in search results. Normal closes update
the desktop index immediately over the existing connection; no reconnect is
needed. Another open tab with the same URL remains a separate result.

A dropped native connection, such as an unexpectedly terminated bridge, is retried with exponential backoff (500 ms through 30 s). A native port opening alone does not send metadata or reset that backoff. Normal desktop exit leaves the existing bridge waiting; desktop restart automatically sends a new `hello` and complete snapshot after the next connected acknowledgement. The index is memory-only and is rebuilt after restart. If the first snapshot query fails, the extension rebuilds its native connection and retries rather than staying unsynchronized.

## Tests

From the repository root:

```powershell
npm --prefix extension test
```

The tests use Node's built-in test runner and cover metadata normalization, snapshot/event race handling, minimized-window activation, bounded reconnect backoff, stable profile IDs, startup behavior, the toolbar status popup, status semantics, retry behavior, ID copying, profile-label save failure, and the bundled public key's derived ID matching the C++ host constant. Lifecycle cases cover waiting without spawning another native process, automatic desktop reconnection on the same port, generation filtering, obsolete asynchronous reads and queued events, failed initial snapshots, and stopping an obsolete activation before it can focus a window after desktop restart.

## Host manifest templates

`host-manifest.chrome.json` and `host-manifest.edge.json` are single-origin templates for the same native host name. For manual use, replace the template's executable path and extension ID before registering it under the current user's Native Messaging registry key. Advanced and CLI registration likewise allow only the supplied extension ID for each browser.

The guide's automatic flow prepares the per-user `GlintBridge.exe`, registers only the selected browser and writes that bridge path with two explicit `allowed_origins`: the bundled public-key ID and the legacy path-derived ID of the former `%LOCALAPPDATA%\Glint\extension` directory. This permits the bundled extension and older unpacked installations at that directory without allowing arbitrary origins or wildcards. The desktop and bridge use the same built application code under separate executable names; the browser owns the bridge's lifetime.

Keep protocol stdout reserved for Native Messaging frames in the host executable; diagnostics belong on stderr or the desktop log.

