const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

async function optionsPage(response, storageError) {
  const elements = new Map();
  const copied = [];
  const requests = [];
  function element(id) {
    if (!elements.has(id)) elements.set(id, {textContent: '', value: '', disabled: false,
      listeners: {}, classList: {toggle() {}}, addEventListener(type, fn) { this.listeners[type] = fn; }});
    return elements.get(id);
  }
  const api = {runtime: {id: 'abcdefghijklmnopabcdefghijklmnop', sendMessage: async message => {
    requests.push(message); return response;
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
});

test('open native port is described as a port, with desktop verification still required', async () => {
  const page = await optionsPage({ok: true, portOpen: true});
  assert.equal(page.element('connection-status').textContent, '桥接通道已打开');
  assert.match(page.element('connection-detail').textContent, /回到 Glint.*确认/);
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
