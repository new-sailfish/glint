(() => {
  const form = document.getElementById('profile-form');
  const input = document.getElementById('profile-label');
  const status = document.getElementById('status');
  const storage = chrome.storage.local;
  storage.get(['profileLabel']).then(values => { input.value = values.profileLabel || ''; });
  form.addEventListener('submit', async event => {
    event.preventDefault();
    const profileLabel = input.value.trim();
    await storage.set({profileLabel});
    try {
      await chrome.runtime.sendMessage({type: 'profile_label_changed', profileLabel});
    } catch (_) {
      // The service worker may be asleep; the next connection reads storage.local.
    }
    status.textContent = 'Saved';
    setTimeout(() => { status.textContent = ''; }, 1800);
  });
})();
