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
      this.logger = opts.logger || (() => {});
      this.port = null;
      this.connected = false;
      this.connecting = false;
      this.listenersAttached = false;
      this.syncing = false;
      this.pendingEvents = [];
      this.eventChain = Promise.resolve();
      this.syncQueue = Promise.resolve();
      this.index = new Map();
      this.reconnectAttempt = 0;
      this.reconnectTimer = null;
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
      this.listenersAttached = true;
    }

    queueEvent(event) {
      if (this.syncing) {
        this.pendingEvents.push(event);
        return;
      }
      this.enqueueEvent(event);
    }

    enqueueEvent(event) {
      this.eventChain = this.eventChain
        .then(() => this.processEvent(event))
        .catch(error => this.logger('tab event processing failed', error));
      return this.eventChain;
    }

    async processEvent(event) {
      if (!event || event.tabId === undefined || event.tabId === null) return;
      if (event.kind === 'remove') {
        this.index.delete(Number(event.tabId));
        this.send({type: 'remove', tabId: Number(event.tabId)});
        return;
      }
      try {
        const tab = await this.api.tabs.get(Number(event.tabId));
        const normalized = normalizeTab(tab);
        this.index.set(normalized.tabId, normalized);
        this.send({type: 'upsert', tab: normalized});
      } catch (_) {
        this.index.delete(Number(event.tabId));
        this.send({type: 'remove', tabId: Number(event.tabId)});
      }
    }

    syncSnapshot() {
      const run = this.syncQueue.then(() => this._syncSnapshot());
      this.syncQueue = run.catch(() => {});
      return run;
    }

    async _syncSnapshot() {
      if (!this.api || !this.api.tabs || !this.connected) return;
      this.syncing = true;
      this.pendingEvents = [];
      let tabs;
      try {
        tabs = await this.api.tabs.query({});
      } catch (error) {
        this.syncing = false;
        throw error;
      }
      const normalized = tabs.map(normalizeTab);
      this.index = new Map(normalized.map(tab => [tab.tabId, tab]));
      this.send({type: 'snapshot', tabs: normalized});
      this.syncing = false;
      const pending = this.pendingEvents;
      this.pendingEvents = [];
      for (const event of pending) this.enqueueEvent(event);
      await this.eventChain;
    }

    sendHello() {
      const hello = {type: 'hello', sessionId: this.sessionId, browser: this.browser, profileId: this.profileId};
      if (this.profileLabel) hello.profileLabel = this.profileLabel;
      return this.send(hello);
    }

    send(message) {
      if (!this.connected || !this.port || typeof this.port.postMessage !== 'function') return false;
      try {
        this.port.postMessage({...message, version: VERSION});
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
        port.onMessage && port.onMessage.addListener(message => { void this.handleMessage(message); });
        port.onDisconnect && port.onDisconnect.addListener(() => this.handleDisconnect(port));
        this.sendHello();

        await this.syncSnapshot();
        this.reconnectAttempt = 0;
      } catch (error) {
        this.logger('native connection failed', error);
        this.handleDisconnect(this.port);
      } finally {
        this.connecting = false;
      }
    }

    handleDisconnect(port) {
      if (port && this.port && port !== this.port) return;
      this.connected = false;
      this.port = null;
      this.syncing = false;
      if (this.reconnectTimer !== null) return;
      const attempt = this.reconnectAttempt;
      const delay = Math.min(MAX_RECONNECT_MS, MIN_RECONNECT_MS * (2 ** attempt));
      this.reconnectAttempt = Math.min(attempt + 1, 20);
      this.reconnectTimer = this.schedule(() => {
        this.reconnectTimer = null;
        void this.connect();
      }, delay);
    }

    async handleMessage(message) {
      if (!message || (message.version !== undefined && message.version !== VERSION)) return;
      if (message.type !== 'activate') return;
      const requestId = message.requestId;
      try {
        const tabId = Number(message.tabId);
        const windowId = Number(message.windowId);
        if (!Number.isFinite(tabId) || !Number.isFinite(windowId)) throw new Error('invalid tab or window id');
        const win = await this.api.windows.get(windowId);
        if (win && win.state === 'minimized') {
          await this.api.windows.update(windowId, {state: 'normal', focused: true});
        } else {
          await this.api.windows.update(windowId, {focused: true});
        }
        await this.api.tabs.update(tabId, {active: true});
        this.send({type: 'activate_result', requestId, ok: true});
      } catch (error) {
        this.send({type: 'activate_result', requestId, ok: false, error: String(error && error.message || error)});
      }
    }
  }

  return {VERSION, DEFAULT_HOST, TabBridge, normalizeTab, makeProfileId, browserNameFromUserAgent, loadIdentity};
});













