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
      this.sessionId = opts.sessionId || makeProfileId();
      this.schedule = opts.schedule || ((fn, ms) => setTimeout(fn, ms));
      this.clearSchedule = opts.clearSchedule || (id => clearTimeout(id));
      this.wait = opts.wait || (ms => new Promise(resolve => setTimeout(resolve, ms)));
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
      this.reconnectAttempt = 0;
      this.reconnectTimer = null;
      this.lastError = '';
    }

    attachListeners() {
      if (this.listenersAttached || !this.api || !this.api.tabs) return;
      const events = this.api.tabs;
      events.onCreated && events.onCreated.addListener(tab => this.queueEvent({kind: 'upsert', tabId: tab.id}));
      events.onUpdated && events.onUpdated.addListener(tabId => this.queueEvent({kind: 'upsert', tabId}));
      events.onActivated && events.onActivated.addListener(info => this.queueEvent({kind: 'upsert', tabId: info.tabId}));
      events.onAttached && events.onAttached.addListener(tabId => this.queueEvent({kind: 'upsert', tabId}));
      // A detached tab can still exist in another window. Reading it avoids a transient false removal.
      events.onDetached && events.onDetached.addListener(tabId => this.queueEvent({kind: 'upsert', tabId}));
      events.onRemoved && events.onRemoved.addListener(tabId => this.queueEvent({kind: 'remove', tabId}));
      // Prerendering can replace a tab ID without firing onRemoved for the old ID.
      events.onReplaced && events.onReplaced.addListener((addedTabId, removedTabId) => {
        this.queueEvent({kind: 'remove', tabId: removedTabId});
        this.queueEvent({kind: 'upsert', tabId: addedTabId});
      });
      this.listenersAttached = true;
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
        this.send({type: 'remove', tabId: Number(event.tabId)});
        return;
      }
      try {
        const tab = await this.api.tabs.get(Number(event.tabId));
        if (epoch !== this.connectionEpoch || !this.desktopConnected) return;
        const normalized = normalizeTab(tab);
        this.index.set(normalized.tabId, normalized);
        this.send({type: 'upsert', tab: normalized});
      } catch (_) {
        if (epoch !== this.connectionEpoch || !this.desktopConnected) return;
        this.index.delete(Number(event.tabId));
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
        const normalized = tabs.map(normalizeTab);
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
            this.logger('native message handling failed', error);
          });
        });
        port.onDisconnect && port.onDisconnect.addListener(() => {
          // Read lastError in this callback so Chrome does not report it as unhandled.
          const error = this.api.runtime.lastError;
          if (this.port !== port) return;
          this.lastError = error && error.message ? error.message : 'Native messaging connection closed.';
          this.handleDisconnect(port);
        });
        this.lastError = '';
      } catch (error) {
        this.lastError = String(error && error.message || error);
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
      if (message.type === 'desktop_status') {
        if (message.version !== VERSION) return;
        if (message.state === 'waiting') {
          this.setDesktopState(false);
        } else if (message.state === 'connected' && Number.isSafeInteger(message.generation) && message.generation > 0) {
          this.setDesktopState(true, message.generation);
          this.reconnectAttempt = 0;
          this.lastError = '';
          this.sendHello();
          const epoch = this.connectionEpoch;
          try {
            await this.syncSnapshot();
          } catch (error) {
            if (epoch !== this.connectionEpoch) return;
            this.lastError = String(error && error.message || error);
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
      try {
        const tabId = Number(message.tabId);
        if (!Number.isSafeInteger(tabId) || tabId < 0) throw new Error('invalid tab id');
        // The tab may have moved since the desktop's last update. The browser's
        // returned tab is authoritative; focusing the cached window can raise
        // a different window while leaving the requested tab behind it.
        const activated = await this.api.tabs.update(tabId, {active: true});
        if (epoch !== this.connectionEpoch || !this.desktopConnected) return;
        if (!activated || activated.active !== true) throw new Error('标签页未能激活，请重试。');
        const windowId = Number(activated.windowId);
        if (!Number.isSafeInteger(windowId) || windowId < 0) throw new Error('invalid current window id');
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
        if (!focused) throw new Error('标签页已切换，但 Windows 未将目标窗口切到前台。请重试，或从任务栏打开该窗口。');
        const currentTab = await this.api.tabs.get(tabId);
        if (epoch !== this.connectionEpoch || !this.desktopConnected) return;
        if (!currentTab.active || currentTab.windowId !== windowId) throw new Error('标签页状态已变化，请重新搜索后重试。');
        this.send({type: 'activate_result', requestId, ok: true}, epoch);
      } catch (error) {
        this.send({type: 'activate_result', requestId, ok: false, error: String(error && error.message || error)}, epoch);
      }
    }
  }

  return {VERSION, DEFAULT_HOST, TabBridge, normalizeTab, makeProfileId, browserNameFromUserAgent, loadIdentity};
});













