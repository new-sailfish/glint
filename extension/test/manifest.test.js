const test = require('node:test');
const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');
const extensionDirectory = path.resolve(__dirname, '..');
const manifest = JSON.parse(fs.readFileSync(path.join(extensionDirectory, 'manifest.json'), 'utf8'));

test('extension-manager metadata has English and both Chinese browser locale catalogs', () => {
  assert.equal(manifest.default_locale, 'en');
  for (const locale of ['en', 'zh_CN', 'zh_TW']) {
    const catalog = JSON.parse(fs.readFileSync(path.join(extensionDirectory, '_locales', locale, 'messages.json'), 'utf8'));
    for (const value of [manifest.name, manifest.description, manifest.action.default_title]) {
      const match = /^__MSG_(\w+)__$/.exec(value);
      assert.ok(match, `${value} must reference a localized message`);
      assert.ok(catalog[match[1]]?.message, `${locale} must supply ${match[1]}`);
    }
  }
});

test('every manifest runtime path names an existing bundled file', () => {
  const files = new Set([manifest.background.service_worker, manifest.options_page,
    manifest.action.default_popup, ...Object.values(manifest.icons), ...Object.values(manifest.action.default_icon)]);
  for (const filename of files) {
    assert.equal(typeof filename, 'string');
    const file = path.resolve(extensionDirectory, filename);
    assert.ok(file.startsWith(extensionDirectory + path.sep), `${filename} must stay inside the extension bundle`);
    assert.ok(fs.statSync(file).isFile(), `${filename} must be packaged with the extension`);
  }
});

test('extension and toolbar icons are PNGs matching their declared sizes', () => {
  assert.deepEqual(Object.keys(manifest.icons).map(Number).sort((a, b) => a - b), [16, 32, 48, 128]);
  const signature = Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]);
  for (const icons of [manifest.icons, manifest.action.default_icon]) {
    for (const [size, filename] of Object.entries(icons)) {
      const png = fs.readFileSync(path.join(extensionDirectory, filename));
      assert.ok(png.length >= 33, `${filename} must contain a PNG header`);
      assert.deepEqual(png.subarray(0, 8), signature, `${filename} must be a PNG`);
      assert.equal(png.readUInt32BE(8), 13, `${filename} must have a complete IHDR chunk`);
      assert.equal(png.toString('ascii', 12, 16), 'IHDR');
      assert.equal(png.readUInt32BE(16), Number(size), `${filename} width must match the manifest`);
      assert.equal(png.readUInt32BE(20), Number(size), `${filename} height must match the manifest`);
    }
  }
});

test('bundled extension public key fixes the same ID as the native host', () => {
  assert.equal(typeof manifest.key, 'string', 'manifest must contain the bundled public key');
  const publicDer = Buffer.from(manifest.key, 'base64');
  assert.equal(publicDer.toString('base64'), manifest.key, 'key uses canonical base64');
  const publicKey = crypto.createPublicKey({key: publicDer, format: 'der', type: 'spki'});
  assert.equal(publicKey.type, 'public');
  assert.equal(publicKey.asymmetricKeyType, 'rsa');
  assert.ok(publicKey.asymmetricKeyDetails.modulusLength >= 2048);
  assert.deepEqual(publicKey.export({format: 'der', type: 'spki'}), publicDer);

  const extensionId = [...crypto.createHash('sha256').update(publicDer).digest().subarray(0, 16)]
    .flatMap(byte => [byte >> 4, byte & 15])
    .map(nibble => String.fromCharCode('a'.charCodeAt(0) + nibble)).join('');
  const header = fs.readFileSync(path.join(__dirname, '../../src/extension_identity.hpp'), 'utf8');
  const nativeId = header.match(/inline constexpr char kBundledExtensionId\[\]\s*=\s*"([a-p]{32})"/);
  assert.ok(nativeId, 'native host must publish its bundled extension ID');
  assert.equal(extensionId, nativeId[1], 'native host allowed origin must match the public key');
});
