'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const i18n = require('./i18n.js');

test('Chinese system variants use Chinese; all other system languages use English', () => {
  for (const locale of ['zh', 'zh-CN', 'zh-TW', 'zh-HK', 'zh-Hans', 'zh-Hant-TW', 'ZH_cn']) {
    assert.equal(i18n.resolveLanguage('system', locale), 'zh-CN', locale);
  }
  for (const locale of ['en', 'en-GB', 'fr-FR', 'ja-JP', 'de', '', undefined, 'zho']) {
    assert.equal(i18n.resolveLanguage('system', locale), 'en', locale);
  }
});

test('explicit choices override the system and survive storage round trips', () => {
  const values = new Map();
  const storage = { getItem: key => values.get(key), setItem: (key, value) => values.set(key, value) };
  assert.equal(i18n.readPreference(storage), 'system');
  for (const [preference, system, expected] of [
    ['en', 'zh-CN', 'en'], ['zh-CN', 'en-US', 'zh-CN'], ['system', 'zh-TW', 'zh-CN'],
  ]) {
    assert.equal(i18n.writePreference(storage, preference), true);
    assert.equal(i18n.readPreference(storage), preference);
    assert.equal(i18n.resolveLanguage(i18n.readPreference(storage), system), expected);
  }
});

test('unsupported preferences and unavailable storage safely follow the system', () => {
  const blockedStorage = { getItem() { throw new Error('blocked'); }, setItem() { throw new Error('blocked'); } };
  assert.equal(i18n.readPreference(blockedStorage), 'system');
  assert.equal(i18n.writePreference(blockedStorage, 'en'), false);
  assert.equal(i18n.readPreference(undefined), 'system');
  assert.equal(i18n.readPreference({ getItem: () => 'fr' }), 'system');
  assert.equal(i18n.resolveLanguage('invalid', 'zh-HK'), 'zh-CN');
});

test('all Chinese page text and accessible attributes have English translations', () => {
  const html = fs.readFileSync(path.join(__dirname, 'index.html'), 'utf8');
  const text = [...html.matchAll(/>([^<>]+)</g)].map(match => match[1].trim());
  const attributes = [...html.matchAll(/(?:aria-label|alt|title|placeholder|content|data-title)="([^"]*)"/g)].map(match => match[1]);
  for (const source of [...text, ...attributes].filter(value => /[\u4e00-\u9fff]/.test(value))) {
    // The language selector always shows each language's own name.
    if (source === '中文') continue;
    assert.ok(Object.hasOwn(i18n.english, source), `Missing English translation: ${source}`);
    assert.doesNotMatch(i18n.translate(source, 'en'), /[\u4e00-\u9fff]/, source);
    assert.equal(i18n.translate(source, 'zh-CN'), source);
  }
});

test('dynamic status and setup messages cover the same keys and placeholders in both languages', () => {
  assert.deepEqual(Object.keys(i18n.messages.en).sort(), Object.keys(i18n.messages['zh-CN']).sort());
  const placeholders = value => [...value.matchAll(/\{\w+\}/g)].map(match => match[0]).sort();
  for (const key of Object.keys(i18n.messages.en)) {
    assert.deepEqual(placeholders(i18n.messages.en[key]), placeholders(i18n.messages['zh-CN'][key]), key);
  }
  assert.equal(i18n.message('matchCount', 'en', { count: 3 }), '3 matching sample tabs');
  assert.equal(i18n.message('progress', 'zh-CN', { step: 2, total: 5 }), '步骤 2 / 5');
  assert.equal(i18n.message('activated', 'en', { title: '<sample>' }), 'Demo: selected “<sample>”. Glint would switch to that tab.');
});

test('screenshots, illustrations and linked previews follow the selected language', () => {
  for (const name of ['search', 'setup-browser', 'setup-load', 'setup-connected', 'hotkey']) {
    assert.equal(i18n.imagePath(`assets/${name}.png`, 'en'), `assets/${name}-en.png`);
    assert.equal(i18n.imagePath(`assets/${name}-en.png`, 'zh-CN'), `assets/${name}.png`);
  }
  assert.equal(i18n.imagePath('assets/tab-overload.svg', 'en'), 'assets/tab-overload-en.svg');
  assert.equal(i18n.imagePath('https://example.com/assets/search.png', 'en'), 'https://example.com/assets/search-en.png');
  assert.equal(i18n.imagePath('assets/glint-logo.svg', 'en'), 'assets/glint-logo.svg');
});
