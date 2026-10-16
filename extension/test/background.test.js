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

async function worker() {
  let optionsOpened = 0;
  let nativeStarts = 0;
  const messages = [];
  const timers = new Map();
  const port = {postMessage: message => messages.push(message), onMessage: event(), onDisconnect: event()};
  const api = {
    runtime: {
      onInstalled: event(), onStartup: event(), onMessage: event(),
      openOptionsPage: async () => { optionsOpened++; },
      connectNative: () => { nativeStarts++; return port; },
    },
    action: {onClicked: event()},
    storage: {local: {get: async () => ({profileId: 'test-profile'}), set: async () => {}}},
    tabs: {query: async () => [{id: 1, windowId: 2, title: '中文标签', url: 'https://example.test'}]},
  };
  const context = vm.createContext({chrome: api, GlintExtension: core, importScripts: () => {}, console,
    navigator: {userAgent: 'Edg/130.0'}, setTimeout: (fn, ms) => { const id = timers.size + 1; timers.set(id, {fn, ms}); return id; },
    clearTimeout: id => timers.delete(id)});
  vm.runInContext(fs.readFileSync(path.join(__dirname, '../background.js'), 'utf8'), context);
  await new Promise(resolve => setImmediate(resolve));
  // Use deterministic timers for later disconnects; the worker uses the real core module.
  context.glintBridge.schedule = (fn, ms) => { const id = timers.size + 1; timers.set(id, {fn, ms}); return id; };
  context.glintBridge.clearSchedule = id => timers.delete(id);
  const request = message => new Promise(resolve => {
    const results = api.runtime.onMessage.emit(message, {id: 'self'}, resolve);
    if (!results.includes(true)) resolve(undefined);
  });
  return {api, port, timers, messages, request, context,
    get optionsOpened() { return optionsOpened; }, get nativeStarts() { return nativeStarts; }};
}

test('first installation opens setup, updates do not reopen it', async () => {
  const app = await worker();
  app.api.runtime.onInstalled.emit({reason: 'install'});
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(app.optionsOpened, 1);
  app.api.runtime.onInstalled.emit({reason: 'update'});
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(app.optionsOpened, 1);
});

test('toolbar action reopens setup', async () => {
  const app = await worker();
  app.api.action.onClicked.emit();
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(app.optionsOpened, 1);
});

test('setup reports the native port separately from desktop synchronization', async () => {
  const app = await worker();
  const response = await app.request({type: 'glint_connection_status'});
  assert.equal(response.ok, true);
  assert.equal(response.portOpen, true);
  assert.equal(response.browser, 'edge');
  assert.equal(response.desktopConnected, undefined);
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
