/* Shared tab index and Native Messaging bridge for the Glint MV3 extension. */
(function (root, factory) {
  const api = factory();
  if (typeof module === 'object' && module.exports) module.exports = api;
  root.GlintExtension = api;
})(typeof globalThis !== 'undefined' ? globalThis : this, function () {
  'use strict';

  const VERSION = 1;
  const DEFAULT_HOST = 'com.glint.host';
  const MIN_RECONNECT_MS = 500;
  const MAX_RECONNECT_MS = 30000;

  // Keep both the worker and popup in the five-file embedded extension bundle.
  const messages = {
    en: {
      pageTitle: 'Connection status · Glint', toolbarTitle: 'Glint · Connection status',
      tagline: 'Find your open tabs', browser: 'Browser', connection: 'Connection status',
      checking: 'Checking connection…', retrying: 'Retrying connection…', retry: 'Retry connection',
      waking: 'Waking the extension background service.', extensionId: 'Current extension ID', copyId: 'Copy ID',
      shortcutSection: 'Search shortcut', shortcutLabel: 'Default shortcut', showSearch: 'Open search',
      searchInstruction: 'Type a title or URL, then press Enter to switch tabs.',
      customShortcut: 'Changed the shortcut? Use the key combination configured in the desktop app.',
      profileHeading: 'Profile name', profileHelp: 'Label this profile “Work” or “Personal” to identify its search results.',
      profileLabel: 'Profile label', profilePlaceholder: 'For example: Work', saveLabel: 'Save label',
      helpHeading: 'Connection help',
      helpFirst: 'For your first connection, return to “Connect browser” in the Glint desktop app, select this browser, and check the connection. You can also open “Connect browser” from the Glint system tray menu.',
      helpUpgrade: 'When upgrading from an older version, run the desktop connection guide again and load the extension once from the new folder it provides. If you move Glint.exe, run the guide again to update the connection.',
      helpClose: 'Closing this panel does not disconnect the extension. It connects automatically in the background; this panel does not need to stay open.',
      privacy: 'Install the extension in each Chrome / Edge profile you want to search. It processes tab titles and URLs, without reading page contents.',
      languageFollowsDesktop: 'Language follows Glint. Change it in the desktop app’s language menu.',
      copySuccess: 'Extension ID copied. Return to Glint’s connection confirmation page, select “Connection help”, paste and save the ID, then retry here.',
      copyFailed: 'Could not copy. The ID is selected; press Ctrl+C to copy it.',
      waiting: 'Glint is not running; waiting to connect',
      waitingDetail: 'Start Glint to connect and sync tabs automatically. You do not need to reload the extension.',
      connected: 'Connected to Glint',
      connectedDetail: 'Return to Glint and search for tabs from this browser to confirm that synchronization is working.',
      reload: 'Reload the extension',
      reloadDetail: 'The status panel and background service versions do not match, so Glint’s connection cannot be confirmed. Find Glint on the {browser} extensions page, select “Reload”, then reopen this panel.',
      noBridge: 'Bridge connection not established',
      missingHost: 'The browser cannot find the Glint native messaging host. Return to Glint’s connection confirmation page, select “Connection help”, paste the extension ID below and save it, then retry here. If the error persists, check that you are using the same Windows user and that your browser allows Native Messaging registrations for the current user.',
      bridgeHelp: 'Make sure Glint is running and this browser is selected in the “Connect browser” guide, then select “Retry connection”. If it still fails, copy the extension ID below and paste it under “Connection help” on Glint’s connection confirmation page, then save it.',
      browserError: ' Browser error: {error}', connectionError: ' Connection error: {error}',
      bridgeUnavailable: 'The extension background service could not start. Reopen this panel or reload the extension.',
      readLabelFailed: 'Could not read the label. Try reopening this page.',
      saveFailed: 'Could not save: {error}', saved: 'Saved', savedLater: 'Saved; will apply on the next connection',
      nativeClosed: 'Native messaging connection closed.', invalidTab: 'Invalid tab ID.', invalidWindow: 'Invalid current window ID.',
      activationFailed: 'The tab could not be activated. Please try again.',
      focusFailed: 'The tab switched, but Windows did not focus its window. Try again or open the window from the taskbar.',
      tabChanged: 'The tab state changed. Search again and retry.',
    },
    'zh-CN': {
      pageTitle: '连接状态 · Glint', toolbarTitle: 'Glint · 连接状态',
      tagline: '标签页，随手找到', browser: '浏览器', connection: '连接状态',
      checking: '正在检查连接…', retrying: '正在重试连接…', retry: '重试连接',
      waking: '正在唤醒扩展后台服务。', extensionId: '当前扩展 ID', copyId: '复制 ID',
      shortcutSection: '搜索快捷键', shortcutLabel: '默认快捷键', showSearch: '呼出搜索',
      searchInstruction: '输入标题或网址，按 Enter 切换标签页。',
      customShortcut: '已修改快捷键？请使用桌面端设置的组合键。',
      profileHeading: '配置文件名称', profileHelp: '给当前配置文件设置“工作”“个人”等标签，方便区分搜索结果来源。',
      profileLabel: '配置文件标签', profilePlaceholder: '例如：工作', saveLabel: '保存标签',
      helpHeading: '连接帮助',
      helpFirst: '首次使用时，回到 Glint 桌面的“连接浏览器”向导，选择当前浏览器并检查连接。找不到向导时，从 Glint 系统托盘菜单打开“连接浏览器”。',
      helpUpgrade: '从旧版升级时，请重新运行桌面连接向导，在浏览器中从向导提供的新文件夹加载扩展一次。更换 EXE 位置后，也请重新运行向导以更新连接信息。',
      helpClose: '关闭这个面板不会断开连接。扩展会在浏览器后台自动连接，状态面板无需一直打开。',
      privacy: '每个需要搜索的 Chrome / Edge 配置文件都要安装扩展。仅处理标签页标题和网址，不读取网页正文。',
      languageFollowsDesktop: '语言跟随 Glint，请在桌面端的语言菜单中修改。',
      copySuccess: '扩展 ID 已复制。请回到 Glint 的确认连接页，点击“连接遇到问题”，粘贴并保存，再回来重试连接。',
      copyFailed: '复制失败，ID 已选中。请按 Ctrl+C 复制。',
      waiting: 'Glint 未运行，等待自动连接', waitingDetail: '启动 Glint 后会自动连接并同步标签页，无需重新加载扩展。',
      connected: '已连接 Glint', connectedDetail: '请回到 Glint，确认能搜索到当前浏览器的标签页，以验证同步结果。',
      reload: '扩展需要重新加载',
      reloadDetail: '状态面板与后台版本不一致，暂时无法确认 Glint 的连接状态。请在 {browser} 扩展管理页找到 Glint，点击“重新加载”，再打开此面板。',
      noBridge: '尚未建立桥接通道',
      missingHost: '浏览器找不到 Glint 本机通信桥。请返回 Glint 的确认连接页，点击“连接遇到问题”，粘贴下方扩展 ID 并保存，再回到此页重试。若仍出现同一错误，请检查是否使用同一 Windows 用户，以及浏览器是否允许当前用户的 Native Messaging 注册。',
      bridgeHelp: '请确认 Glint 正在运行，并在“连接新的浏览器”引导中选择了当前浏览器，然后点击“重试连接”。如果仍未连接，可复制下方扩展 ID，在 Glint 的确认连接页点击“连接遇到问题”，粘贴并保存。',
      browserError: ' 浏览器返回：{error}', connectionError: ' 连接错误：{error}',
      bridgeUnavailable: '扩展后台服务未能启动。请重新打开此面板，或重新加载扩展。',
      readLabelFailed: '读取标签失败，可重试打开此页面。',
      saveFailed: '保存失败：{error}', saved: '已保存', savedLater: '已保存，将在下次连接时生效',
      nativeClosed: '本机通信连接已关闭。', invalidTab: '标签页 ID 无效。', invalidWindow: '当前窗口 ID 无效。',
      activationFailed: '标签页未能激活，请重试。',
      focusFailed: '标签页已切换，但 Windows 未将目标窗口切到前台。请重试，或从任务栏打开该窗口。',
      tabChanged: '标签页状态已变化，请重新搜索后重试。',
    },
  };

  function normalizeLanguagePreference(value) {
    return value === 'en' || value === 'zh-CN' ? value : 'system';
  }

  function resolveLanguage(preference, systemLanguage) {
    const choice = normalizeLanguagePreference(preference);
    return choice !== 'system' ? choice : /^zh(?:[-_]|$)/i.test(systemLanguage || '') ? 'zh-CN' : 'en';
  }

  function validDesktopLanguage(settings) {
    return Boolean(settings && ['system', 'en', 'zh-CN'].includes(settings.preference) &&
      ['en', 'zh-CN'].includes(settings.language) &&
      (settings.preference === 'system' || settings.preference === settings.language));
  }

  function systemLanguage(api = globalThis.chrome) {
    try {
      if (api && api.i18n && api.i18n.getUILanguage) return api.i18n.getUILanguage();
    } catch (_) {}
    return globalThis.navigator && globalThis.navigator.language || 'en';
  }

  function translate(language, key, values = {}) {
    const text = (messages[language] || messages.en)[key] || messages.en[key] || key;
    return text.replace(/\{(\w+)\}/g, (placeholder, name) => values[name] === undefined ? placeholder : String(values[name]));
  }

  function makeProfileId() {
    try {
      if (globalThis.crypto && typeof globalThis.crypto.randomUUID === 'function') {
        return globalThis.crypto.randomUUID();
      }
    } catch (_) {}
    return `profile-${Date.now().toString(36)}-${Math.random().toString(36).slice(2)}`;
  }

  function normalizeTab(tab) {
    return {
      tabId: Number(tab.id),
      windowId: Number(tab.windowId),
      title: typeof tab.title === 'string' ? tab.title : '',
      url: typeof tab.url === 'string' ? tab.url : '',
      active: Boolean(tab.active),
      lastAccessed: typeof tab.lastAccessed === 'number' && Number.isFinite(tab.lastAccessed) &&
        tab.lastAccessed > 0 && tab.lastAccessed <= Number.MAX_SAFE_INTEGER ? Math.trunc(tab.lastAccessed) : 0,
    };
  }

  function browserNameFromUserAgent(userAgent) {
    return /Edg\//i.test(userAgent || '') ? 'edge' : 'chrome';
  }

  async function loadIdentity(storage) {
    const values = await storage.get(['profileId', 'profileLabel']);
    const profileId = values && typeof values.profileId === 'string' && values.profileId
      ? values.profileId : makeProfileId();
    if (!values || values.profileId !== profileId) await storage.set({profileId});
    return {
      profileId,
      profileLabel: values && typeof values.profileLabel === 'string' ? values.profileLabel.trim() : '',
    };
  }

  class TabBridge {
    constructor(options) {
      const opts = options || {};
      this.api = opts.api || globalThis.chrome || globalThis.browser;
      this.hostName = opts.hostName || DEFAULT_HOST;
      this.browser = opts.browser || browserNameFromUserAgent(globalThis.navigator && globalThis.navigator.userAgent);
      this.profileId = opts.profileId || makeProfileId();
      this.profileLabel = opts.profileLabel || '';
      this.language = resolveLanguage(opts.language, systemLanguage(this.api));
      this.onLanguageChanged = typeof opts.onLanguageChanged === 'function' ? opts.onLanguageChanged : null;
      this.sessionId = opts.sessionId || makeProfileId();
      this.schedule = opts.schedule || ((fn, ms) => setTimeout(fn, ms));
      this.clearSchedule = opts.clearSchedule || (id => clearTimeout(id));
      this.wait = opts.wait || (ms => new Promise(resolve => setTimeout(resolve, ms)));
      this.now = opts.now || (() => Date.now());
      this.logger = opts.logger || (() => {});
      this.port = null;
      this.connected = false;
      this.desktopConnected = false;
      this.desktopGeneration = null;
      this.connectionEpoch = 0;
      this.connecting = false;
      this.listenersAttached = false;
      this.syncing = false;
      this.pendingEvents = [];
      this.eventChain = Promise.resolve();
      this.syncQueue = Promise.resolve();
      this.index = new Map();
      this.accessTimes = new Map();
      this.focusedWindowId = null;
      this.reconnectAttempt = 0;
      this.reconnectTimer = null;
      this.lastError = '';
      this.lastErrorCode = '';
    }

    attachListeners() {
      if (this.listenersAttached || !this.api || !this.api.tabs) return;
      const events = this.api.tabs;
      events.onCreated && events.onCreated.addListener(tab => this.queueEvent({kind: 'upsert', tabId: tab.id}));
      events.onUpdated && events.onUpdated.addListener(tabId => this.queueEvent({kind: 'upsert', tabId}));
      events.onActivated && events.onActivated.addListener(info => {
        this.rememberAccess(info.tabId, this.now());
        this.queueEvent({kind: 'upsert', tabId: info.tabId});
      });
      events.onAttached && events.onAttached.addListener(tabId => this.queueEvent({kind: 'upsert', tabId}));
      // A detached tab can still exist in another window. Reading it avoids a transient false removal.
      events.onDetached && events.onDetached.addListener(tabId => this.queueEvent({kind: 'upsert', tabId}));
      events.onRemoved && events.onRemoved.addListener(tabId => {
        this.accessTimes.delete(Number(tabId));
        this.queueEvent({kind: 'remove', tabId});
      });
      // Prerendering can replace a tab ID without firing onRemoved for the old ID.
      events.onReplaced && events.onReplaced.addListener((addedTabId, removedTabId) => {
        this.accessTimes.delete(Number(removedTabId));
        this.queueEvent({kind: 'remove', tabId: removedTabId});
        this.queueEvent({kind: 'upsert', tabId: addedTabId});
      });
      const windows = this.api.windows;
      windows && windows.onFocusChanged && windows.onFocusChanged.addListener(
        windowId => this.observeWindowFocus(windowId));
      this.listenersAttached = true;
    }

    rememberAccess(tabId, timestamp) {
      if (!Number.isSafeInteger(Number(tabId)) || Number(tabId) < 0 ||
          !Number.isSafeInteger(timestamp) || timestamp <= 0) return;
      const id = Number(tabId);
      this.accessTimes.set(id, Math.max(timestamp, this.accessTimes.get(id) || 0));
    }

    normalizeCurrentTab(tab) {
      const normalized = normalizeTab(tab);
      this.rememberAccess(normalized.tabId, normalized.lastAccessed);
      normalized.lastAccessed = Math.max(normalized.lastAccessed, this.accessTimes.get(normalized.tabId) || 0);
      return normalized;
    }

    async observeWindowFocus(windowId) {
      if (!Number.isSafeInteger(windowId) || windowId < 0) {
        this.focusedWindowId = null;
        return;
      }
      if (this.focusedWindowId === windowId) return;
      this.focusedWindowId = windowId;
      const accessedAt = this.now();
      try {
        const tabs = await this.api.tabs.query({windowId, active: true});
        for (const tab of tabs) {
          if (!tab.active || tab.windowId !== windowId) continue;
          this.rememberAccess(tab.id, accessedAt);
          this.queueEvent({kind: 'upsert', tabId: tab.id});
        }
      } catch (error) {
        this.logger('focused tab lookup failed', error);
      }
    }

    queueEvent(event) {
      // A complete snapshot follows every desktop reconnect. Do not buffer
      // offline metadata in the native port for a future desktop process.
      if (!this.desktopConnected) return;
      if (this.syncing) {
        this.pendingEvents.push(event);
        return;
      }
      this.enqueueEvent(event);
    }

    enqueueEvent(event) {
      const epoch = this.connectionEpoch;
      this.eventChain = this.eventChain
        .then(() => {
          if (epoch === this.connectionEpoch && this.desktopConnected) return this.processEvent(event, epoch);
        })
        .catch(error => this.logger('tab event processing failed', error));
      return this.eventChain;
    }

    async processEvent(event, epoch = this.connectionEpoch) {
      if (!event || event.tabId === undefined || event.tabId === null) return;
      if (event.kind === 'remove') {
        this.index.delete(Number(event.tabId));
        this.accessTimes.delete(Number(event.tabId));
        this.send({type: 'remove', tabId: Number(event.tabId)});
        return;
      }
      try {
        const tab = await this.api.tabs.get(Number(event.tabId));
        if (epoch !== this.connectionEpoch || !this.desktopConnected) return;
        const normalized = this.normalizeCurrentTab(tab);
        this.index.set(normalized.tabId, normalized);
        this.send({type: 'upsert', tab: normalized});
      } catch (_) {
        if (epoch !== this.connectionEpoch || !this.desktopConnected) return;
        this.index.delete(Number(event.tabId));
        this.accessTimes.delete(Number(event.tabId));
        this.send({type: 'remove', tabId: Number(event.tabId)});
      }
    }

    syncSnapshot() {
      const epoch = this.connectionEpoch;
      const run = this.syncQueue.then(() => this._syncSnapshot(epoch));
      this.syncQueue = run.catch(() => {});
      return run;
    }

    async _syncSnapshot(epoch) {
      if (!this.api || !this.api.tabs || !this.desktopConnected || epoch !== this.connectionEpoch) return;
      this.syncing = true;
      this.syncEpoch = epoch;
      this.pendingEvents = [];
      try {
        const tabs = await this.api.tabs.query({});
        if (!this.desktopConnected || epoch !== this.connectionEpoch) return;
        const normalized = tabs.map(tab => this.normalizeCurrentTab(tab));
        this.index = new Map(normalized.map(tab => [tab.tabId, tab]));
        this.send({type: 'snapshot', tabs: normalized}, epoch);
      } finally {
        // A failed manual refresh must not discard closes received during query.
        if (this.syncEpoch === epoch) {
          this.syncing = false;
          const pending = this.pendingEvents;
          this.pendingEvents = [];
          for (const event of pending) this.enqueueEvent(event);
          await this.eventChain;
        }
      }
    }

    sendHello() {
      const hello = {type: 'hello', sessionId: this.sessionId, browser: this.browser, profileId: this.profileId};
      if (this.profileLabel) hello.profileLabel = this.profileLabel;
      return this.send(hello);
    }

    send(message, epoch = this.connectionEpoch) {
      if (!this.connected || !this.desktopConnected || epoch !== this.connectionEpoch ||
          !this.port || typeof this.port.postMessage !== 'function') return false;
      try {
        this.port.postMessage({...message, version: VERSION, desktopGeneration: this.desktopGeneration});
        return true;
      } catch (error) {
        this.logger('native message send failed', error);
        this.handleDisconnect(this.port);
        return false;
      }
    }

    async connect() {
      if (this.connecting || this.connected || !this.api || !this.api.runtime) return;
      this.connecting = true;
      try {
        const port = this.api.runtime.connectNative(this.hostName);
        this.port = port;
        this.connected = true;
        this.setDesktopState(false);
        port.onMessage && port.onMessage.addListener(message => {
          if (this.port !== port) return;
          void this.handleMessage(message).catch(error => {
            this.lastError = String(error && error.message || error);
            this.lastErrorCode = '';
            this.logger('native message handling failed', error);
          });
        });
        port.onDisconnect && port.onDisconnect.addListener(() => {
          // Read lastError in this callback so Chrome does not report it as unhandled.
          const error = this.api.runtime.lastError;
          if (this.port !== port) return;
          this.lastError = error && error.message ? error.message : '';
          this.lastErrorCode = this.lastError ? '' : 'native_closed';
          this.handleDisconnect(port);
        });
        this.lastError = '';
        this.lastErrorCode = '';
      } catch (error) {
        this.lastError = String(error && error.message || error);
        this.lastErrorCode = '';
        this.logger('native connection failed', error);
        this.handleDisconnect(this.port);
      } finally {
        this.connecting = false;
      }
    }

    async retryConnect() {
      if (this.connected) return this.desktopConnected ? this.syncSnapshot() : undefined;
      if (this.reconnectTimer !== null) {
        this.clearSchedule(this.reconnectTimer);
        this.reconnectTimer = null;
      }
      this.reconnectAttempt = 0;
      return this.connect();
    }

    handleDisconnect(port) {
      if (port && port !== this.port) return;
      this.connected = false;
      this.port = null;
      this.setDesktopState(false);
      if (this.reconnectTimer !== null) return;
      const attempt = this.reconnectAttempt;
      const delay = Math.min(MAX_RECONNECT_MS, MIN_RECONNECT_MS * (2 ** attempt));
      this.reconnectAttempt = Math.min(attempt + 1, 20);
      this.reconnectTimer = this.schedule(() => {
        this.reconnectTimer = null;
        void this.connect();
      }, delay);
    }

    setDesktopState(connected, generation = null) {
      this.connectionEpoch++;
      this.desktopConnected = connected;
      this.desktopGeneration = generation;
      this.syncing = false;
      this.syncEpoch = null;
      this.pendingEvents = [];
    }

    async handleMessage(message) {
      if (!message || (message.version !== undefined && message.version !== VERSION)) return;
      if (message.type === 'language_settings') {
        if (message.version !== VERSION || !this.desktopConnected || !validDesktopLanguage(message)) return;
        const settings = {preference: message.preference, language: message.language};
        this.language = settings.language;
        if (this.onLanguageChanged) {
          try { await this.onLanguageChanged(settings); } catch (error) { this.logger('language preference cache failed', error); }
        }
        return;
      }
      if (message.type === 'desktop_status') {
        if (message.version !== VERSION) return;
        if (message.state === 'waiting') {
          this.setDesktopState(false);
        } else if (message.state === 'connected' && Number.isSafeInteger(message.generation) && message.generation > 0) {
          this.setDesktopState(true, message.generation);
          this.reconnectAttempt = 0;
          this.lastError = '';
          this.lastErrorCode = '';
          this.sendHello();
          const epoch = this.connectionEpoch;
          try {
            await this.syncSnapshot();
          } catch (error) {
            if (epoch !== this.connectionEpoch) return;
            this.lastError = String(error && error.message || error);
            this.lastErrorCode = '';
            this.logger('initial desktop snapshot failed', error);
            const port = this.port;
            this.handleDisconnect(port);
            try { port && port.disconnect(); } catch (_) {}
          }
        }
        return;
      }
      if (message.type !== 'activate') return;
      if (!this.desktopConnected) return;
      const epoch = this.connectionEpoch;
      const requestId = message.requestId;
      const failure = (key, code) => Object.assign(new Error(translate(this.language, key)), {glintCode: code});
      try {
        const tabId = Number(message.tabId);
        if (!Number.isSafeInteger(tabId) || tabId < 0) throw failure('invalidTab', 'invalid_tab_id');
        // The tab may have moved since the desktop's last update. The browser's
        // returned tab is authoritative; focusing the cached window can raise
        // a different window while leaving the requested tab behind it.
        const activated = await this.api.tabs.update(tabId, {active: true});
        if (epoch !== this.connectionEpoch || !this.desktopConnected) return;
        if (!activated || activated.active !== true) throw failure('activationFailed', 'activation_failed');
        const windowId = Number(activated.windowId);
        if (!Number.isSafeInteger(windowId) || windowId < 0) throw failure('invalidWindow', 'invalid_window_id');
        const win = await this.api.windows.get(windowId);
        if (epoch !== this.connectionEpoch || !this.desktopConnected) return;
        if (win && win.state === 'minimized') {
          await this.api.windows.update(windowId, {state: 'normal', focused: true});
        } else {
          await this.api.windows.update(windowId, {focused: true});
        }
        if (epoch !== this.connectionEpoch || !this.desktopConnected) return;
        // A competing window can cover the target just after the first focus
        // notification. Require 150 ms of stable focus, with bounded retries
        // when a positive sample is followed by a lost foreground window.
        let focused = false;
        let focusedSamples = 0;
        let focusRetries = 0;
        for (let attempt = 0; attempt < 12; ++attempt) {
          if (epoch !== this.connectionEpoch || !this.desktopConnected) return;
          const current = await this.api.windows.get(windowId);
          if (epoch !== this.connectionEpoch || !this.desktopConnected) return;
          if (current && current.focused === true) {
            if (++focusedSamples >= 4) { focused = true; break; }
          } else {
            if (focusedSamples > 0 && focusRetries < 2) {
              focusRetries++;
              await this.api.windows.update(windowId, {focused: true});
              if (epoch !== this.connectionEpoch || !this.desktopConnected) return;
            }
            focusedSamples = 0;
          }
          if (attempt < 11) {
            await this.wait(50);
            if (epoch !== this.connectionEpoch || !this.desktopConnected) return;
          }
        }
        if (!focused) throw failure('focusFailed', 'window_not_focused');
        const currentTab = await this.api.tabs.get(tabId);
        if (epoch !== this.connectionEpoch || !this.desktopConnected) return;
        if (!currentTab.active || currentTab.windowId !== windowId) throw failure('tabChanged', 'activation_superseded');
        this.send({type: 'activate_result', requestId, ok: true}, epoch);
      } catch (error) {
        this.send({type: 'activate_result', requestId, ok: false, error: String(error && error.message || error),
          ...(error && error.glintCode ? {errorCode: error.glintCode} : {})}, epoch);
      }
    }
  }

  return {VERSION, DEFAULT_HOST, TabBridge, normalizeTab, makeProfileId, browserNameFromUserAgent, loadIdentity,
    messages, normalizeLanguagePreference, resolveLanguage, validDesktopLanguage, systemLanguage, translate};
});













