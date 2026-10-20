const test = require('node:test');
const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');

test('bundled extension public key fixes the same ID as the native host', () => {
  const manifest = JSON.parse(fs.readFileSync(path.join(__dirname, '../manifest.json'), 'utf8'));
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
