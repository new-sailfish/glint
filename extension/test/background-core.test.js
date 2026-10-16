const test = require('node:test');
const assert = require('node:assert/strict');
const { TabBridge, normalizeTab, makeProfileId } = require('../background-core.js');

function fakeApi() {
  const listeners = () => { const f=[]; f.addListener = fn => f.push(fn); return f; };
  const tabs = [
    {id: 1, windowId: 10, title: 'A', url: 'https://a.test', active: true},
    {id: 2, windowId: 10, title: 'B', url: 'https://b.test', active: false},
  ];
  const api = {
    tabs: {
      query: async () => tabs.map(t => ({...t})),
      get: async id => { const t = tabs.find(x => x.id === id); if (!t) throw new Error('missing'); return {...t}; },
      update: async (id, patch) => { const t = tabs.find(x => x.id === id); if (!t) throw new Error('missing'); Object.assign(t, patch); return {...t}; },
      onCreated: listeners(), onUpdated: listeners(), onRemoved: listeners(), onActivated: listeners(), onAttached: listeners(), onDetached: listeners(),
    },
    windows: { get: async id => ({id, state: 'normal'}), update: async () => ({}) },
    runtime: { connectNative: () => { throw new Error('not connected in test'); } },
    storage: { local: { get: async keys => ({profileId: 'profile-test'}), set: async () => {} } },
  };
  return {api, tabs};
}

function portFake() {
  const messages=[]; const listeners=[]; const disconnect=[];
  return {messages, postMessage: m => messages.push(m), onMessage: {addListener: fn => listeners.push(fn)}, onDisconnect: {addListener: fn => disconnect.push(fn)}, emit: m => listeners.forEach(fn => fn(m)), drop: () => disconnect.forEach(fn => fn())};
}

test('normalizeTab keeps metadata and strips unsupported fields', () => {
  assert.deepEqual(normalizeTab({id: 4, windowId: 2, title: '中文', url: 'https://x', active: true, favIconUrl: 'x'}), {tabId:4, windowId:2, title:'中文', url:'https://x', active:true});
});

test('snapshot queues events and replays current tab state after snapshot', async () => {
  const {api, tabs} = fakeApi();
  const bridge = new TabBridge({api, profileId: 'p', browser: 'chrome'});
  const port = portFake(); bridge.port = port; bridge.connected = true; bridge.attachListeners();
  const originalQuery = api.tabs.query;
  api.tabs.query = async () => { const result = await originalQuery(); tabs[0].title = 'A updated'; api.tabs.onUpdated.forEach(fn => fn(1, {title:'A updated'}, {...tabs[0]})); return result; };
  await bridge.syncSnapshot();
  assert.equal(port.messages[0].type, 'snapshot');
  assert.equal(port.messages[0].tabs[0].title, 'A');
  assert.equal(port.messages[1].type, 'upsert');
  assert.equal(port.messages[1].tab.title, 'A updated');
});

test('tab removal emits a remove message with the protocol tabId', async () => {
  const {api} = fakeApi(); const bridge = new TabBridge({api, profileId: 'p'}); const port = portFake();
  bridge.port = port; bridge.connected = true; bridge.attachListeners();
  api.tabs.onRemoved.forEach(fn => fn(2, {windowId: 10, isWindowClosing: false}));
  await new Promise(resolve => setImmediate(resolve));
  assert.deepEqual(port.messages[0], {version:1, type:'remove', tabId:2});
});
test('connect sends hello before the initial snapshot', async () => {
  const {api} = fakeApi(); const port = portFake(); api.runtime.connectNative = () => port;
  const bridge = new TabBridge({api, profileId: 'p', profileLabel: 'Work', browser: 'edge'});
  bridge.attachListeners(); await bridge.connect();
  assert.deepEqual(port.messages.map(m => m.type), ['hello', 'snapshot']);
  assert.equal(port.messages[0].version, 1);
  assert.match(port.messages[0].sessionId, /^[a-z0-9-]{8,}$/);
  assert.equal(port.messages[0].profileLabel, 'Work');
});

test('activate focuses window, restores minimized state, activates tab and reports success', async () => {
  const {api} = fakeApi(); let focused; let restored; let activated;
  api.windows.get = async () => ({id:10, state:'minimized'});
  api.windows.update = async (id, p) => { focused = p.focused; restored = p.state; };
  api.tabs.update = async (id, p) => { activated = [id,p.active]; };
  const bridge = new TabBridge({api, profileId:'p', browser:'edge'}); const port = portFake(); bridge.port=port; bridge.connected=true;
  await bridge.handleMessage({version:1,type:'activate',requestId:'r',tabId:2,windowId:10});
  assert.deepEqual([focused, restored, activated], [true,'normal',[2,true]]);
  assert.deepEqual(port.messages[0], {version:1,type:'activate_result',requestId:'r',ok:true});
});

test('activation failure is reported to desktop', async () => {
  const {api} = fakeApi(); api.windows.get = async () => { throw new Error('window closed'); };
  const bridge = new TabBridge({api, profileId:'p'}); const port = portFake(); bridge.port=port; bridge.connected=true;
  await bridge.handleMessage({version:1,type:'activate',requestId:'r2',tabId:2,windowId:10});
  assert.equal(port.messages[0].ok, false);
  assert.match(port.messages[0].error, /window closed/);
});

test('disconnect schedules bounded reconnect with exponential backoff', () => {
  const {api} = fakeApi(); const timers=[];
  const bridge = new TabBridge({api, schedule: (fn, ms) => { timers.push(ms); return 1; }, clearSchedule:()=>{}});
  bridge.handleDisconnect(); bridge.handleDisconnect(); bridge.handleDisconnect();
  assert.deepEqual(timers, [500]);
  assert.equal(bridge.reconnectAttempt, 1);
});

test('makeProfileId produces stable non-empty id', () => { const id = makeProfileId(); assert.match(id, /^[a-z0-9-]{8,}$/); });




