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

  elements['copy-id'].addEventListener('click', async () => {
    try {
      await navigator.clipboard.writeText(chrome.runtime.id);
      elements['copy-status'].textContent = '扩展 ID 已复制。回到 Glint，打开“连接浏览器”并粘贴。';
    } catch (_) {
      elements['extension-id'].focus();
      elements['extension-id'].select();
      elements['copy-status'].textContent = '复制失败，ID 已选中。请按 Ctrl+C 复制。';
    }
  });

  function renderConnection(response) {
    const portOpen = Boolean(response && response.ok && response.portOpen);
    elements['status-dot'].classList.toggle('good', portOpen);
    if (portOpen) {
      elements['connection-status'].textContent = '桥接通道已打开';
      elements['connection-detail'].textContent = '扩展端口已建立。请回到 Glint，确认标签页已出现在搜索列表中；这里不能单独确认桌面端同步完成。';
    } else {
      elements['connection-status'].textContent = '尚未建立桥接通道';
      const error = response && response.error;
      elements['connection-detail'].textContent = '请确认 Glint 正在运行，并已使用当前扩展 ID 注册。' + (error ? ` 浏览器返回：${error}` : '');
    }
  }

  let checkingConnection = false;
  async function checkConnection(retry = false) {
    if (checkingConnection) return;
    checkingConnection = true;
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
      elements['save-status'].textContent = response && response.ok && response.portOpen
        ? '已保存' : '已保存，将在下次连接时生效';
    } catch (_) {
      elements['save-status'].textContent = '已保存，将在下次连接时生效';
    }
  });
  void checkConnection();
})();
