# Glint 浏览器扩展

Chrome 和 Edge 共用这个 Manifest V3 扩展。扩展只读取标签页元数据（ID、窗口 ID、标题、网址和激活状态），不会注入脚本或读取网页正文。每个需要搜索的浏览器配置文件都必须安装并启用扩展。

## 安装和连接

1. 将 Glint 安装包解压到准备长期保留的位置，运行 `Glint.exe`。自行构建时，请先按项目根目录 README 生成 EXE。保留 `extension` 文件夹，浏览器需要从该位置持续读取扩展文件。
2. 在需要接入的浏览器配置文件中打开扩展管理页面：Chrome 为 `chrome://extensions`，Edge 为 `edge://extensions`。
3. 开启**开发者模式**，选择**加载已解压的扩展程序**（也可能显示为“加载解压缩的扩展”或 “Load unpacked”），选中这个 `extension` 文件夹。不要选择上一级目录。
4. 首次安装会自动打开中文引导页。点击**复制 ID**，回到桌面端 Glint 的**连接浏览器**窗口，将 ID 粘贴到对应的 Chrome / Edge 输入框，点击**保存并注册**。以后可从 Glint 托盘菜单重新打开此窗口。
5. 回到扩展引导页点击**重试连接**，再按默认热键 `Ctrl+Shift+Space` 打开 Glint。空查询应列出此配置文件中的标签页；搜索标题或网址，选中结果并按回车，确认能切换到目标标签页。
6. 如果有多个浏览器配置文件，切换到每个需要搜索的配置文件重复以上操作。可在引导页展开**给这个配置文件起个名字**，设置“工作”“个人”等标签，方便在结果中区分来源。扩展不能自动获取浏览器配置文件的显示名称。

再次查看引导：点击浏览器工具栏里的 Glint 图标（如果未固定，可先打开浏览器的扩展菜单），或从扩展详情页进入**扩展程序选项**。浏览器和扩展更新不会反复自动弹出引导。

## 连接状态与排查

- **桥接通道已打开**仅表示扩展拥有 Native Messaging 端口。这个状态不能单独证明桌面端已经收到完整标签页索引；请在 Glint 搜索列表中确认。
- **尚未建立桥接通道**：确认 `Glint.exe` 正在运行，再检查当前扩展 ID 是否已注册到正确浏览器。页面会显示浏览器提供的原始错误。修复后点击**重试连接**，也可以等待自动重连（最多间隔 30 秒）。
- 程序路径改变、解压目录改变、或使用了扩展商店版本等不同 ID 的扩展时，重新执行**保存并注册**。
- 只接入了一个配置文件时，Glint 无法看到其他尚未安装扩展的配置文件中的标签页。
- 注册操作只写当前 Windows 用户的 HKCU 项，无需管理员权限。命令行注册和注销方式见项目根目录 README。

当前扩展申请 `tabs`、`nativeMessaging`、`storage` 权限；`storage` 只保存配置文件身份和可选标签。页面不加载外部脚本、字体或其他依赖。

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

The tests use Node's built-in test runner and cover metadata normalization, snapshot/event race handling, minimized-window activation, bounded reconnect backoff, stable profile IDs, install-only onboarding, toolbar entry, status semantics, retry behavior, ID copying and profile-label save failure.

## Host manifest templates

`host-manifest.chrome.json` and `host-manifest.edge.json` are templates for the same native host name. Replace the executable path and extension ID before registering them under the current user's Native Messaging registry keys. The desktop installer/registration helper should generate these files so a changed executable path or a release extension ID can be registered again without administrator rights.

Keep protocol stdout reserved for Native Messaging frames in the host executable; diagnostics belong on stderr or the desktop log.

