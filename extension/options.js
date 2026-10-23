(() => {
  'use strict';
  const elements = {};
  for (const id of ['browser-name', 'extension-id', 'copy-id', 'copy-status', 'connection-status',
    'connection-detail', 'status-dot', 'retry', 'profile-form', 'profile-label', 'save-status']) {
    elements[id] = document.getElementById(id);
  }
  let language = GlintExtension.resolveLanguage('system', GlintExtension.systemLanguage(chrome));
  let hasWorkerLanguage = false;
  let languageRevision = -1;
  let languageSession;
  let connectionResponse;
  let checkingConnection = false;
  let retryingConnection = false;
  const feedback = new Map();
  const t = (key, values) => GlintExtension.translate(language, key, values);

  function showFeedback(id, key, values) {
    feedback.set(id, {key, values});
    elements[id].textContent = t(key, values);
  }

  function applyLanguage(value) {
    if (value !== 'en' && value !== 'zh-CN') return;
    language = value;
    document.documentElement.lang = language;
    for (const element of document.querySelectorAll('[data-i18n], [data-i18n-aria-label], [data-i18n-placeholder]')) {
      if (element.dataset.i18n) element.textContent = t(element.dataset.i18n);
      if (element.dataset.i18nAriaLabel) element.setAttribute('aria-label', t(element.dataset.i18nAriaLabel));
      if (element.dataset.i18nPlaceholder) element.setAttribute('placeholder', t(element.dataset.i18nPlaceholder));
    }
    for (const [id, entry] of feedback) elements[id].textContent = t(entry.key, entry.values);
    if (connectionResponse !== undefined) renderConnection(connectionResponse);
    else elements['connection-detail'].textContent = t('waking');
    if (checkingConnection || connectionResponse === undefined) {
      elements['connection-status'].textContent = t(retryingConnection ? 'retrying' : 'checking');
    }
  }

  function applyWorkerLanguage(message) {
    if (!message || !['en', 'zh-CN'].includes(message.language)) return;
    if (Number.isSafeInteger(message.languageRevision)) {
      if (message.languageSession === languageSession && message.languageRevision < languageRevision) return;
      languageRevision = message.languageRevision;
      languageSession = message.languageSession;
    }
    hasWorkerLanguage = true;
    applyLanguage(message.language);
  }
  const edge = /Edg\//i.test(navigator.userAgent || '');
  elements['browser-name'].textContent = edge ? 'Microsoft Edge' : 'Google Chrome';
  elements['extension-id'].value = chrome.runtime.id;

  function setConnectionState(state) {
    if (document.documentElement) document.documentElement.dataset.connection = state;
  }

  elements['copy-id'].addEventListener('click', async () => {
    try {
      await navigator.clipboard.writeText(chrome.runtime.id);
      showFeedback('copy-status', 'copySuccess');
    } catch (_) {
      elements['extension-id'].focus();
      elements['extension-id'].select();
      showFeedback('copy-status', 'copyFailed');
    }
  });

  function renderConnection(response) {
    connectionResponse = response || {};
    const portOpen = Boolean(response && response.ok && response.portOpen);
    const desktopConnected = portOpen && response.desktopConnected === true;
    setConnectionState(desktopConnected ? 'connected' : portOpen && response.desktopConnected === false ? 'waiting' : 'error');
    elements['status-dot'].classList.toggle('good', desktopConnected);
    if (portOpen && response.desktopConnected === false) {
      elements['connection-status'].textContent = t('waiting');
      elements['connection-detail'].textContent = t('waitingDetail');
    } else if (desktopConnected) {
      elements['connection-status'].textContent = t('connected');
      elements['connection-detail'].textContent = t('connectedDetail');
    } else if (portOpen) {
      elements['connection-status'].textContent = t('reload');
      elements['connection-detail'].textContent = t('reloadDetail', {browser: edge ? 'Edge' : 'Chrome'});
    } else {
      elements['connection-status'].textContent = t('noBridge');
      const errorKey = response && ({native_closed: 'nativeClosed', bridge_unavailable: 'bridgeUnavailable'})[response.errorCode];
      const error = errorKey ? t(errorKey) : response && response.error;
      const missingHost = /\bnative(?: messaging)? host not found\b/i.test(error || '');
      const guidance = t(missingHost ? 'missingHost' : 'bridgeHelp');
      elements['connection-detail'].textContent = guidance + (error ? t(errorKey ? 'connectionError' : 'browserError', {error}) : '');
    }
  }

  async function checkConnection(retry = false) {
    if (checkingConnection) return;
    checkingConnection = true;
    retryingConnection = retry;
    setConnectionState('checking');
    elements.retry.disabled = true;
    elements['connection-status'].textContent = t(retry ? 'retrying' : 'checking');
    try {
      const response = await chrome.runtime.sendMessage({type: retry ? 'glint_retry_connection' : 'glint_connection_status'});
      applyWorkerLanguage(response);
      renderConnection(response);
    } catch (error) {
      renderConnection({error: String(error && error.message || error)});
    } finally {
      elements.retry.disabled = false;
      checkingConnection = false;
    }
  }
  elements.retry.addEventListener('click', () => checkConnection(true));
  document.addEventListener('visibilitychange', () => { if (!document.hidden) void checkConnection(); });
  // connectNative may return a port before the browser reports an invalid registration.
  // Refresh while visible so that the asynchronous disconnect is reflected here too.
  setInterval(() => { if (!document.hidden) void checkConnection(); }, 3000);

  chrome.runtime.onMessage && chrome.runtime.onMessage.addListener(message => {
    if (message && message.type === 'glint_language_changed') applyWorkerLanguage(message);
  });
  elements['profile-form'].addEventListener('submit', async event => {
    event.preventDefault();
    const profileLabel = elements['profile-label'].value.trim();
    try {
      await chrome.storage.local.set({profileLabel});
    } catch (error) {
      showFeedback('save-status', 'saveFailed', {error: String(error && error.message || error)});
      return;
    }
    try {
      const response = await chrome.runtime.sendMessage({type: 'profile_label_changed', profileLabel});
      showFeedback('save-status', response && response.ok && response.portOpen && response.desktopConnected === true
        ? 'saved' : 'savedLater');
    } catch (_) {
      showFeedback('save-status', 'savedLater');
    }
  });
  applyLanguage(language);
  chrome.storage.local.get(['profileLabel', 'desktopLanguage']).then(values => {
    elements['profile-label'].value = values.profileLabel || '';
    if (!hasWorkerLanguage && GlintExtension.validDesktopLanguage(values.desktopLanguage)) applyLanguage(values.desktopLanguage.language);
  }).catch(() => {
    showFeedback('save-status', 'readLabelFailed');
  }).finally(() => {
    void checkConnection();
  });
})();
