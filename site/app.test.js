'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const i18n = require('./i18n.js');

// A small event-capable DOM fixture lets Node run the actual application code.
// Only browser APIs are replaced; filtering, selection and setup state are real.
function createPage() {
  const document = { documentElement: {} };
  class Element {
    constructor() {
      this.value = '';
      this.textContent = '';
      this.dataset = {};
      this.hidden = false;
      this.attributes = new Map();
      this.listeners = new Map();
      this.children = new Map();
      const classes = new Set();
      this.classList = {
        add: value => classes.add(value), remove: value => classes.delete(value),
        contains: value => classes.has(value),
        toggle: (value, on) => on ? classes.add(value) : classes.delete(value),
      };
    }
    addEventListener(type, listener) { this.listeners.set(type, listener); }
    emit(type, values = {}) { this.listeners.get(type)?.({ target: this, preventDefault() {}, ...values }); }
    setAttribute(name, value) { this.attributes.set(name, value); }
    removeAttribute(name) { this.attributes.delete(name); }
    querySelector(selector) { return this.children.get(selector); }
    querySelectorAll(selector) { return this.children.get(selector) || []; }
    append() {}
    focus() { document.activeElement = this; this.emit('focus'); }
  }
  const nodes = new Map(['search-demo', 'demo-query', 'demo-clear', 'demo-status', 'demo-empty',
    'site-language', 'setup-guide', 'setup-back', 'setup-next', 'setup-progress'].map(id => [`#${id}`, new Element()]));
  const titles = ['产品设计评审 · 飞书文档', '设计规范 · Figma', '日历 · Outlook'];
  const rows = titles.map((title, index) => {
    const row = new Element();
    row.dataset = { title, url: `https://example.com/${index}` };
    const button = new Element();
    const label = new Element();
    label.textContent = title;
    button.children.set('strong', label);
    row.children.set('button', button);
    return row;
  });
  const browsers = ['chrome', 'edge'].map(name => {
    const element = new Element(); element.dataset.browser = name; return element;
  });
  const browserFields = ['name', 'address', 'developer', 'mode', 'load'].map(name => {
    const element = new Element(); element.dataset.browserField = name; return element;
  });
  const panels = Array.from({ length: 5 }, (_, index) => {
    const element = new Element(); element.id = `setup-step-${index + 1}`; return element;
  });
  const steps = Array.from({ length: 5 }, () => new Element());
  const guide = nodes.get('#setup-guide');
  guide.children.set('.setup-panel', panels);
  guide.children.set('[data-step]', steps);
  guide.children.set('.setup-controls', new Element());
  document.querySelector = selector => nodes.get(selector);
  document.querySelectorAll = selector => ({ '#demo-results > li': rows,
    '[data-browser]': browsers, '[data-browser-field]': browserFields })[selector] || [];
  document.createElement = () => new Element();
  const stored = new Map();
  const window = {
    location: { hash: '' }, addEventListener() {},
    localStorage: { getItem: key => stored.get(key), setItem: (key, value) => stored.set(key, value) },
    GlintSiteI18n: { ...i18n, translatePage(_document, language) {
      document.documentElement.lang = language;
      rows.forEach((row, index) => { row.querySelector('button').querySelector('strong').textContent = i18n.translate(titles[index], language); });
    } },
  };
  vm.runInNewContext(fs.readFileSync(path.join(__dirname, 'app.js'), 'utf8'), {
    document, window, navigator: { language: 'en-US' },
  });
  return { nodes, rows, browsers, browserFields, panels, steps, document, stored };
}

test('switching language preserves query, selected result, activation feedback, browser and setup step', () => {
  const page = createPage();
  const query = page.nodes.get('#demo-query');
  query.value = 'design';
  query.emit('input');
  assert.equal(page.nodes.get('#demo-status').textContent, '2 matching sample tabs');
  const selected = page.rows[0].querySelector('button');
  selected.emit('click');
  page.browsers[1].emit('click');
  page.steps[3].emit('click');
  const selector = page.nodes.get('#site-language');
  selector.value = 'zh-CN';
  selector.emit('change');
  assert.equal(query.value, 'design');
  assert.equal(page.rows[0].hidden, false);
  assert.equal(selected.classList.contains('is-selected'), true);
  assert.equal(page.nodes.get('#demo-status').textContent, '演示：已选择“产品设计评审 · 飞书文档”。在 Glint 中，这一步会切换到对应标签页。');
  assert.equal(page.nodes.get('#setup-progress').textContent, '步骤 4 / 5');
  assert.equal(page.browserFields.find(field => field.dataset.browserField === 'name').textContent, 'Edge');
  selector.value = 'en';
  selector.emit('change');
  assert.equal(selected.classList.contains('is-selected'), true);
  assert.equal(page.nodes.get('#demo-status').textContent, 'Demo: selected “Product design review · Feishu Docs”. Glint would switch to that tab.');
  query.value = 'missing';
  query.emit('input');
  assert.equal(selected.classList.contains('is-selected'), false);
  assert.equal(page.nodes.get('#demo-status').textContent, '0 matching sample tabs');
  assert.equal(page.nodes.get('#demo-empty').hidden, false);
});

test('switching language keeps a keyboard-selected sample and accepts a query in either language', () => {
  const page = createPage();
  const query = page.nodes.get('#demo-query');
  query.value = '设计';
  query.emit('input');
  const selected = page.rows[1].querySelector('button');
  selected.focus();
  const selector = page.nodes.get('#site-language');
  selector.value = 'zh-CN';
  selector.emit('change');
  assert.equal(selected.classList.contains('is-selected'), true);
  assert.equal(page.nodes.get('#demo-status').textContent, '2 个匹配的示例标签页');
  assert.equal(page.document.activeElement, selected);
});
