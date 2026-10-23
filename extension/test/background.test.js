const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const core = require('../background-core.js');

function event() {
  const listeners = [];
  return {addListener: fn => listeners.push(fn), emit: (...args) => listeners.map(fn => fn(...args))};
}

async function worker(storedValues = {}, desktopState = 'connected', locale = 'zh-CN', settings = {}) {
  let optionsOpened = 0;
  let nativeStarts = 0;
  const messages = [];
  const notifications = [];
  const titles = [];
  const timers = new Map();
  const port = {postMessage: message => messages.push(message), onMessage: event(), onDisconnect: event()};
  port.disconnect = () => port.onDisconnect.emit();
  const api = {
    runtime: {
      onInstalled: event(), onStartup: event(), onMessage: event(),
      openOptionsPage: async () => { optionsOpened++; },
      connectNative: () => { nativeStarts++; return port; },
      sendMessage: async message => { notifications.push(message); },
    },
    i18n: {getUILanguage: () => locale},
    action: {onClicked: event(), setTitle: async ({title}) => titles.push(title)},
    storage: {onChanged: event(), local: {get: async keys => {
      if (settings.failIdentity && keys.includes('profileId')) throw new Error('Storage unavailable');
      return {profileId: 'test-profile', ...storedValues};
    },
      set: async values => {
        if (settings.setStorage) await settings.setStorage(values);
        Object.assign(storedValues, values);
      }}},
    tabs: {query: async () => [{id: 1, windowId: 2, title: '中文标签', url: 'https://example.test'}]},
  };
  const context = vm.createContext({chrome: api, GlintExtension: core, importScripts: () => {},
    console: settings.failIdentity ? {error() {}} : console,
    navigator: {userAgent: 'Edg/130.0'}, setTimeout: (fn, ms) => { const id = timers.size + 1; timers.set(id, {fn, ms}); return id; },
    clearTimeout: id => timers.delete(id)});
  vm.runInContext(fs.readFileSync(path.join(__dirname, '../background.js'), 'utf8'), context);
  await new Promise(resolve => setImmediate(resolve));
  port.onMessage.emit({version: 1, type: 'desktop_status', state: desktopState, generation: 1});
  await new Promise(resolve => setImmediate(resolve));
  // Use deterministic timers for later disconnects; the worker uses the real core module.
  if (context.glintBridge) {
    context.glintBridge.schedule = (fn, ms) => { const id = timers.size + 1; timers.set(id, {fn, ms}); return id; };
    context.glintBridge.clearSchedule = id => timers.delete(id);
  }
  const request = message => new Promise(resolve => {
    const results = api.runtime.onMessage.emit(message, {id: 'self'}, resolve);
    if (!results.includes(true)) resolve(undefined);
  });
  const sendLanguage = async (language, preference = language) => {
    port.onMessage.emit({version: 1, type: 'language_settings', preference, language});
    await new Promise(resolve => setImmediate(resolve));
  };
  return {api, port, timers, messages, notifications, titles, request, context, sendLanguage,
    get optionsOpened() { return optionsOpened; }, get nativeStarts() { return nativeStarts; }};
}

test('installation and updates connect quietly without opening a browser tab', async () => {
  const app = await worker();
  app.api.runtime.onInstalled.emit({reason: 'install'});
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(app.optionsOpened, 0);
  app.api.runtime.onInstalled.emit({reason: 'update'});
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(app.optionsOpened, 0);
});

test('toolbar and bridge use the last desktop language while offline, ignoring old profile choices', async () => {
  const app = await worker({desktopLanguage: {preference: 'system', language: 'en'}, language: 'zh-CN'}, 'waiting', 'zh-TW');
  assert.equal(app.titles.at(-1), 'Glint · Connection status');
  assert.equal(app.context.glintBridge.language, 'en');
  app.port.onMessage.emit({version: 1, type: 'desktop_status', state: 'connected', generation: 2});
  await app.sendLanguage('zh-CN', 'system');
  assert.equal(app.titles.at(-1), 'Glint · 连接状态');
  assert.equal(app.context.glintBridge.language, 'zh-CN');
  const automatic = await worker({language: 'zh-CN'}, 'waiting', 'fr-FR');
  assert.equal(automatic.context.glintBridge.language, 'en');
});

test('native language settings update toolbar, status and open panels before storage finishes', async () => {
  let finishWrite;
  const stored = {};
  const app = await worker(stored, 'connected', 'en-US', {setStorage: () => new Promise(resolve => { finishWrite = resolve; })});
  await app.sendLanguage('zh-CN');
  assert.equal(app.context.glintBridge.language, 'zh-CN');
  assert.equal(app.titles.at(-1), 'Glint · 连接状态');
  assert.equal((await app.request({type: 'glint_connection_status'})).language, 'zh-CN');
  assert.equal(app.notifications.at(-1).type, 'glint_language_changed');
  assert.equal(app.notifications.at(-1).language, 'zh-CN');
  assert.equal(stored.desktopLanguage, undefined);
  finishWrite();
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(stored.desktopLanguage.language, 'zh-CN');
  const restarted = await worker(stored, 'waiting', 'en-US');
  assert.equal(restarted.context.glintBridge.language, 'zh-CN');
});

test('rapid desktop language changes persist in order and a failed write does not stop tab sync', async () => {
  const writes = [];
  const completions = [];
  const stored = {};
  const app = await worker(stored, 'connected', 'en-US', {setStorage: value => {
    writes.push(value.desktopLanguage.language);
    return new Promise((resolve, reject) => completions.push({resolve, reject}));
  }});
  await app.sendLanguage('zh-CN');
  await app.sendLanguage('en', 'system');
  assert.deepEqual(writes, ['zh-CN'], 'only one cache write may be in flight');
  assert.equal(app.context.glintBridge.language, 'en');
  completions[0].reject(new Error('Storage full'));
  await new Promise(resolve => setImmediate(resolve));
  assert.deepEqual(writes, ['zh-CN', 'en']);
  completions[1].resolve();
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(stored.desktopLanguage.language, 'en');
  assert.equal((await app.request({type: 'glint_connection_status'})).language, 'en');
  await app.context.glintBridge.syncSnapshot();
  assert.equal(app.messages.at(-1).type, 'snapshot');
});

test('failed worker startup returns a stable code for the popup to localize', async () => {
  const app = await worker({}, 'connected', 'zh-CN', {failIdentity: true});
  const response = await app.request({type: 'glint_connection_status'});
  assert.equal(response.ok, false);
  assert.equal(response.errorCode, 'bridge_unavailable');
  assert.notEqual(response.error, 'bridge is not available');
});

test('a cached disconnect retains desktop language while browser diagnostics stay intact', async () => {
  const app = await worker({}, 'connected', 'en-US');
  await app.sendLanguage('zh-CN');
  app.port.onDisconnect.emit();
  let response = await app.request({type: 'glint_connection_status'});
  assert.equal(response.errorCode, 'native_closed');
  assert.equal(response.error, '本机通信连接已关闭。');
  await app.request({type: 'glint_retry_connection'});
  app.api.runtime.lastError = {message: 'Browser diagnostic: native host failed'};
  app.port.onDisconnect.emit();
  delete app.api.runtime.lastError;
  response = await app.request({type: 'glint_connection_status'});
  assert.equal(response.error, 'Browser diagnostic: native host failed');
  assert.equal(response.errorCode, undefined);
});

test('toolbar status uses a packaged popup without opening an options tab', async () => {
  const app = await worker();
  const manifest = JSON.parse(fs.readFileSync(path.join(__dirname, '../manifest.json'), 'utf8'));
  assert.equal(manifest.action.default_popup, 'options.html');
  assert.ok(fs.existsSync(path.join(__dirname, '..', manifest.action.default_popup)));
  app.api.action.onClicked.emit();
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(app.optionsOpened, 0);
});

test('browser startup reconnects without requiring a status page', async () => {
  const app = await worker();
  app.api.runtime.onStartup.emit();
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(app.nativeStarts, 1, 'startup must reuse the existing native connection');
  assert.equal(app.optionsOpened, 0);
  assert.ok(app.messages.some(message => message.type === 'hello'));
  assert.ok(app.messages.some(message => message.type === 'snapshot'));
});

test('setup reports the native port separately from desktop synchronization', async () => {
  const app = await worker();
  const response = await app.request({type: 'glint_connection_status'});
  assert.equal(response.ok, true);
  assert.equal(response.portOpen, true);
  assert.equal(response.browser, 'edge');
  assert.equal(response.desktopConnected, true);
});

test('saving a profile label reports its current port and refreshes desktop metadata', async () => {
  const app = await worker();
  const before = app.messages.length;
  const response = await app.request({type: 'profile_label_changed', profileLabel: ' 工作 '});
  assert.equal(response.ok, true);
  assert.equal(response.portOpen, true);
  const updates = app.messages.slice(before);
  assert.equal(updates[0].type, 'hello');
  assert.equal(updates[0].profileLabel, '工作');
  assert.equal(updates[1].type, 'snapshot');
});

test('manual retry cancels backoff and creates one replacement native connection', async () => {
  const app = await worker();
  app.api.runtime.lastError = {message: 'Specified native messaging host not found.'};
  app.port.onDisconnect.emit();
  delete app.api.runtime.lastError;
  const before = await app.request({type: 'glint_connection_status'});
  assert.equal(before.portOpen, false);
  assert.match(before.error, /host not found/);
  assert.equal(app.timers.size, 1);
  const response = await app.request({type: 'glint_retry_connection'});
  assert.equal(response.ok, true);
  assert.equal(response.portOpen, true);
  assert.equal(app.timers.size, 0);
  assert.equal(app.nativeStarts, 2);
  await app.request({type: 'glint_retry_connection'});
  assert.equal(app.nativeStarts, 2);
});

test('desktop exit waits without spawning another native port or persisting a pause', async () => {
  const stored = {};
  const app = await worker(stored);
  app.port.onMessage.emit({version: 1, type: 'desktop_status', state: 'waiting'});
  await new Promise(resolve => setImmediate(resolve));
  const response = await app.request({type: 'glint_connection_status'});
  assert.equal(response.desktopConnected, false);
  assert.equal(response.portOpen, true);
  assert.equal(stored.connectionPaused, undefined);
  assert.equal(app.timers.size, 0);
  assert.equal(app.nativeStarts, 1);
  const before = app.messages.length;
  const saved = await app.request({type: 'profile_label_changed', profileLabel: '工作'});
  assert.equal(saved.desktopConnected, false);
  assert.equal(app.messages.length, before, 'offline labels synchronize with the next complete snapshot');
  app.port.onMessage.emit({version: 1, type: 'desktop_status', state: 'connected', generation: 2});
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(app.nativeStarts, 1);
  assert.deepEqual(app.messages.slice(before).map(message => message.type), ['hello', 'snapshot']);
  assert.equal(app.messages.at(-2).profileLabel, '工作');
});

test('status queries and retries reuse the native port while waiting for Glint to start', async () => {
  const app = await worker({}, 'waiting');
  assert.equal(app.nativeStarts, 1);
  assert.equal((await app.request({type: 'glint_connection_status'})).desktopConnected, false);
  const response = await app.request({type: 'glint_retry_connection'});
  assert.equal(response.desktopConnected, false);
  assert.equal(response.portOpen, true);
  assert.equal(app.nativeStarts, 1);
  assert.deepEqual(app.messages, []);
});

test('browser startup and extension install connect without requiring a manual retry', async () => {
  for (const eventName of ['onStartup', 'onInstalled']) {
    const app = await worker({}, 'waiting');
    app.api.runtime[eventName].emit();
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(app.nativeStarts, 1);
    assert.equal(app.optionsOpened, 0);
  }
});
