const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const optionsHtml = fs.readFileSync(path.join(__dirname, '../options.html'), 'utf8');

async function optionsPage(response, storageError) {
  const elements = new Map();
  const copied = [];
  const requests = [];
  function element(id) {
    if (!elements.has(id)) {
      const classes = new Set();
      elements.set(id, {textContent: '', value: '', disabled: false,
        listeners: {}, classList: {
          toggle(name, enabled) { if (enabled) classes.add(name); else classes.delete(name); },
          contains(name) { return classes.has(name); },
        }, addEventListener(type, fn) { this.listeners[type] = fn; }});
    }
    return elements.get(id);
  }
  const api = {runtime: {id: 'abcdefghijklmnopabcdefghijklmnop', sendMessage: async message => {
    requests.push(message);
    if (response instanceof Error) throw response;
    return response;
  }}, storage: {local: {get: async () => ({profileLabel: '工作'}), set: async () => { if (storageError) throw storageError; }}}};
  const context = vm.createContext({chrome: api, navigator: {userAgent: 'Edg/130.0',
    clipboard: {writeText: async text => copied.push(text)}}, document: {getElementById: element,
    addEventListener: () => {}}, setTimeout: () => 1, clearTimeout() {}, setInterval: () => 1});
  vm.runInContext(fs.readFileSync(path.join(__dirname, '../options.js'), 'utf8'), context);
  await new Promise(resolve => setImmediate(resolve));
  return {element, copied, requests, api, click: async id => element(id).listeners.click?.(),
    submit: async () => element('profile-form').listeners.submit({preventDefault() {}})};
}

test('setup shows the current browser and copies the actual extension ID', async () => {
  const page = await optionsPage({ok: true, portOpen: false});
  assert.equal(page.element('browser-name').textContent, 'Microsoft Edge');
  assert.equal(page.element('extension-id').value, page.api.runtime.id);
  await page.click('copy-id');
  assert.deepEqual(page.copied, [page.api.runtime.id]);
  assert.match(page.element('copy-status').textContent, /Glint.*连接遇到问题.*粘贴.*保存/);
  assert.doesNotMatch(page.element('copy-status').textContent, /其他版本/);
});

test('extension ID and copy action are visible beside connection status without opening details', () => {
  const section = optionsHtml.match(/<section\b[^>]*class="connection"[^>]*>[\s\S]*?<\/section>/)?.[0];
  assert.ok(section, 'connection status section must exist');
  assert.match(section, /id="extension-id"/);
  assert.match(section, /id="copy-id"/);
  assert.doesNotMatch(section, /<details\b|\shidden(?:\s|=|>)/);
});

test('missing native host directs ID repair to the desktop troubleshooting page', async () => {
  const error = 'Specified native messaging host not found.';
  for (const response of [{ok: true, portOpen: false, error}, new Error(error)]) {
    const page = await optionsPage(response);
    assert.equal(page.element('connection-status').textContent, '尚未建立桥接通道');
    const detail = page.element('connection-detail').textContent;
    assert.match(detail, /浏览器找不到 Glint 本机通信桥/);
    assert.match(detail, /连接遇到问题.*粘贴下方扩展 ID.*保存.*重试/);
    assert.match(detail, /同一 Windows 用户/);
    assert.match(detail, /浏览器.*允许当前用户.*Native Messaging/);
    assert.ok(detail.includes(error), 'the original browser error must remain visible');
    assert.doesNotMatch(detail, /Glint 正在运行/);
  }
});

test('only a confirmed desktop connection is green and still asks for tab synchronization confirmation', async () => {
  const page = await optionsPage({ok: true, portOpen: true, desktopConnected: true});
  assert.equal(page.element('connection-status').textContent, '已连接 Glint');
  assert.equal(page.element('status-dot').classList.contains('good'), true);
  assert.match(page.element('connection-detail').textContent, /Glint.*确认.*标签页.*同步/);
  assert.doesNotMatch(page.element('connection-detail').textContent, /标签页已同步|搜索已就绪/);
});

test('a legacy or malformed worker status is not green and explains reloading the extension', async () => {
  for (const desktopConnected of [undefined, null, 'true', 1]) {
    const response = {ok: true, portOpen: true};
    if (desktopConnected !== undefined) response.desktopConnected = desktopConnected;
    const page = await optionsPage(response);
    assert.equal(page.element('connection-status').textContent, '扩展需要重新加载');
    assert.equal(page.element('status-dot').classList.contains('good'), false);
    assert.match(page.element('connection-detail').textContent, /后台版本不一致/);
    assert.match(page.element('connection-detail').textContent, /Edge.*扩展管理页.*Glint.*重新加载/);
    assert.doesNotMatch(page.element('connection-detail').textContent, /已连接 Glint/);
  }
});

test('a waiting native bridge explains automatic connection when Glint starts', async () => {
  const page = await optionsPage({ok: true, desktopConnected: false, portOpen: true});
  assert.equal(page.element('connection-status').textContent, 'Glint 未运行，等待自动连接');
  assert.equal(page.element('status-dot').classList.contains('good'), false);
  assert.match(page.element('connection-detail').textContent, /启动 Glint.*自动连接/);
  assert.doesNotMatch(page.element('connection-detail').textContent, /粘贴.*ID|网络|点击.*重试/);
  assert.equal(page.requests.length, 1);
  assert.equal(page.requests[0].type, 'glint_connection_status');
  await page.click('retry');
  assert.equal(page.requests.at(-1).type, 'glint_retry_connection');
});

test('retry clears its busy state and displays the actual native host error', async () => {
  const page = await optionsPage({ok: true, portOpen: false, error: 'Native host not found'});
  await page.click('retry');
  assert.equal(page.element('retry').disabled, false);
  assert.equal(page.requests.at(-1).type, 'glint_retry_connection');
  assert.match(page.element('connection-detail').textContent, /Native host not found/);
});

test('storage failure does not report a saved profile label', async () => {
  const page = await optionsPage({ok: true, portOpen: false}, new Error('Storage unavailable'));
  await page.submit();
  assert.match(page.element('save-status').textContent, /保存失败/);
  assert.doesNotMatch(page.element('save-status').textContent, /^已保存$/);
  assert.equal(page.requests.filter(message => message.type === 'profile_label_changed').length, 0);
});

test('profile label save explains when the native port is still closed', async () => {
  const page = await optionsPage({ok: true, portOpen: false});
  await page.submit();
  assert.equal(page.element('save-status').textContent, '已保存，将在下次连接时生效');
});

test('a legacy worker cannot claim a saved label already reached the desktop', async () => {
  const page = await optionsPage({ok: true, portOpen: true});
  await page.submit();
  assert.equal(page.element('save-status').textContent, '已保存，将在下次连接时生效');
});
