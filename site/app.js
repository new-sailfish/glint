/* Progressive enhancement only. No browser data, requests, or storage. */
(() => {
  'use strict';

  const demo = document.querySelector('#search-demo');
  const input = document.querySelector('#demo-query');
  const clear = document.querySelector('#demo-clear');
  const status = document.querySelector('#demo-status');
  const empty = document.querySelector('#demo-empty');
  const rows = [...document.querySelectorAll('#demo-results > li')];
  const visibleRows = () => rows.filter(row => !row.hidden);
  const resultButton = row => row.querySelector('button');

  function select(button) {
    rows.forEach(row => resultButton(row).classList.remove('is-selected'));
    if (button) button.classList.add('is-selected');
  }

  function filterResults() {
    const query = input.value.toLowerCase();
    rows.forEach(row => {
      const title = row.dataset.title.toLowerCase();
      const url = row.dataset.url.toLowerCase();
      row.hidden = !title.includes(query) && !url.includes(query);
    });
    select(null);
    const count = visibleRows().length;
    empty.hidden = count !== 0;
    clear.hidden = input.value.length === 0;
    status.textContent = query.length ? `${count} 个匹配的示例标签页` : `${count} 个示例标签页`;
  }

  function resetDemo() {
    input.value = '';
    filterResults();
    input.focus();
  }

  function activate(button) {
    select(button);
    const title = button.querySelector('strong').textContent;
    status.textContent = `演示：已选择“${title}”。在 Glint 中，这一步会切换到对应标签页。`;
  }

  input.addEventListener('input', filterResults);
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
      developer: '在 Chrome 扩展页面右上角，开启“开发者模式”（Developer mode）。',
      mode: '开发者模式',
      load: '加载已解压的扩展程序',
    },
    edge: {
      name: 'Edge',
      address: 'edge://extensions/',
      developer: '在 Edge 扩展页面的左侧栏中，开启“开发人员模式”（Developer mode）。',
      mode: '开发人员模式',
      load: '加载解压缩的扩展',
    },
  };
  const browserButtons = [...document.querySelectorAll('[data-browser]')];
  browserButtons.forEach(button => button.addEventListener('click', () => {
    const copy = browserCopy[button.dataset.browser];
    browserButtons.forEach(item => item.setAttribute('aria-pressed', String(item === button)));
    document.querySelectorAll('[data-browser-field]').forEach(field => {
      field.textContent = copy[field.dataset.browserField];
    });
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
    next.innerHTML = step === panels.length - 1 ? '已到最后一步' : '下一步<span aria-hidden="true">→</span>';
    progress.textContent = `步骤 ${step + 1} / ${panels.length}`;
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
  showStep(initialStep >= 0 ? initialStep : 0);

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
