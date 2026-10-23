// MV3 service worker entry point. The core is kept in a separate file so it can be unit-tested.
importScripts('background-core.js');

const api = globalThis.chrome;
let bridgeStartup = null;
let currentLanguage = GlintExtension.resolveLanguage('system', GlintExtension.systemLanguage(api));
let languageRevision = 0;
const languageSession = GlintExtension.makeProfileId();
let languageWrites = Promise.resolve();

function updateLanguage(language) {
  currentLanguage = language;
  if (api.action && api.action.setTitle) {
    Promise.resolve().then(() => api.action.setTitle({title: GlintExtension.translate(language, 'toolbarTitle')})).catch(() => {});
  }
}

function receiveDesktopLanguage(settings) {
  updateLanguage(settings.language);
  languageRevision++;
  if (api.runtime.sendMessage) {
    const notification = {type: 'glint_language_changed', language: currentLanguage, languageRevision, languageSession};
    Promise.resolve().then(() => api.runtime.sendMessage(notification)).catch(() => {}); // No open popup is a normal case.
  }
  // Serialize writes so a slower old preference can never overwrite a newer one.
  // Cache failures must not affect live language updates or tab synchronization.
  languageWrites = languageWrites.then(() => api.storage.local.set({desktopLanguage: settings})).catch(() => {});
}

async function startBridge() {
  if (!api || !api.runtime || !api.storage) return null;
  if (globalThis.glintBridge) return globalThis.glintBridge;
  if (bridgeStartup) return bridgeStartup;
  bridgeStartup = (async () => {
    const preferences = await api.storage.local.get(['desktopLanguage']).catch(() => ({}));
    updateLanguage(GlintExtension.validDesktopLanguage(preferences.desktopLanguage)
      ? preferences.desktopLanguage.language : GlintExtension.resolveLanguage('system', GlintExtension.systemLanguage(api)));
    const identity = await GlintExtension.loadIdentity(api.storage.local);
    const bridge = new GlintExtension.TabBridge({
      api,
      profileId: identity.profileId,
      profileLabel: identity.profileLabel,
      language: currentLanguage,
      onLanguageChanged: receiveDesktopLanguage,
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
      if (!bridge) {
        if (sendResponse) sendResponse({ok: false, errorCode: 'bridge_unavailable', language: currentLanguage, languageRevision, languageSession});
        return;
      }
      if (message.type !== 'profile_label_changed') {
        if (message.type === 'glint_retry_connection') await bridge.retryConnect();
        if (sendResponse) sendResponse({ok: true, portOpen: bridge.connected,
          desktopConnected: bridge.desktopConnected,
          connecting: bridge.connecting, browser: bridge.browser,
          language: currentLanguage, languageRevision, languageSession,
          error: bridge.lastErrorCode === 'native_closed'
            ? GlintExtension.translate(bridge.language, 'nativeClosed') : bridge.lastError,
          ...(bridge.lastErrorCode ? {errorCode: bridge.lastErrorCode} : {})});
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
