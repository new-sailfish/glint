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
  const openSetup = () => {
    Promise.resolve(api.runtime.openOptionsPage()).catch(error => {
      console.error('Glint setup page could not open', error);
    });
  };
  // These events wake a dormant MV3 service worker after browser/extension startup.
  api.runtime.onStartup && api.runtime.onStartup.addListener(() => { void startBridge(); });
  api.runtime.onInstalled && api.runtime.onInstalled.addListener(details => {
    if (details && details.reason === 'install') openSetup();
    void startBridge();
  });
  api.action && api.action.onClicked && api.action.onClicked.addListener(openSetup);
  api.runtime.onMessage && api.runtime.onMessage.addListener((message, _sender, sendResponse) => {
    if (!message || !['profile_label_changed', 'glint_connection_status', 'glint_retry_connection'].includes(message.type)) return undefined;
    (async () => {
      const bridge = await startBridge();
      if (!bridge) throw new Error('bridge is not available');
      if (message.type !== 'profile_label_changed') {
        if (message.type === 'glint_retry_connection') await bridge.retryConnect();
        if (sendResponse) sendResponse({ok: true, portOpen: bridge.connected,
          connecting: bridge.connecting, browser: bridge.browser, error: bridge.lastError});
        return;
      }
      bridge.profileLabel = typeof message.profileLabel === 'string' ? message.profileLabel.trim() : '';
      // A second hello resets the desktop connection metadata; follow it with a complete snapshot.
      bridge.sendHello();
      if (bridge.connected) await bridge.syncSnapshot();
      if (sendResponse) sendResponse({ok: true});
    })().catch(error => {
      if (sendResponse) sendResponse({ok: false, error: String(error && error.message || error)});
    });
    return true;
  });
  void startBridge();
}
