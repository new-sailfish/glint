/* The page stores only its language preference; no browser tab data is read. */
(function (root, factory) {
  'use strict';
  const api = factory();
  if (typeof module === 'object' && module.exports) module.exports = api;
  else root.GlintSiteI18n = api;
})(typeof globalThis !== 'undefined' ? globalThis : this, function () {
  'use strict';

  const storageKey = 'glint.site.language';
  const preferences = ['system', 'en', 'zh-CN'];
  // Source phrases stay alongside the Chinese HTML. Text nodes are translated
  // individually so emphasis, links, keyboard hints and listeners remain intact.
  const english = Object.freeze({
    'Glint 是 Windows 上的 Chrome 与 Edge 标签页搜索工具。在一个搜索窗口中查找不同浏览器、窗口和配置文件中已打开的标签页，并直接切换过去。': 'Glint searches Chrome and Edge tabs on Windows. Find open tabs across browsers, windows and profiles in one search window, then switch straight to them.',
    'Glint · 跨浏览器查找已打开的标签页': 'Glint · Find open tabs across browsers',
    '面向 Windows 的轻量标签页搜索工具。连接 Chrome 和 Edge，按下快捷键，搜索并切换到目标标签页。': 'A lightweight tab search tool for Windows. Connect Chrome and Edge, press a shortcut, and search and switch to the tab you need.',
    'Glint 的 Windows 搜索窗口，使用示例标签页展示搜索结果': 'Glint search on Windows, showing search results with sample tabs',
    '跳至主要内容': 'Skip to main content',
    'Glint 首页': 'Glint home',
    '主要导航': 'Main navigation',
    '搜索演示': 'Search demo',
    '连接浏览器': 'Connect a browser',
    '下载': 'Download',
    '语言': 'Language',
    '跟随系统': 'Follow system',
    'WINDOWS 标签页搜索工具': 'TAB SEARCH FOR WINDOWS',
    '跨浏览器查找': 'Find open tabs',
    '已打开的标签页': 'across browsers',
    '在一个窗口中搜索已连接的 Chrome 和 Edge 标签页。输入标题或网址关键词，直接切换到目标页面。': 'Search your connected Chrome and Edge tabs in one window. Type a title or URL keyword to switch straight to the page you need.',
    '下载 Glint.exe': 'Download Glint.exe',
    '体验搜索演示': 'Try the search demo',
    'Windows 10 / 11 · x64 · 单个 EXE': 'Windows 10 / 11 · x64 · Single EXE',
    '默认唤起快捷键': 'Default search shortcut',
    'Control 加 Shift 加空格': 'Control plus Shift plus Space',
    'Glint 搜索窗口': 'Glint search window',
    'Glint 实际搜索界面：搜索框下方统一列出 Chrome 和 Edge 的示例标签页，并显示所属浏览器与配置文件。': 'The Glint search window lists sample Chrome and Edge tabs with their browser and profile.',
    '原生界面渲染，示例数据': 'Native interface · Sample data',
    '标签页分散在': 'Tabs spread across',
    '不同窗口与配置文件中': 'windows and profiles',
    '工作文档、参考资料和日常页面往往同时打开在多个位置。标签数量增加后，仅靠标签栏和窗口预览，很难迅速定位需要的页面。': 'Work documents, reference material and everyday pages often live in different places. As tabs accumulate, tab bars and window previews make the page you need harder to find.',
    'Glint 按标题或网址查找已连接的标签页，让你从搜索结果直接回到页面。': 'Glint finds connected tabs by title or URL so you can return to a page directly from the results.',
    '示意图：Chrome 和 Edge 的多个窗口、工作与个人配置文件中分散着许多标签页。': 'Illustration of tabs spread across Chrome and Edge windows and work and personal profiles.',
    '多窗口、多配置文件场景示意': 'Multiple windows and profiles · Illustration',
    '统一搜索，直接切换': 'One search. Straight to your tab.',
    '按下快捷键，输入关键词，再按 Enter。': 'Press the shortcut, type a keyword, then press Enter.',
    '目标窗口即使已最小化，也能被还原并切换到前台。': 'Even a minimized window can be restored and brought to the front.',
    '随时唤起': 'Open search anytime',
    '默认使用': 'Use ',
    '，也可从托盘菜单配置自己的全局热键。': ' by default, or set your own global hotkey from the tray menu.',
    '输入标题或网址': 'Type a title or URL',
    '搜索已连接的 Chrome 和 Edge 配置文件，无需先找到浏览器窗口。': 'Search connected Chrome and Edge profiles without finding the browser window first.',
    '选择后直接切换': 'Select and switch',
    '用方向键选择结果，按 Enter 回到标签页。实际应用中，Esc 隐藏搜索窗口。': 'Choose a result with the arrow keys and press Enter to return to the tab. In the app, Esc hides the search window.',
    '查看使用说明': 'Read the user guide',
    'Glint 搜索演示': 'Glint search demo',
    '示例数据': 'Sample data',
    '搜索示例标签页': 'Search sample tabs',
    '试试“设计”或“github”': 'Try “design” or “github”',
    '清空搜索': 'Clear search',
    '6 个示例标签页': '6 sample tabs',
    '示例搜索结果': 'Sample search results',
    '产品设计评审 · 飞书文档': 'Product design review · Feishu Docs',
    'Chrome · 工作': 'Chrome · Work',
    '设计规范 · Figma': 'Design system · Figma',
    'Edge · 工作': 'Edge · Work',
    'Chrome · 个人': 'Chrome · Personal',
    '项目周报 · Notion': 'Weekly project update · Notion',
    '产品设计需求 · GitHub': 'Product design requirements · GitHub',
    '日历 · Outlook': 'Calendar · Outlook',
    'Edge · 个人': 'Edge · Personal',
    '没有匹配的示例标签页': 'No matching sample tabs',
    '试试“设计”“github”或“日历”，也可以清空搜索查看全部示例。': 'Try “design”, “github” or “calendar”, or clear the search to see all samples.',
    '选择': 'Select',
    '模拟切换': 'Simulate switching',
    '清空示例': 'Clear search',
    '此演示仅使用页面内的示例数据，不读取或操作你的浏览器标签页。': 'This demo uses sample data on this page. It does not read or control your browser tabs.',
    '启用 JavaScript 后可筛选以上示例。Glint 应用的搜索不依赖本网页。': 'Enable JavaScript to filter these samples. Search in the Glint app works independently of this page.',
    '按向导完成浏览器连接': 'Connect your browser, step by step',
    'Glint 已内置扩展文件。首次连接时，按向导在浏览器中加载扩展；每个需要搜索的配置文件都要设置一次。': 'Glint includes the extension files. Follow the setup guide to load the extension once for each browser profile you want to search.',
    '查看对应浏览器的步骤': 'Show instructions for your browser',
    '设置教程的浏览器': 'Browser for setup instructions',
    '以下展示 Chrome 步骤。使用 Edge 时，在向导中选择 Edge，进入 edge://extensions/，开启左侧栏的“开发人员模式”，再点击“加载解压缩的扩展”。其他操作相同。': 'These steps show Chrome. For Edge, select Edge in the wizard, open edge://extensions/, enable Developer mode in the sidebar, then click Load unpacked. The remaining steps are the same.',
    '连接步骤': 'Connection steps',
    '选择浏览器': 'Choose a browser',
    '打开扩展页面': 'Open extensions',
    '开启开发者模式': 'Enable Developer mode',
    '加载扩展文件夹': 'Load extension folder',
    '确认连接': 'Confirm connection',
    '步骤 1 / 5': 'Step 1 / 5',
    '运行 Glint，选择浏览器': 'Run Glint and choose a browser',
    '打开下载的 Glint.exe，点击': 'Open the downloaded Glint.exe and click ',
    '连接新的浏览器': 'Connect browser',
    '。在向导中选择': '. In the wizard, select',
    '，再点击': ', then click ',
    '下一步：打开扩展页': 'Next: extensions',
    '。': '.',
    '扩展文件会自动准备好，无需单独下载，也无需管理员权限。': 'The extension files are prepared automatically. No separate download or administrator access is needed.',
    'Glint 实际连接向导：选择 Chrome 或 Edge 浏览器。': 'Glint connection wizard with a choice of Chrome or Edge.',
    '原生界面渲染 · 选择浏览器，以 Chrome 为例': 'Native interface · Chrome selection',
    '步骤 2 / 5': 'Step 2 / 5',
    '进入目标配置文件的扩展页面': 'Open extensions in the right profile',
    '点击': 'Click ',
    '打开浏览器并复制页面地址': 'Open browser and copy address',
    '。通过浏览器头像切换到需要搜索的配置文件，在地址栏粘贴地址并按 Enter。': '. Use the browser profile icon to switch to the profile you want to search, paste the address into the address bar, and press Enter.',
    '若浏览器未自动打开，可以手动打开，再输入上面的地址。': 'If the browser does not open automatically, open it manually and enter the address above.',
    '操作示意：在浏览器地址栏输入扩展管理页面地址': 'Illustration: enter the extensions page address in the browser address bar',
    '在浏览器地址栏中输入': 'Enter in the browser address bar',
    '请先确认右上角头像对应的配置文件': 'First check the profile icon at the top right',
    '操作示意': 'Illustration',
    '步骤 3 / 5': 'Step 3 / 5',
    '在 Chrome 扩展页面右上角，开启“开发者模式”（Developer mode）。': 'Enable Developer mode at the top right of the Chrome extensions page.',
    '开启后，页面中会出现加载本地扩展的按钮。返回 Glint，继续下一步。': 'A button for loading a local extension will appear. Return to Glint to continue.',
    '此版本通过本地文件夹加载扩展，需要你在浏览器中完成确认。': 'This version loads the extension from a local folder. Confirm this action in your browser.',
    '操作示意：打开浏览器的开发者模式开关': 'Illustration: turn on Developer mode in the browser',
    '扩展管理页面': 'Extensions page',
    '开发者模式': 'Developer mode',
    '加载已解压的扩展程序': 'Load unpacked',
    '操作示意 · 浏览器界面可能因版本而异': 'Illustration · Browser appearance may vary by version',
    '步骤 4 / 5': 'Step 4 / 5',
    '选择扩展文件夹': 'Select the extension folder',
    '在 Glint 中点击': 'In Glint, click ',
    '复制扩展文件夹路径': 'Copy extension folder path',
    '，回到浏览器点击': ', return to the browser and click ',
    '（Load unpacked）。': '.',
    '在弹出的窗口底部，将路径粘贴到': 'At the bottom of the dialog, paste the path into the ',
    '文件夹': 'Folder',
    '输入框，直接点击': ' field, then click ',
    '选择文件夹': 'Select Folder',
    '路径应以': 'The path should end in',
    '结尾。请选择这个文件夹，而不是它的上一级。': '. Select this folder, not its parent folder.',
    'Glint 实际连接向导：复制扩展文件夹路径，并在浏览器中加载已解压的扩展。': 'Glint connection wizard: copy the extension folder path and load the unpacked extension in the browser.',
    '原生界面渲染 · 加载扩展，以 Chrome 为例': 'Native interface · Load the Chrome extension',
    '步骤 5 / 5': 'Step 5 / 5',
    '确认连接，开始搜索': 'Confirm the connection and search',
    '回到 Glint，点击': 'Return to Glint and click ',
    '已加载，检查连接': 'Loaded: check connection',
    '。收到浏览器的标签页后，向导会显示': '. Once the browser sends its tabs, the wizard shows ',
    '已连接': 'Connected',
    '和标签页数量，点击': ' and the tab count. Click ',
    '开始搜索': 'Start searching',
    '即可使用。': ' to begin.',
    '需要添加其他浏览器或配置文件时，再次点击“连接新的浏览器”，在对应配置文件中重复以上步骤。': 'To add another browser or profile, click “Connect browser” again and repeat these steps in that profile.',
    'Glint 实际连接成功界面：已接收浏览器标签页，显示连接状态与开始搜索按钮；使用示例标签页。': 'Glint confirms a successful connection with a tab count and a Start searching button; sample tabs are shown.',
    '原生界面渲染 · Chrome 连接成功，示例数据': 'Native interface · Chrome connected · Sample data',
    '上一步': 'Back',
    '下一步': 'Next',
    '连接后请保留扩展文件夹，浏览器仍需从该位置加载扩展。': 'Keep the extension folder after setup. Your browser continues to load the extension from that location. ',
    '完整安装与故障排查说明': 'Full installation and troubleshooting guide',
    '下载 Glint': 'Download Glint',
    '下载后运行 Glint.exe，': 'Download and run Glint.exe,',
    '按连接向导接入你的浏览器。': 'then follow the wizard to connect your browser.',
    'Chrome / Edge 105 或更新版本': 'Chrome / Edge 105 or later',
    '完整 ZIP 包': 'Complete ZIP package',
    '版本记录': 'Release notes',
    '常见问题': 'Frequently asked questions',
    '标签页数据会上传吗？': 'Is my tab data uploaded?',
    'Glint 在本机接收已连接配置文件的标签页标题、网址等信息，并在内存中建立搜索索引，不将这些数据上传到云端。本网页的搜索演示仅使用固定示例数据。': 'Glint receives tab titles, URLs and related information from connected profiles locally and builds a search index in memory. This data is not uploaded to the cloud. The demo on this page uses fixed sample data only.',
    '查看架构说明 ↗': 'Read about the architecture ↗',
    '为什么每个配置文件都要加载扩展？': 'Why does each profile need the extension?',
    'Chrome 和 Edge 的扩展按配置文件独立安装。只有加载并连接 Glint 扩展的配置文件，其标签页才会出现在搜索中。你可以在扩展面板中给配置文件设置“工作”或“个人”等本地名称。': 'Chrome and Edge install extensions separately for each profile. Only profiles with the Glint extension loaded and connected appear in search. You can give each profile a local name, such as “Work” or “Personal”, in the extension panel.',
    '重启浏览器或 Glint 后需要重新连接吗？': 'Do I need to reconnect after a restart?',
    '正常重启后会自动重新连接并同步标签页，无需再次加载扩展。更新 Glint 时，可能需要在浏览器扩展页面重新加载已有的扩展，具体以版本说明为准。': 'After a normal restart, the browser reconnects and syncs tabs automatically. You do not need to load the extension again. After updating Glint, you may need to reload the extension on the browser extensions page; check the release notes.',
    '需要哪些权限？': 'Which permissions are needed?',
    '扩展请求 tabs、nativeMessaging 和 storage 权限，分别用于读取标签页信息、连接本机 Glint 和保存扩展设置。首次配置需要手动开启开发者模式并加载扩展；本机注册不需要管理员权限。': 'The extension requests tabs, nativeMessaging and storage permissions to read tab information, connect to Glint on your computer and save extension settings. Initial setup requires enabling Developer mode and loading the extension manually. Local registration does not require administrator access.',
    '退出 Glint 后，后台进程会停止吗？': 'What happens when I quit Glint?',
    '从托盘菜单退出会停止桌面搜索程序。每个已连接配置文件的桥接进程会在后台等待 Glint 再次启动；关闭浏览器或停用扩展后，对应的桥接进程也会退出。': 'Quitting from the tray menu stops the desktop search app. A bridge process for each connected profile waits in the background for Glint to start again. Closing the browser or disabling its extension also stops that profile’s bridge process.',
    '可以修改唤起搜索的快捷键吗？': 'Can I change the search shortcut?',
    '可以。右键点击托盘图标，选择“配置全局热键”，录入组合键并保存。如果组合键已被占用，Glint 会提示冲突并保留原来的快捷键。': 'Yes. Right-click the tray icon, choose “Configure shortcut”, enter a key combination and save. If it is already in use, Glint reports the conflict and keeps your previous shortcut.',
    '查看热键设置实际界面 ↗': 'View hotkey settings ↗',
    'Windows 标签页搜索': 'Tab search for Windows',
    '使用说明': 'User guide',
    '反馈问题': 'Report an issue',
  });

  const messages = Object.freeze({
    en: {
      sampleCount: '{count} sample tabs',
      matchCount: '{count} matching sample tabs',
      activated: 'Demo: selected “{title}”. Glint would switch to that tab.',
      next: 'Next',
      lastStep: 'Last step',
      progress: 'Step {step} / {total}',
      chromeDeveloper: 'Enable Developer mode at the top right of the Chrome extensions page.',
      edgeDeveloper: 'Enable Developer mode in the sidebar of the Edge extensions page.',
      chromeMode: 'Developer mode',
      edgeMode: 'Developer mode',
      chromeLoad: 'Load unpacked',
      edgeLoad: 'Load unpacked',
    },
    'zh-CN': {
      sampleCount: '{count} 个示例标签页',
      matchCount: '{count} 个匹配的示例标签页',
      activated: '演示：已选择“{title}”。在 Glint 中，这一步会切换到对应标签页。',
      next: '下一步',
      lastStep: '已到最后一步',
      progress: '步骤 {step} / {total}',
      chromeDeveloper: '在 Chrome 扩展页面右上角，开启“开发者模式”（Developer mode）。',
      edgeDeveloper: '在 Edge 扩展页面的左侧栏中，开启“开发人员模式”（Developer mode）。',
      chromeMode: '开发者模式',
      edgeMode: '开发人员模式',
      chromeLoad: '加载已解压的扩展程序',
      edgeLoad: '加载解压缩的扩展',
    },
  });

  function normalizePreference(value) {
    return preferences.includes(value) ? value : 'system';
  }

  function resolveLanguage(preference, systemLanguage) {
    const normalized = normalizePreference(preference);
    if (normalized !== 'system') return normalized;
    return /^zh(?:[-_]|$)/i.test(systemLanguage || '') ? 'zh-CN' : 'en';
  }

  function readPreference(storage) {
    try { return normalizePreference(storage.getItem(storageKey)); }
    catch { return 'system'; }
  }

  function writePreference(storage, preference) {
    try {
      storage.setItem(storageKey, normalizePreference(preference));
      return true;
    } catch { return false; }
  }

  function translate(source, language) {
    const phrase = source.trim();
    if (language !== 'en' || !Object.hasOwn(english, phrase)) return source;
    return source.replace(phrase, () => english[phrase]);
  }

  function message(key, language, values = {}) {
    const template = messages[language]?.[key] ?? messages.en[key];
    if (template === undefined) throw new Error(`Unknown translation: ${key}`);
    return template.replace(/\{(\w+)\}/g, (_, name) => String(values[name] ?? `{${name}}`));
  }

  function imagePath(source, language) {
    return source.replace(/(assets\/(?:search|setup-browser|setup-load|setup-connected|hotkey|tab-overload))(?:-en)?(\.(?:png|svg))$/,
      (_, name, extension) => `${name}${language === 'en' ? '-en' : ''}${extension}`);
  }

  const pages = new WeakMap();
  function translatePage(document, language) {
    if (!pages.has(document)) {
      const text = [];
      const walker = document.createTreeWalker(document.documentElement, 4);
      while (walker.nextNode()) {
        const node = walker.currentNode;
        if (!node.parentElement?.closest('script, style, noscript')) text.push([node, node.nodeValue]);
      }
      const attributes = [];
      document.querySelectorAll('*').forEach(element => {
        ['aria-label', 'alt', 'title', 'placeholder', 'content', 'data-title', 'src', 'href'].forEach(name => {
          if (element.hasAttribute(name)) attributes.push([element, name, element.getAttribute(name)]);
        });
      });
      pages.set(document, { text, attributes });
    }
    const page = pages.get(document);
    page.text.forEach(([node, source]) => { node.nodeValue = translate(source, language); });
    page.attributes.forEach(([element, name, source]) => {
      const value = translate(source, language);
      element.setAttribute(name, ['src', 'href', 'content'].includes(name) ? imagePath(value, language) : value);
    });
    document.documentElement.lang = language;
    document.querySelector('meta[property="og:locale"]')?.setAttribute('content', language === 'en' ? 'en_US' : 'zh_CN');
  }

  return { storageKey, english, messages, normalizePreference, resolveLanguage, readPreference, writePreference, translate, message, imagePath, translatePage };
});
