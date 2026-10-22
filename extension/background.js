// MV3 service worker entry point. The core is kept in a separate file so it can be unit-tested.
importScripts('background-core.js');

const api = globalThis.chrome;
let bridgeStartup = null;

async function startBridge() {
  if (!api || !api.runtime || !api.storage) return null;
  if (globalThis.glintBridge) return globalThis.glintBridge;
  if (bridgeStartup) return bridgeStartup;
  bridgeStartup = (async () => {
    const identity = await GlintExtension.loadIdentity(api.storage.local);
    const bridge = new GlintExtension.TabBridge({
      api,
      profileId: identity.profileId,
      profileLabel: identity.profileLabel,
      browser: GlintExtension.browserNameFromUserAgent(globalThis.navigator && globalThis.navigator.userAgent),
    });
    bridge.attachListeners();
    globalThis.glintBridge = bridge;
    await bridge.connect();
    return bridge;
  })().catch(error => {
    console.error('Glint extension startup failed', error);
    return null;
  });
  const result = await bridgeStartup;
  bridgeStartup = null;
  return result;
}

if (api && api.runtime) {
  // These events wake a dormant MV3 service worker after browser/extension startup.
  api.runtime.onStartup && api.runtime.onStartup.addListener(() => { void startBridge(); });
  api.runtime.onInstalled && api.runtime.onInstalled.addListener(() => { void startBridge(); });
  // action.default_popup owns the toolbar surface. Connecting never opens tabs.
  api.runtime.onMessage && api.runtime.onMessage.addListener((message, _sender, sendResponse) => {
    if (!message || !['profile_label_changed', 'glint_connection_status', 'glint_retry_connection'].includes(message.type)) return undefined;
    (async () => {
      const bridge = await startBridge();
      if (!bridge) throw new Error('bridge is not available');
      if (message.type !== 'profile_label_changed') {
        if (message.type === 'glint_retry_connection') await bridge.retryConnect();
        if (sendResponse) sendResponse({ok: true, portOpen: bridge.connected,
          desktopConnected: bridge.desktopConnected,
          connecting: bridge.connecting, browser: bridge.browser, error: bridge.lastError});
        return;
      }
      bridge.profileLabel = typeof message.profileLabel === 'string' ? message.profileLabel.trim() : '';
      // A second hello resets the desktop connection metadata; follow it with a complete snapshot.
      bridge.sendHello();
      if (bridge.desktopConnected) await bridge.syncSnapshot();
      if (sendResponse) sendResponse({ok: true, portOpen: bridge.connected, desktopConnected: bridge.desktopConnected});
    })().catch(error => {
      if (sendResponse) sendResponse({ok: false, error: String(error && error.message || error)});
    });
    return true;
  });
  void startBridge();
}
