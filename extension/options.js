(() => {
  'use strict';
  const elements = {};
  for (const id of ['browser-name', 'extension-id', 'copy-id', 'copy-status', 'connection-status',
    'connection-detail', 'status-dot', 'retry', 'profile-form', 'profile-label', 'save-status']) {
    elements[id] = document.getElementById(id);
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
      elements['copy-status'].textContent = '扩展 ID 已复制。请回到 Glint 的确认连接页，点击“连接遇到问题”，粘贴并保存，再回来重试连接。';
    } catch (_) {
      elements['extension-id'].focus();
      elements['extension-id'].select();
      elements['copy-status'].textContent = '复制失败，ID 已选中。请按 Ctrl+C 复制。';
    }
  });

  function renderConnection(response) {
    const portOpen = Boolean(response && response.ok && response.portOpen);
    const desktopConnected = portOpen && response.desktopConnected === true;
    setConnectionState(desktopConnected ? 'connected' : portOpen && response.desktopConnected === false ? 'waiting' : 'error');
    elements['status-dot'].classList.toggle('good', desktopConnected);
    if (portOpen && response.desktopConnected === false) {
      elements['connection-status'].textContent = 'Glint 未运行，等待自动连接';
      elements['connection-detail'].textContent = '启动 Glint 后会自动连接并同步标签页，无需重新加载扩展。';
    } else if (desktopConnected) {
      elements['connection-status'].textContent = '已连接 Glint';
      elements['connection-detail'].textContent = '请回到 Glint，确认能搜索到当前浏览器的标签页，以验证同步结果。';
    } else if (portOpen) {
      elements['connection-status'].textContent = '扩展需要重新加载';
      elements['connection-detail'].textContent = `状态面板与后台版本不一致，暂时无法确认 Glint 的连接状态。请在 ${edge ? 'Edge' : 'Chrome'} 扩展管理页找到 Glint，点击“重新加载”，再打开此面板。`;
    } else {
      elements['connection-status'].textContent = '尚未建立桥接通道';
      const error = response && response.error;
      const missingHost = /\bnative(?: messaging)? host not found\b/i.test(error || '');
      const guidance = missingHost
        ? '浏览器找不到 Glint 本机通信桥。请返回 Glint 的确认连接页，点击“连接遇到问题”，粘贴下方扩展 ID 并保存，再回到此页重试。若仍出现同一错误，请检查是否使用同一 Windows 用户，以及浏览器是否允许当前用户的 Native Messaging 注册。'
        : '请确认 Glint 正在运行，并在“连接新的浏览器”引导中选择了当前浏览器，然后点击“重试连接”。如果仍未连接，可复制下方扩展 ID，在 Glint 的确认连接页点击“连接遇到问题”，粘贴并保存。';
      elements['connection-detail'].textContent = guidance + (error ? ` 浏览器返回：${error}` : '');
    }
  }

  let checkingConnection = false;
  async function checkConnection(retry = false) {
    if (checkingConnection) return;
    checkingConnection = true;
    setConnectionState('checking');
    elements.retry.disabled = true;
    elements['connection-status'].textContent = retry ? '正在重试连接…' : '正在检查连接…';
    try {
      const response = await chrome.runtime.sendMessage({type: retry ? 'glint_retry_connection' : 'glint_connection_status'});
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

  chrome.storage.local.get(['profileLabel']).then(values => {
    elements['profile-label'].value = values.profileLabel || '';
  }).catch(() => { elements['save-status'].textContent = '读取标签失败，可重试打开此页面。'; });
  elements['profile-form'].addEventListener('submit', async event => {
    event.preventDefault();
    const profileLabel = elements['profile-label'].value.trim();
    try {
      await chrome.storage.local.set({profileLabel});
    } catch (error) {
      elements['save-status'].textContent = `保存失败：${String(error && error.message || error)}`;
      return;
    }
    try {
      const response = await chrome.runtime.sendMessage({type: 'profile_label_changed', profileLabel});
      elements['save-status'].textContent = response && response.ok && response.portOpen && response.desktopConnected === true
        ? '已保存' : '已保存，将在下次连接时生效';
    } catch (_) {
      elements['save-status'].textContent = '已保存，将在下次连接时生效';
    }
  });
  void checkConnection();
})();
