import './ui.js';

export function slides(deck) {
  const pages = [...deck.querySelectorAll('.kr-slide')];
  if (!pages.length) return;
  const controls = document.createElement('nav');
  controls.className = 'kr-row kr-controls';
  controls.setAttribute('aria-label', 'Slides');
  const overview = document.createElement('div');
  overview.className = 'kr-overview';
  overview.hidden = true;
  const status = document.createElement('output');
  status.setAttribute('aria-live', 'polite');
  let current = 0;
  function button(label, click) {
    const control = document.createElement('button');
    control.type = 'button';
    control.textContent = label;
    control.addEventListener('click', click);
    return control;
  }
  function show(index) {
    current = Math.max(0, Math.min(pages.length - 1, index));
    pages.forEach((page, index) => {
      page.hidden = index !== current;
      const entry = overview.children[index];
      if (index === current) entry.setAttribute('aria-current', 'true');
      else entry.removeAttribute('aria-current');
    });
    status.value = (current + 1) + ' / ' + pages.length;
    previous.disabled = current === 0;
    next.disabled = current === pages.length - 1;
  }
  const previous = button('Previous', () => show(current - 1));
  const next = button('Next', () => show(current + 1));
  const list = button('Overview', () => {
    overview.hidden = !overview.hidden;
    list.setAttribute('aria-expanded', String(!overview.hidden));
  });
  list.setAttribute('aria-expanded', 'false');
  controls.append(previous, status, next, list);
  if (document.fullscreenEnabled && window === window.parent) {
    controls.append(button('Fullscreen', () => {
      const change = document.fullscreenElement ?
          document.exitFullscreen() : deck.requestFullscreen();
      change.catch(() => {});
    }));
  }
  pages.forEach((page, index) => {
    const heading = page.querySelector('h1, h2, h3')?.textContent?.trim();
    overview.append(button(heading || 'Slide ' + (index + 1), () => {
      show(index);
      overview.hidden = true;
      list.setAttribute('aria-expanded', 'false');
      list.focus();
    }));
  });
  deck.append(controls, overview);
  deck.addEventListener('keydown', event => {
    if (event.target instanceof HTMLElement &&
        event.target.closest('input, textarea, select, [contenteditable]')) return;
    if (event.key === 'ArrowLeft' || event.key === 'ArrowRight') {
      event.preventDefault();
      const direction = getComputedStyle(deck).direction === 'rtl' ? -1 : 1;
      show(current + (event.key === 'ArrowRight' ? direction : -direction));
    }
  });
  show(0);
}
