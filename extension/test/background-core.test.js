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
      onCreated: listeners(), onUpdated: listeners(), onRemoved: listeners(), onActivated: listeners(), onAttached: listeners(), onDetached: listeners(), onReplaced: listeners(),
    },
    windows: { get: async id => ({id, state: 'normal', focused: true}), update: async () => ({}), onFocusChanged: listeners() },
    runtime: { connectNative: () => { throw new Error('not connected in test'); } },
    storage: { local: { get: async keys => ({profileId: 'profile-test'}), set: async () => {} } },
  };
  return {api, tabs};
}

function portFake() {
  const messages=[]; const listeners=[]; const disconnect=[];
  return {messages, postMessage: m => messages.push(m), onMessage: {addListener: fn => listeners.push(fn)}, onDisconnect: {addListener: fn => disconnect.push(fn)}, emit: m => listeners.forEach(fn => fn(m)), drop: () => disconnect.forEach(fn => fn())};
}

function readyBridge(bridge, port) {
  bridge.port = port;
  bridge.connected = true;
  bridge.desktopConnected = true;
  bridge.desktopGeneration = 1;
}

test('normalizeTab keeps metadata and strips unsupported fields', () => {
  assert.deepEqual(normalizeTab({id: 4, windowId: 2, title: '中文', url: 'https://x', active: true, favIconUrl: 'x'}), {tabId:4, windowId:2, title:'中文', url:'https://x', active:true, lastAccessed:0});
});

test('normalizeTab keeps valid browser access times and rejects invalid values', () => {
  assert.equal(normalizeTab({id: 1, lastAccessed: 1730000000123.75}).lastAccessed, 1730000000123);
  for (const lastAccessed of [undefined, null, -1, NaN, Infinity, '1730000000123', Number.MAX_SAFE_INTEGER + 1]) {
    assert.equal(normalizeTab({id: 1, lastAccessed}).lastAccessed, 0);
  }
});

test('snapshots and metadata refreshes retain recency without manufacturing visits', async () => {
  const {api, tabs} = fakeApi();
  tabs[0].lastAccessed = 1000;
  let now = 3000;
  const bridge = new TabBridge({api, now: () => now});
  const port = portFake(); readyBridge(bridge, port); bridge.attachListeners();
  await bridge.syncSnapshot();
  assert.deepEqual([...bridge.index.values()].map(tab => tab.lastAccessed), [1000, 0]);
  tabs[0].title = 'Renamed';
  api.tabs.onUpdated.forEach(fn => fn(1));
  await bridge.eventChain;
  assert.equal(bridge.index.get(1).lastAccessed, 1000);
  api.tabs.onActivated.forEach(fn => fn({tabId: 2, windowId: 10}));
  await bridge.eventChain;
  assert.equal(bridge.index.get(2).lastAccessed, 3000);
  now = 5000;
  await bridge.syncSnapshot();
  assert.equal(bridge.index.get(2).lastAccessed, 3000, 'a snapshot preserves the observed activation time');
  api.tabs.onRemoved.forEach(fn => fn(2));
  await bridge.eventChain;
  assert.equal(bridge.accessTimes.has(2), false);
});

test('window focus records only its active tab and ignores losing or repeated focus', async () => {
  const {api, tabs} = fakeApi();
  tabs.push({id: 3, windowId: 20, title: 'Other window', active: true});
  // Deliberately return all tabs: the bridge must still validate the focused window.
  let now = 1000;
  const bridge = new TabBridge({api, now: () => now});
  const port = portFake(); readyBridge(bridge, port); bridge.attachListeners();
  await bridge.syncSnapshot();
  await Promise.all(api.windows.onFocusChanged.map(fn => fn(10)));
  await bridge.eventChain;
  assert.deepEqual([...bridge.index.values()].map(tab => tab.lastAccessed), [1000, 0, 0]);
  now = 2000;
  await Promise.all(api.windows.onFocusChanged.map(fn => fn(10)));
  await bridge.eventChain;
  assert.equal(bridge.index.get(1).lastAccessed, 1000);
  await Promise.all(api.windows.onFocusChanged.map(fn => fn(-1)));
  await Promise.all(api.windows.onFocusChanged.map(fn => fn(20)));
  await bridge.eventChain;
  assert.equal(bridge.index.get(3).lastAccessed, 2000);
});

test('real activation while desktop is disconnected survives the next snapshot', async () => {
  const {api} = fakeApi();
  const bridge = new TabBridge({api, now: () => 1234});
  bridge.attachListeners();
  api.tabs.onActivated.forEach(fn => fn({tabId: 2, windowId: 10}));
  const port = portFake(); readyBridge(bridge, port);
  await bridge.syncSnapshot();
  assert.equal(bridge.index.get(2).lastAccessed, 1234);
});

test('activation of a newly created tab during a stale snapshot retains its observed time', async () => {
  const {api, tabs} = fakeApi();
  const bridge = new TabBridge({api, now: () => 1234});
  const port = portFake(); readyBridge(bridge, port); bridge.attachListeners();
  api.tabs.query = async () => {
    const stale = tabs.map(tab => ({...tab}));
    tabs.push({id: 3, windowId: 10, title: 'New tab', active: true});
    api.tabs.onCreated.forEach(fn => fn(tabs.at(-1)));
    api.tabs.onActivated.forEach(fn => fn({tabId: 3, windowId: 10}));
    return stale;
  };
  await bridge.syncSnapshot();
  assert.equal(bridge.index.get(3).lastAccessed, 1234);
});

test('an older window-focus read cannot replace a newer activation timestamp', async () => {
  const {api, tabs} = fakeApi();
  let now = 1000;
  const bridge = new TabBridge({api, now: () => now});
  const port = portFake(); readyBridge(bridge, port); bridge.attachListeners();
  let finishQuery;
  api.tabs.query = () => new Promise(resolve => { finishQuery = resolve; });
  const focused = api.windows.onFocusChanged[0](10);
  now = 2000;
  api.tabs.onActivated.forEach(fn => fn({tabId: 1, windowId: 10}));
  await bridge.eventChain;
  finishQuery(tabs.map(tab => ({...tab})));
  await focused;
  await bridge.eventChain;
  assert.equal(bridge.index.get(1).lastAccessed, 2000);
});

test('only validated language settings from a connected desktop can change extension language', async () => {
  const {api} = fakeApi();
  const changes = [];
  const bridge = new TabBridge({api, language: 'en', onLanguageChanged: value => changes.push(value)});
  const port = portFake(); readyBridge(bridge, port);
  const valid = {version: 1, type: 'language_settings', preference: 'system', language: 'zh-CN'};
  for (const invalid of [{...valid, version: undefined}, {...valid, version: 2}, {...valid, language: 'fr'},
    {...valid, preference: 'fr'}, {...valid, preference: 'en'}, {...valid, language: undefined}]) {
    await bridge.handleMessage(invalid);
    assert.equal(bridge.language, 'en');
  }
  await bridge.handleMessage(valid);
  assert.equal(bridge.language, 'zh-CN');
  assert.deepEqual(changes, [{preference: 'system', language: 'zh-CN'}]);
  bridge.setDesktopState(false);
  await bridge.handleMessage({...valid, preference: 'en', language: 'en'});
  assert.equal(bridge.language, 'zh-CN', 'offline native frames must be ignored');
  await bridge.handleMessage({version: 1, type: 'desktop_status', state: 'connected', generation: 2});
  await bridge.handleMessage({...valid, preference: 'en', language: 'en'});
  assert.equal(bridge.language, 'en', 'reconnected desktop can refresh its choice');
});

test('snapshot queues events and replays current tab state after snapshot', async () => {
  const {api, tabs} = fakeApi();
  const bridge = new TabBridge({api, profileId: 'p', browser: 'chrome'});
  const port = portFake(); readyBridge(bridge, port); bridge.attachListeners();
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
  readyBridge(bridge, port); bridge.attachListeners();
  api.tabs.onRemoved.forEach(fn => fn(2, {windowId: 10, isWindowClosing: false}));
  await new Promise(resolve => setImmediate(resolve));
  assert.deepEqual(port.messages[0], {version:1, desktopGeneration: 1, type:'remove', tabId:2});
});

test('closing a tab removes it from the live index without reconnecting', async () => {
  const {api, tabs} = fakeApi();
  const bridge = new TabBridge({api, profileId: 'p'}); const port = portFake();
  readyBridge(bridge, port); bridge.attachListeners();
  await bridge.syncSnapshot();
  tabs.splice(1, 1);
  api.tabs.onRemoved.forEach(fn => fn(2, {windowId: 10, isWindowClosing: false}));
  await bridge.eventChain;
  assert.deepEqual([...bridge.index.keys()], [1]);
  assert.deepEqual(port.messages.at(-1), {version: 1, desktopGeneration: 1, type: 'remove', tabId: 2});
});

test('a close during a snapshot is replayed after its stale query result', async () => {
  const {api, tabs} = fakeApi();
  const bridge = new TabBridge({api, profileId: 'p'}); const port = portFake();
  readyBridge(bridge, port); bridge.attachListeners();
  api.tabs.query = async () => {
    const oldTabs = tabs.map(tab => ({...tab}));
    tabs.splice(1, 1);
    api.tabs.onRemoved.forEach(fn => fn(2, {windowId: 10, isWindowClosing: false}));
    return oldTabs;
  };
  await bridge.syncSnapshot();
  assert.deepEqual([...bridge.index.keys()], [1]);
  assert.deepEqual(port.messages.map(message => message.type), ['snapshot', 'remove']);
});

test('an in-flight tab read cannot reinsert a tab after its close event', async () => {
  const {api, tabs} = fakeApi();
  const bridge = new TabBridge({api, profileId: 'p'}); const port = portFake();
  readyBridge(bridge, port); bridge.attachListeners();
  await bridge.syncSnapshot();
  let completeRead;
  api.tabs.get = () => new Promise(resolve => { completeRead = resolve; });
  api.tabs.onUpdated.forEach(fn => fn(2));
  await Promise.resolve();
  const oldTab = {...tabs[1]};
  tabs.splice(1, 1);
  api.tabs.onRemoved.forEach(fn => fn(2, {windowId: 10, isWindowClosing: false}));
  completeRead(oldTab);
  await bridge.eventChain;
  assert.deepEqual([...bridge.index.keys()], [1]);
  assert.equal(port.messages.at(-1).type, 'remove');
});

test('replaced tabs retire their old ID and closing the replacement leaves no stale result', async () => {
  const {api, tabs} = fakeApi();
  const bridge = new TabBridge({api, profileId: 'p'}); const port = portFake();
  readyBridge(bridge, port); bridge.attachListeners();
  await bridge.syncSnapshot();
  tabs[0] = {...tabs[0], id: 3, title: 'Replacement'};
  api.tabs.onReplaced.forEach(fn => fn(3, 1));
  await bridge.eventChain;
  assert.equal(bridge.index.has(1), false, 'the browser retires the old ID without onRemoved');
  assert.equal(bridge.index.get(3).title, 'Replacement');
  assert.deepEqual(port.messages.slice(1).map(message => message.type), ['remove', 'upsert']);
  tabs.splice(0, 1);
  api.tabs.onRemoved.forEach(fn => fn(3, {windowId: 10, isWindowClosing: false}));
  await bridge.eventChain;
  assert.deepEqual([...bridge.index.keys()], [2]);
});

test('a failed refresh still delivers a close captured during its query', async () => {
  const {api, tabs} = fakeApi();
  const bridge = new TabBridge({api, profileId: 'p'}); const port = portFake();
  readyBridge(bridge, port); bridge.attachListeners();
  await bridge.syncSnapshot();
  api.tabs.query = async () => {
    tabs.splice(1, 1);
    api.tabs.onRemoved.forEach(fn => fn(2, {windowId: 10, isWindowClosing: false}));
    throw new Error('query interrupted');
  };
  await assert.rejects(bridge.syncSnapshot(), /query interrupted/);
  await bridge.eventChain;
  assert.deepEqual([...bridge.index.keys()], [1]);
  assert.deepEqual(port.messages.at(-1), {version: 1, desktopGeneration: 1, type: 'remove', tabId: 2});
});
test('connect waits for a desktop acknowledgement before sending hello and snapshot', async () => {
  const {api} = fakeApi(); const port = portFake(); api.runtime.connectNative = () => port;
  const bridge = new TabBridge({api, profileId: 'p', profileLabel: 'Work', browser: 'edge'});
  bridge.attachListeners(); await bridge.connect();
  assert.deepEqual(port.messages, [], 'a native port alone does not imply a running desktop');
  await bridge.handleMessage({version: 1, type: 'desktop_status', state: 'connected', generation: 1});
  assert.deepEqual(port.messages.map(m => m.type), ['hello', 'snapshot']);
  assert.equal(port.messages[0].version, 1);
  assert.match(port.messages[0].sessionId, /^[a-z0-9-]{8,}$/);
  assert.equal(port.messages[0].profileLabel, 'Work');
});

test('activate focuses window, restores minimized state, activates tab and reports success', async () => {
  const {api, tabs} = fakeApi(); let focused; let restored; let activated;
  api.windows.get = async () => ({id:10, state:focused ? 'normal' : 'minimized', focused: Boolean(focused)});
  api.windows.update = async (id, p) => { focused = p.focused; restored = p.state; };
  api.tabs.update = async (id, p) => { activated = [id,p.active]; Object.assign(tabs[1], p); return {...tabs[1]}; };
  const bridge = new TabBridge({api, profileId:'p', browser:'edge'}); const port = portFake(); readyBridge(bridge, port);
  await bridge.handleMessage({version:1,type:'activate',requestId:'r',tabId:2,windowId:10});
  assert.deepEqual([focused, restored, activated], [true,'normal',[2,true]]);
  assert.deepEqual(port.messages[0], {version:1,desktopGeneration:1,type:'activate_result',requestId:'r',ok:true});
});

test('a resolved focus call that leaves the browser behind reports failure', async () => {
  const {api} = fakeApi();
  api.windows.get = async id => ({id, state: 'normal', focused: false});
  const bridge = new TabBridge({api, wait: async () => {}}); const port = portFake(); readyBridge(bridge, port);
  await bridge.handleMessage({version: 1, type: 'activate', requestId: 'denied', tabId: 2, windowId: 10});
  assert.equal(port.messages.at(-1).ok, false, 'API resolution alone does not prove foreground activation');
  assert.match(port.messages.at(-1).error, /前台|focus/i);
  assert.equal(port.messages.at(-1).errorCode, 'window_not_focused');
});

test('activation failures expose stable codes independently of the extension language', async () => {
  for (const language of ['en', 'zh-CN']) {
    for (const [scenario, code] of [['invalid-tab', 'invalid_tab_id'], ['inactive', 'activation_failed'],
      ['invalid-window', 'invalid_window_id'], ['superseded', 'activation_superseded']]) {
      const {api} = fakeApi();
      if (scenario === 'inactive') api.tabs.update = async () => ({active: false});
      if (scenario === 'invalid-window') api.tabs.update = async () => ({active: true, windowId: -1});
      if (scenario === 'superseded') api.tabs.get = async () => ({active: false, windowId: 10});
      const bridge = new TabBridge({api, language, wait: async () => {}});
      const port = portFake(); readyBridge(bridge, port);
      await bridge.handleMessage({version: 1, type: 'activate', requestId: 'failure',
        tabId: scenario === 'invalid-tab' ? -1 : 2});
      const result = port.messages.at(-1);
      assert.equal(result.ok, false);
      assert.equal(result.errorCode, code);
      assert.equal(/[\u3400-\u9fff]/.test(result.error), language === 'zh-CN');
    }
  }
});

test('activation focuses the current window after a tab moved and preserves maximized state', async () => {
  const {api, tabs} = fakeApi(); const calls = [];
  tabs[1].windowId = 30;
  const updateTab = api.tabs.update;
  api.tabs.update = async (id, patch) => { calls.push(['tab', id]); return updateTab(id, patch); };
  api.windows.get = async id => ({id, state: 'maximized', focused: true});
  api.windows.update = async (id, patch) => { calls.push(['window', id, patch]); };
  const bridge = new TabBridge({api}); const port = portFake(); readyBridge(bridge, port);
  await bridge.handleMessage({version: 1, type: 'activate', requestId: 'moved', tabId: 2, windowId: 10});
  assert.deepEqual(calls, [['tab', 2], ['window', 30, {focused: true}]]);
  assert.equal(port.messages.at(-1).ok, true);
});

test('activation waits briefly for focus confirmation before reporting success', async () => {
  const {api} = fakeApi(); let reads = 0; let waits = 0;
  api.windows.get = async id => ({id, state: 'normal', focused: ++reads >= 3});
  const bridge = new TabBridge({api, wait: async () => { ++waits; }}); const port = portFake(); readyBridge(bridge, port);
  await bridge.handleMessage({version: 1, type: 'activate', requestId: 'slow', tabId: 2, windowId: 10});
  assert.equal(port.messages.at(-1).ok, true);
  assert.equal(waits, 4, 'delayed focus must remain stable across several samples');
});

test('activation regains focus when another window covers the first focused sample', async () => {
  const {api} = fakeApi(); let focused = false; let focusCalls = 0; const delays = [];
  api.windows.get = async id => ({id, state: 'normal', focused});
  api.windows.update = async () => { focusCalls++; focused = true; };
  const bridge = new TabBridge({api, wait: async ms => {
    delays.push(ms);
    if (delays.length === 1) focused = false;
  }});
  const port = portFake(); readyBridge(bridge, port);
  await bridge.handleMessage({version: 1, type: 'activate', requestId: 'covered', tabId: 2, windowId: 10});
  assert.equal(focusCalls, 2, 'the target must be raised again after the competing window covers it');
  assert.equal(port.messages.at(-1).ok, true);
  assert.equal(focused, true);
  assert.ok(delays.length >= 4, 'success must follow stable focus after the retry');
  assert.ok(delays.reduce((sum, ms) => sum + ms, 0) <= 550, 'settling must stay bounded');
});

test('activation reports failure when focus is permanently lost during settling', async () => {
  const {api} = fakeApi(); let focused = false; let focusCalls = 0; let waited = 0;
  api.windows.get = async id => ({id, state: 'normal', focused});
  api.windows.update = async () => { if (++focusCalls === 1) focused = true; };
  const bridge = new TabBridge({api, wait: async ms => { waited += ms; focused = false; }});
  const port = portFake(); readyBridge(bridge, port);
  await bridge.handleMessage({version: 1, type: 'activate', requestId: 'lost', tabId: 2, windowId: 10});
  assert.equal(port.messages.at(-1).ok, false, 'a transient focused sample cannot prove successful activation');
  assert.match(port.messages.at(-1).error, /前台|focus/i);
  assert.ok(focusCalls >= 2 && focusCalls <= 3, 'focus retries must be bounded');
  assert.ok(waited <= 550, 'failure must not keep retrying indefinitely');
});

test('activation reports a refocus failure after losing focus during settling', async () => {
  const {api} = fakeApi(); let focused = false; let focusCalls = 0;
  api.windows.get = async id => ({id, state: 'normal', focused});
  api.windows.update = async () => {
    if (++focusCalls > 1) throw new Error('refocus failed');
    focused = true;
  };
  const bridge = new TabBridge({api, wait: async () => { focused = false; }});
  const port = portFake(); readyBridge(bridge, port);
  await bridge.handleMessage({version: 1, type: 'activate', requestId: 'refused', tabId: 2, windowId: 10});
  assert.equal(port.messages.at(-1).ok, false);
  assert.match(port.messages.at(-1).error, /refocus failed/);
});

test('activation gives up after bounded retries when windows repeatedly exchange focus', async () => {
  const {api} = fakeApi(); let reads = 0; let focusCalls = 0; let waited = 0;
  api.windows.get = async id => ({id, state: 'normal', focused: ++reads % 2 === 0});
  api.windows.update = async () => { focusCalls++; };
  const bridge = new TabBridge({api, wait: async ms => { waited += ms; }});
  const port = portFake(); readyBridge(bridge, port);
  await bridge.handleMessage({version: 1, type: 'activate', requestId: 'competing', tabId: 2, windowId: 10});
  assert.equal(port.messages.at(-1).ok, false);
  assert.equal(focusCalls, 3, 'only two retries may follow the initial focus request');
  assert.ok(waited <= 550);
});

test('activation stops when the desktop reconnects during focus settling', async () => {
  const {api} = fakeApi(); let focusCalls = 0; let tabReads = 0; let waits = 0;
  api.windows.update = async () => { focusCalls++; };
  const getTab = api.tabs.get;
  api.tabs.get = async id => { tabReads++; return getTab(id); };
  const bridge = new TabBridge({api, wait: async () => {
    waits++;
    await bridge.handleMessage({version: 1, type: 'desktop_status', state: 'waiting'});
    await bridge.handleMessage({version: 1, type: 'desktop_status', state: 'connected', generation: 2});
  }});
  const port = portFake(); readyBridge(bridge, port);
  await bridge.handleMessage({version: 1, type: 'activate', requestId: 'settling-restart', tabId: 2, windowId: 10});
  assert.equal(waits, 1);
  assert.equal(focusCalls, 1, 'obsolete activation must not raise its target again');
  assert.equal(tabReads, 0, 'obsolete activation must not keep validating its target');
  assert.equal(port.messages.some(message => message.type === 'activate_result'), false);
});

test('activation stops when the desktop reconnects during a refocus request', async () => {
  const {api} = fakeApi(); let focused = false; let focusCalls = 0; let tabReads = 0;
  api.windows.get = async id => ({id, state: 'normal', focused});
  const getTab = api.tabs.get;
  api.tabs.get = async id => { tabReads++; return getTab(id); };
  const bridge = new TabBridge({api, wait: async () => { focused = false; }});
  api.windows.update = async () => {
    focused = true;
    if (++focusCalls === 2) {
      await bridge.handleMessage({version: 1, type: 'desktop_status', state: 'waiting'});
      await bridge.handleMessage({version: 1, type: 'desktop_status', state: 'connected', generation: 2});
    }
  };
  const port = portFake(); readyBridge(bridge, port);
  await bridge.handleMessage({version: 1, type: 'activate', requestId: 'refocus-restart', tabId: 2, windowId: 10});
  assert.equal(focusCalls, 2);
  assert.equal(tabReads, 0);
  assert.equal(port.messages.some(message => message.type === 'activate_result'), false);
});

test('activation does not report success when the selected tab failed to become active', async () => {
  const {api} = fakeApi();
  api.tabs.update = async id => api.tabs.get(id);
  const bridge = new TabBridge({api}); const port = portFake(); readyBridge(bridge, port);
  await bridge.handleMessage({version: 1, type: 'activate', requestId: 'inactive', tabId: 2, windowId: 10});
  assert.equal(port.messages.at(-1).ok, false);
});

test('activation failure is reported to desktop', async () => {
  const {api} = fakeApi(); api.windows.get = async () => { throw new Error('window closed'); };
  const bridge = new TabBridge({api, profileId:'p'}); const port = portFake(); readyBridge(bridge, port);
  await bridge.handleMessage({version:1,type:'activate',requestId:'r2',tabId:2,windowId:10});
  assert.equal(port.messages[0].ok, false);
  assert.match(port.messages[0].error, /window closed/);
});

test('activation stops after desktop restart at asynchronous tab, window and focus boundaries', async () => {
  for (const stage of ['tab-update', 'window-read', 'focus-poll']) {
    const {api} = fakeApi(); const port = portFake();
    const bridge = new TabBridge({api, wait: async () => { waits++; }}); readyBridge(bridge, port);
    let finishStep; let focused = 0; let reads = 0; let waits = 0; let tabReads = 0;
    if (stage === 'tab-update') api.tabs.update = () => new Promise(resolve => { finishStep = resolve; });
    api.windows.get = async id => {
      reads++;
      if ((stage === 'window-read' && reads === 1) || (stage === 'focus-poll' && reads === 2)) {
        return new Promise(resolve => { finishStep = resolve; });
      }
      return {id, state: 'normal', focused: true};
    };
    api.windows.update = async () => { focused++; };
    const getTab = api.tabs.get;
    api.tabs.get = async id => { tabReads++; return getTab(id); };
    const activation = bridge.handleMessage({version: 1, type: 'activate', requestId: stage, tabId: 2, windowId: 10});
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(typeof finishStep, 'function', `${stage} must reach its asynchronous boundary`);
    await bridge.handleMessage({version: 1, type: 'desktop_status', state: 'waiting'});
    await bridge.handleMessage({version: 1, type: 'desktop_status', state: 'connected', generation: 2});
    finishStep(stage === 'tab-update'
      ? {id: 2, active: true, windowId: 10}
      : {id: 10, state: 'normal', focused: false});
    await activation;
    assert.equal(focused, stage === 'focus-poll' ? 1 : 0, `${stage}: a superseded request must not focus a window`);
    assert.equal(waits, 0, `${stage}: obsolete focus polling must stop`);
    assert.equal(tabReads, 0, `${stage}: obsolete activation must not continue reading its target`);
    assert.equal(port.messages.some(message => message.type === 'activate_result'), false);
  }
});

test('disconnect schedules bounded reconnect with exponential backoff', () => {
  const {api} = fakeApi(); const timers=[];
  const bridge = new TabBridge({api, schedule: (fn, ms) => { timers.push(ms); return 1; }, clearSchedule:()=>{}});
  bridge.handleDisconnect(); bridge.handleDisconnect(); bridge.handleDisconnect();
  assert.deepEqual(timers, [500]);
  assert.equal(bridge.reconnectAttempt, 1);
});

test('a native port without desktop acknowledgement keeps exponential backoff', async () => {
  const {api} = fakeApi(); const timers = []; let nextPort;
  api.runtime.connectNative = () => { nextPort = portFake(); return nextPort; };
  const bridge = new TabBridge({api, schedule: (fn, ms) => { timers.push({fn, ms}); return timers.length; }});
  for (let attempt = 0; attempt < 9; ++attempt) {
    await bridge.connect();
    nextPort.drop();
    bridge.reconnectTimer = null;
  }
  assert.deepEqual(timers.map(timer => timer.ms), [500, 1000, 2000, 4000, 8000, 16000, 30000, 30000, 30000]);
});

test('only a real connected desktop acknowledgement resets retry backoff', async () => {
  const {api} = fakeApi(); const port = portFake();
  api.runtime.connectNative = () => port;
  const bridge = new TabBridge({api, schedule: () => 1});
  bridge.reconnectAttempt = 4;
  await bridge.connect();
  assert.equal(bridge.reconnectAttempt, 4);
  await bridge.handleMessage({version: 1, type: 'desktop_status', state: 'connected', generation: 1});
  assert.equal(bridge.reconnectAttempt, 0);
});

test('invalid desktop acknowledgements cannot enable metadata or reset backoff', async () => {
  const {api} = fakeApi(); const port = portFake(); api.runtime.connectNative = () => port;
  const bridge = new TabBridge({api}); bridge.reconnectAttempt = 3;
  await bridge.connect();
  for (const message of [
    {type: 'desktop_status', state: 'connected', generation: 1},
    {version: 2, type: 'desktop_status', state: 'connected', generation: 1},
    {version: 1, type: 'desktop_status', state: 'connected'},
    {version: 1, type: 'desktop_status', state: 'connected', generation: 0},
  ]) await bridge.handleMessage(message);
  assert.equal(bridge.desktopConnected, false);
  assert.equal(bridge.reconnectAttempt, 3);
  assert.deepEqual(port.messages, []);
});

test('desktop offline keeps one native port and automatically resynchronizes on the same port', async () => {
  const {api, tabs} = fakeApi(); const saved = []; let starts = 0;
  api.storage.local.set = async values => saved.push(values);
  const port = portFake(); api.runtime.connectNative = () => { starts++; return port; };
  const bridge = new TabBridge({api, schedule: () => assert.fail('waiting must not schedule connectNative')});
  bridge.attachListeners();
  await bridge.connect();
  await bridge.handleMessage({version: 1, type: 'desktop_status', state: 'connected', generation: 1});
  const before = port.messages.length;
  await bridge.handleMessage({version: 1, type: 'desktop_status', state: 'waiting'});
  assert.equal(bridge.connected, true);
  assert.equal(bridge.desktopConnected, false);
  assert.equal(bridge.port, port);
  tabs.splice(1, 1);
  api.tabs.onRemoved.forEach(fn => fn(2, {windowId: 10}));
  await bridge.syncSnapshot();
  await bridge.eventChain;
  assert.equal(port.messages.length, before, 'offline metadata must not queue behind the pipe reconnect');
  await bridge.handleMessage({version: 1, type: 'desktop_status', state: 'connected', generation: 2});
  assert.deepEqual(port.messages.slice(before).map(message => message.type), ['hello', 'snapshot']);
  assert.deepEqual(port.messages.at(-1).tabs.map(tab => tab.tabId), [1]);
  assert.ok(port.messages.slice(before).every(message => message.desktopGeneration === 2));
  assert.equal(starts, 1);
  assert.deepEqual(saved, [], 'no persistent pause should prevent the next desktop startup');
});

test('late messages and disconnects from an old native port cannot alter a replacement', async () => {
  const {api} = fakeApi(); const ports = [];
  api.runtime.connectNative = () => { const port = portFake(); ports.push(port); return port; };
  const bridge = new TabBridge({api, schedule: () => 1, clearSchedule: () => {}});
  await bridge.connect();
  ports[0].drop();
  await bridge.retryConnect();
  await bridge.handleMessage({version: 1, type: 'desktop_status', state: 'connected', generation: 1});
  ports[0].emit({version: 1, type: 'desktop_status', state: 'waiting'});
  ports[0].drop();
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(bridge.port, ports[1]);
  assert.equal(bridge.connected, true);
  assert.equal(bridge.desktopConnected, true);
});

test('a snapshot from a previous desktop session cannot overwrite a restarted desktop', async () => {
  const {api, tabs} = fakeApi(); const port = portFake();
  api.runtime.connectNative = () => port;
  const bridge = new TabBridge({api});
  await bridge.connect();
  let finishOldQuery;
  const originalQuery = api.tabs.query;
  api.tabs.query = () => new Promise(resolve => { finishOldQuery = resolve; });
  const firstSync = bridge.handleMessage({version: 1, type: 'desktop_status', state: 'connected', generation: 1});
  await Promise.resolve();
  const staleTabs = tabs.map(tab => ({...tab}));
  await bridge.handleMessage({version: 1, type: 'desktop_status', state: 'waiting'});
  tabs.splice(1, 1);
  api.tabs.query = originalQuery;
  const nextSync = bridge.handleMessage({version: 1, type: 'desktop_status', state: 'connected', generation: 2});
  assert.equal(typeof finishOldQuery, 'function', 'desktop acknowledgement must begin a complete query');
  finishOldQuery(staleTabs);
  await Promise.all([firstSync, nextSync]);
  const snapshots = port.messages.filter(message => message.type === 'snapshot');
  assert.equal(snapshots.length, 1);
  assert.equal(snapshots[0].desktopGeneration, 2);
  assert.deepEqual(snapshots[0].tabs.map(tab => tab.tabId), [1]);
});

test('an in-flight tab read from an old desktop generation cannot send into a new one', async () => {
  const {api, tabs} = fakeApi(); const port = portFake();
  api.runtime.connectNative = () => port;
  const bridge = new TabBridge({api}); bridge.attachListeners();
  await bridge.connect();
  await bridge.handleMessage({version: 1, type: 'desktop_status', state: 'connected', generation: 1});
  let finishRead;
  api.tabs.get = () => new Promise(resolve => { finishRead = resolve; });
  api.tabs.onUpdated.forEach(fn => fn(2));
  await Promise.resolve();
  const oldTab = {...tabs[1]};
  await bridge.handleMessage({version: 1, type: 'desktop_status', state: 'waiting'});
  tabs.splice(1, 1);
  const nextSync = bridge.handleMessage({version: 1, type: 'desktop_status', state: 'connected', generation: 2});
  finishRead(oldTab);
  await nextSync;
  await bridge.eventChain;
  const newMessages = port.messages.filter(message => message.desktopGeneration === 2);
  assert.deepEqual(newMessages.map(message => message.type), ['hello', 'snapshot']);
  assert.deepEqual(newMessages[1].tabs.map(tab => tab.tabId), [1]);
  assert.equal(bridge.index.has(2), false);
});

test('events queued behind an old tab read cannot leak into a new desktop generation', async () => {
  const {api, tabs} = fakeApi(); const port = portFake(); api.runtime.connectNative = () => port;
  const bridge = new TabBridge({api}); bridge.attachListeners();
  await bridge.connect();
  await bridge.handleMessage({version: 1, type: 'desktop_status', state: 'connected', generation: 1});
  let finishRead;
  api.tabs.get = () => new Promise(resolve => { finishRead = resolve; });
  api.tabs.onUpdated.forEach(fn => fn(1));
  await Promise.resolve();
  api.tabs.onRemoved.forEach(fn => fn(2, {windowId: 10}));
  await bridge.handleMessage({version: 1, type: 'desktop_status', state: 'waiting'});
  tabs.splice(1, 1);
  const nextSync = bridge.handleMessage({version: 1, type: 'desktop_status', state: 'connected', generation: 2});
  finishRead({...tabs[0]});
  await nextSync;
  await bridge.eventChain;
  assert.deepEqual(port.messages.filter(message => message.desktopGeneration === 2).map(message => message.type), ['hello', 'snapshot']);
});

test('a failed initial desktop snapshot reconnects automatically instead of remaining unsynchronized', async () => {
  const {api} = fakeApi(); const ports = []; const timers = [];
  api.runtime.connectNative = () => {
    const port = portFake(); port.disconnect = () => port.drop(); ports.push(port); return port;
  };
  const query = api.tabs.query;
  api.tabs.query = async () => { throw new Error('transient query failure'); };
  const bridge = new TabBridge({api, schedule: (fn, ms) => { timers.push({fn, ms}); return timers.length; }});
  await bridge.connect();
  ports[0].emit({version: 1, type: 'desktop_status', state: 'connected', generation: 1});
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(bridge.connected, false, 'a hello without a snapshot must not stay stuck on this port');
  assert.equal(timers.length, 1);
  assert.equal(timers[0].ms, 500);
  api.tabs.query = query;
  timers[0].fn();
  await new Promise(resolve => setImmediate(resolve));
  ports[1].emit({version: 1, type: 'desktop_status', state: 'connected', generation: 1});
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(bridge.desktopConnected, true);
  assert.deepEqual(ports[1].messages.map(message => message.type), ['hello', 'snapshot']);
});

test('makeProfileId produces stable non-empty id', () => { const id = makeProfileId(); assert.match(id, /^[a-z0-9-]{8,}$/); });




