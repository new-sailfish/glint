/* Progressive enhancement. Stores only the website language preference. */
(() => {
  'use strict';

  const i18n = window.GlintSiteI18n;
  const languageControl = document.querySelector('#site-language');
  let storage;
  try { storage = window.localStorage; } catch { /* Storage may be disabled. */ }
  let preference = i18n.readPreference(storage);
  let language = i18n.resolveLanguage(preference, navigator.language);
  const t = (key, values) => i18n.message(key, language, values);

  const demo = document.querySelector('#search-demo');
  const input = document.querySelector('#demo-query');
  const clear = document.querySelector('#demo-clear');
  const status = document.querySelector('#demo-status');
  const empty = document.querySelector('#demo-empty');
  const rows = [...document.querySelectorAll('#demo-results > li')];
  const searchTitles = new Map(rows.map(row => [row, [row.dataset.title,
    i18n.translate(row.dataset.title, 'en')].join(' ').toLowerCase()]));
  const visibleRows = () => rows.filter(row => !row.hidden);
  const resultButton = row => row.querySelector('button');
  let activatedRow = null;

  function select(button) {
    rows.forEach(row => resultButton(row).classList.remove('is-selected'));
    if (button) button.classList.add('is-selected');
  }

  function filterResults(preserveSelection = false) {
    const query = input.value.toLowerCase();
    rows.forEach(row => {
      const title = searchTitles.get(row);
      const url = row.dataset.url.toLowerCase();
      row.hidden = !title.includes(query) && !url.includes(query);
    });
    if (!preserveSelection) {
      select(null);
      activatedRow = null;
    }
    const count = visibleRows().length;
    empty.hidden = count !== 0;
    clear.hidden = input.value.length === 0;
    status.textContent = activatedRow && !activatedRow.hidden
      ? t('activated', { title: resultButton(activatedRow).querySelector('strong').textContent })
      : t(query.length ? 'matchCount' : 'sampleCount', { count });
  }

  function resetDemo() {
    input.value = '';
    filterResults();
    input.focus();
  }

  function activate(button) {
    select(button);
    activatedRow = rows.find(row => resultButton(row) === button);
    const title = button.querySelector('strong').textContent;
    status.textContent = t('activated', { title });
  }

  input.addEventListener('input', () => filterResults());
  clear.addEventListener('click', resetDemo);
  rows.forEach(row => {
    const button = resultButton(row);
    button.addEventListener('focus', () => select(button));
    button.addEventListener('click', () => activate(button));
  });

  // Keyboard shortcuts belong to this demonstration, never the whole page.
  demo.addEventListener('keydown', event => {
    if (event.isComposing || event.altKey || event.ctrlKey || event.metaKey) return;
    if (event.key === 'Escape') {
      event.preventDefault();
      resetDemo();
      return;
    }
    const buttons = visibleRows().map(resultButton);
    const index = buttons.indexOf(event.target);
    if (event.target === input) {
      if (event.key === 'ArrowDown' && buttons.length) {
        event.preventDefault();
        buttons[0].focus();
      } else if (event.key === 'Enter' && buttons.length) {
        event.preventDefault();
        activate(buttons[0]);
      }
      return;
    }
    if (index < 0) return;
    if (event.key === 'ArrowDown') {
      event.preventDefault();
      buttons[Math.min(index + 1, buttons.length - 1)].focus();
    } else if (event.key === 'ArrowUp') {
      event.preventDefault();
      if (index === 0) input.focus();
      else buttons[index - 1].focus();
    } else if (event.key === 'Home' || event.key === 'PageUp') {
      event.preventDefault();
      if (index === 0) input.focus();
      else buttons[0].focus();
    } else if (event.key === 'End') {
      event.preventDefault();
      buttons[buttons.length - 1].focus();
    }
  });

  const browserCopy = {
    chrome: {
      name: 'Chrome',
      address: 'chrome://extensions/',
    },
    edge: {
      name: 'Edge',
      address: 'edge://extensions/',
    },
  };
  const browserButtons = [...document.querySelectorAll('[data-browser]')];
  let selectedBrowser = 'chrome';

  function updateBrowser() {
    const copy = {
      ...browserCopy[selectedBrowser],
      developer: t(`${selectedBrowser}Developer`),
      mode: t(`${selectedBrowser}Mode`),
      load: t(`${selectedBrowser}Load`),
    };
    document.querySelectorAll('[data-browser-field]').forEach(field => {
      field.textContent = copy[field.dataset.browserField];
    });
  }

  browserButtons.forEach(button => button.addEventListener('click', () => {
    selectedBrowser = button.dataset.browser;
    browserButtons.forEach(item => item.setAttribute('aria-pressed', String(item === button)));
    updateBrowser();
  }));

  const guide = document.querySelector('#setup-guide');
  const panels = [...guide.querySelectorAll('.setup-panel')];
  const stepLinks = [...guide.querySelectorAll('[data-step]')];
  const back = document.querySelector('#setup-back');
  const next = document.querySelector('#setup-next');
  const progress = document.querySelector('#setup-progress');
  let step = 0;

  function showStep(index) {
    step = Math.max(0, Math.min(index, panels.length - 1));
    panels.forEach((panel, position) => {
      panel.hidden = position !== step;
      panel.classList.toggle('is-active', position === step);
    });
    stepLinks.forEach((link, position) => {
      if (position === step) link.setAttribute('aria-current', 'step');
      else link.removeAttribute('aria-current');
    });
    back.disabled = step === 0;
    next.disabled = step === panels.length - 1;
    next.textContent = t(step === panels.length - 1 ? 'lastStep' : 'next');
    if (step !== panels.length - 1) {
      const arrow = document.createElement('span');
      arrow.setAttribute('aria-hidden', 'true');
      arrow.textContent = '→';
      next.append(arrow);
    }
    progress.textContent = t('progress', { step: step + 1, total: panels.length });
  }

  stepLinks.forEach((link, index) => link.addEventListener('click', event => {
    event.preventDefault();
    showStep(index);
  }));
  back.addEventListener('click', () => {
    showStep(step - 1);
    if (back.disabled) stepLinks[step].focus({ preventScroll: true });
  });
  next.addEventListener('click', () => {
    showStep(step + 1);
    if (next.disabled) stepLinks[step].focus({ preventScroll: true });
  });
  guide.classList.add('is-enhanced');
  guide.querySelector('.setup-controls').hidden = false;
  const initialStep = panels.findIndex(panel => `#${panel.id}` === window.location.hash);
  step = initialStep >= 0 ? initialStep : 0;

  function applyLanguage() {
    language = i18n.resolveLanguage(preference, navigator.language);
    languageControl.value = preference;
    i18n.translatePage(document, language);
    updateBrowser();
    filterResults(true);
    showStep(step);
  }

  languageControl.addEventListener('change', () => {
    preference = i18n.normalizePreference(languageControl.value);
    i18n.writePreference(storage, preference);
    applyLanguage();
  });
  window.addEventListener('languagechange', () => {
    if (preference === 'system') applyLanguage();
  });
  window.addEventListener('storage', event => {
    if (event.key === i18n.storageKey || event.key === null) {
      preference = i18n.readPreference(storage);
      applyLanguage();
    }
  });
  applyLanguage();

  if ('IntersectionObserver' in window && !window.matchMedia('(prefers-reduced-motion: reduce)').matches) {
    const observer = new IntersectionObserver(entries => {
      entries.forEach(entry => {
        if (!entry.isIntersecting) return;
        entry.target.classList.add('is-visible');
        observer.unobserve(entry.target);
      });
    }, { threshold: 0.16 });
    document.querySelectorAll('.reveal').forEach(element => observer.observe(element));
  }
})();
