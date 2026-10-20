# Glint 浏览器扩展

Chrome 和 Edge 共用这个 Manifest V3 扩展。扩展只读取标签页元数据（ID、窗口 ID、标题、网址和激活状态），不会注入脚本或读取网页正文。每个需要搜索的浏览器配置文件都必须安装并启用扩展。

## 安装和连接

运行 `Glint.exe`，按**连接浏览器**向导操作。内置扩展使用固定身份，默认无需复制或填写扩展 ID。

1. **选择浏览器。** 选择 Chrome 或 Edge，点击**下一步：安装扩展**。Glint 会把内置扩展释放到 `%LOCALAPPDATA%\Glint\extension`，并为所选浏览器注册当前用户的连接信息。无需管理员权限。
2. **打开扩展管理页并加载 Glint。** 点击**打开浏览器并复制页面地址**。在浏览器的头像菜单选择要接入的配置文件（例如工作或个人），然后在该配置文件的窗口中按 `Ctrl+L` 选中地址栏，按 `Ctrl+V` 粘贴页面地址，再按 `Enter` 进入扩展管理页：Chrome 为 `chrome://extensions/`，Edge 为 `edge://extensions/`。

   回到 Glint，点击**复制扩展文件夹路径**，再回到浏览器。

   **Chrome：** 开启页面右上角的**开发者模式**（Developer mode），点击**加载已解压的扩展程序**（Load unpacked）。**Edge：** 开启页面左侧的**开发人员模式**（Developer mode），点击**加载解压缩的扩展**（Load unpacked）。

   在文件夹选择窗口，按 `Alt+D` 选中地址栏，再按 `Ctrl+V` 粘贴刚复制的目录，按 `Enter` 进入该目录，最后点击**选择文件夹**（Select Folder）。请选择 `extension` 文件夹本身。
3. **确认连接。** 回到 Glint，点击**我已加载扩展，检查连接**。Glint 收到当前浏览器的标签页后，点击**开始搜索**。需要接入另一个浏览器时，点击**连接另一个浏览器**。以后按默认热键 `Ctrl+Shift+Space` 呼出搜索框，输入标题或网址，按回车切换标签页。

按钮会打开所选浏览器，扩展管理页需要按上面的步骤粘贴地址进入。如果浏览器无法自动打开，请自行启动对应浏览器，在目标配置文件的地址栏输入 `chrome://extensions/` 或 `edge://extensions/`。**页面地址**用于浏览器地址栏，**文件夹路径**用于加载扩展时的文件夹选择窗口；请按顺序分别复制。

Chrome 和 Edge 要求用户确认加载未打包扩展，普通桌面程序无法代为静默安装。首次加载后会自动打开扩展引导页；回到 Glint 完成检查即可。连接暂未建立时，可在扩展页点击**重试连接**。

多个浏览器配置文件需要分别加载扩展。可在扩展页展开**给这个配置文件起个名字**，设置“工作”“个人”等标签，方便区分搜索结果来源。扩展不能自动获取浏览器配置文件的显示名称。自行构建时，先按项目根目录 README 生成 EXE；也可以手动加载安装包或仓库中的 `extension` 文件夹。

再次查看引导：点击浏览器工具栏里的 Glint 图标（如果未固定，可先打开浏览器的扩展菜单），或从扩展详情页进入**扩展程序选项**。浏览器和扩展更新不会反复自动弹出引导。

## 旧版升级与其他版本

内置扩展在 Chrome 和 Edge 中使用相同的固定 ID：`pecljnjkgandbbelbajkppllibpehhpe`。`manifest.json` 中的 RSA 公钥用于固定身份，扩展目录改变不会导致该 ID 改变；这里不包含签名私钥。

从尚无固定 ID 的旧版升级后，请在浏览器扩展管理页**重新加载** Glint。如果 ID 仍与上述固定 ID 不同，请**移除旧版扩展**，再加载 Glint 新向导提供的扩展目录，然后为该浏览器重新完成连接向导。移除扩展可能会清除之前设置的配置文件标签。

使用商店版或其他 ID 的扩展时，可在扩展页展开**高级排查：旧版或其他版本扩展**，点击**复制 ID**。回到 Glint 的选择浏览器页面，选择对应浏览器，再点击**其他版本：手动填写扩展 ID**，填入 ID 并注册。普通内置扩展无需这一步。

## 连接状态与排查

- **桥接通道已打开**仅表示扩展拥有 Native Messaging 端口。这个状态不能单独证明桌面端已经收到完整标签页索引；Glint 向导收到所选浏览器的同步数据后才会显示连接成功。
- **尚未建立桥接通道**：确认 `Glint.exe` 正在运行，并在 Glint 向导中选择了当前浏览器。页面会显示浏览器提供的原始错误。修复后点击**重试连接**，也可以等待自动重连（最多间隔 30 秒）。旧版升级请先按上一节重新加载扩展。
- 更换 `Glint.exe` 的位置后，重新运行连接向导以更新程序路径。内置扩展目录保持在 `%LOCALAPPDATA%\Glint\extension`；更换 EXE 位置不会改变该目录。
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

The tests use Node's built-in test runner and cover metadata normalization, snapshot/event race handling, minimized-window activation, bounded reconnect backoff, stable profile IDs, install-only onboarding, toolbar entry, status semantics, retry behavior, ID copying, profile-label save failure, and the bundled public key's derived ID matching the C++ host constant.

## Host manifest templates

`host-manifest.chrome.json` and `host-manifest.edge.json` are templates for the same native host name. Glint's guide generates these manifests with the current executable path and the bundled extension's fixed ID, registering only the selected browser. Each generated manifest allows exactly one extension origin. For manual use, replace the template's executable path and extension ID before registering it under the current user's Native Messaging registry key. Other extension IDs remain supported through the desktop guide's advanced fields and the CLI.

Keep protocol stdout reserved for Native Messaging frames in the host executable; diagnostics belong on stderr or the desktop log.

