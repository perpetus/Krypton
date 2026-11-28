const root = document.documentElement;
const parentWindow = window.parent;
const reduced = matchMedia('(prefers-reduced-motion: reduce)');
const visibilityCallbacks = new Set();
const pausedAnimations = new Set();
let visible = parentWindow === window;
let heightFrame = 0;
let lastHeight = null;

function updateVisibility() {
  const active = visible && !document.hidden;
  root.toggleAttribute('data-kr-hidden', !active);
  if (!active || reduced.matches) {
    for (const animation of document.getAnimations()) {
      if (animation.playState === 'running' && !pausedAnimations.has(animation)) {
        animation.pause();
        pausedAnimations.add(animation);
      }
    }
  } else {
    for (const animation of pausedAnimations) {
      if (animation.playState === 'paused') animation.play();
    }
    pausedAnimations.clear();
  }
  for (const notify of visibilityCallbacks) notify(active && !reduced.matches);
}

export function whileVisible(callback) {
  visibilityCallbacks.add(callback);
  callback(visible && !document.hidden && !reduced.matches);
  return () => visibilityCallbacks.delete(callback);
}

function reportHeight() {
  if (parentWindow === window || heightFrame) return;
  heightFrame = requestAnimationFrame(() => {
    heightFrame = 0;
    const height = Math.ceil(document.body.getBoundingClientRect().height);
    if (height && height !== lastHeight) {
      lastHeight = height;
      parentWindow.postMessage({type: 'krypton-render-size', height}, '*');
    }
  });
}

addEventListener('message', ({source, origin, data}) => {
  if (source !== parentWindow || source === window ||
      origin !== 'chrome-untrusted://krypton-agent-chat' || !data) return;
  if (data.type === 'krypton-theme' && typeof data.dark === 'boolean') {
    root.dataset.theme = data.dark ? 'dark' : 'light';
    dispatchEvent(new Event('krypton-theme'));
  } else if (data.type === 'krypton-render-visible' &&
             typeof data.visible === 'boolean') {
    visible = data.visible;
    updateVisibility();
  }
});
document.addEventListener('visibilitychange', updateVisibility);
reduced.addEventListener('change', updateVisibility);
const resize = new ResizeObserver(reportHeight);
if (document.body) resize.observe(document.body);
updateVisibility();

export function suggestPrompt(text) {
  if (parentWindow !== window && navigator.userActivation.isActive &&
      typeof text === 'string' && text.trim()) {
    parentWindow.postMessage({type: 'krypton-suggest', text}, '*');
  }
}

export function tabs(list) {
  const buttons = [...list.querySelectorAll('[role=tab]')];
  function select(button, focus) {
    for (const other of buttons) {
      const selected = other === button;
      other.tabIndex = selected ? 0 : -1;
      other.setAttribute('aria-selected', String(selected));
      const id = other.getAttribute('aria-controls');
      const panel = id && document.getElementById(id);
      if (panel) panel.hidden = !selected;
    }
    if (focus) button.focus();
  }
  buttons.forEach((button, index) => {
    button.addEventListener('click', () => select(button, false));
    button.addEventListener('keydown', event => {
      const direction = getComputedStyle(list).direction === 'rtl' ? -1 : 1;
      const keys = {ArrowLeft: index - direction, ArrowRight: index + direction,
                    Home: 0, End: buttons.length - 1};
      if (!Object.hasOwn(keys, event.key)) return;
      event.preventDefault();
      select(buttons[(keys[event.key] + buttons.length) % buttons.length], true);
    });
  });
  const initial = buttons.find(button => button.getAttribute('aria-selected') === 'true');
  if (buttons.length) select(initial ?? buttons[0], false);
}

export function toggle(button, callback) {
  button.addEventListener('click', () => {
    const enabled = button.getAttribute('aria-pressed') !== 'true';
    button.setAttribute('aria-pressed', String(enabled));
    callback(enabled);
  });
}

export function slider(input, output, format = value => value) {
  const update = () => output.value = format(input.value);
  input.addEventListener('input', update);
  update();
}

export function segmented(group, callback) {
  group.addEventListener('change', ({target}) => {
    if (target instanceof HTMLInputElement && target.type === 'radio' &&
        target.checked) {
      callback(target.value);
    }
  });
}

export function sortableTable(table) {
  const collator = new Intl.Collator(undefined, {numeric: true, sensitivity: 'base'});
  for (const button of table.querySelectorAll('th button')) {
    button.addEventListener('click', () => {
      const column = button.closest('th');
      const body = table.tBodies[0];
      if (!column || !body) return;
      const descending = column.getAttribute('aria-sort') === 'ascending';
      for (const cell of table.querySelectorAll('[aria-sort]')) cell.removeAttribute('aria-sort');
      column.setAttribute('aria-sort', descending ? 'descending' : 'ascending');
      const rows = [...body.rows].map(row => {
        const cell = row.cells[column.cellIndex];
        const raw = cell?.dataset.sortValue?.trim();
        return {row, raw, value: raw || cell?.textContent || ''};
      });
      const numeric = rows.every(({raw}) => raw && Number.isFinite(Number(raw)));
      const compare = numeric ? (a, b) => Number(a.value) - Number(b.value) :
          (a, b) => collator.compare(a.value, b.value);
      const direction = descending ? -1 : 1;
      rows.sort((a, b) => direction * compare(a, b));
      body.append(...rows.map(({row}) => row));
    });
  }
}

// Render scripts can also use these names after loading this module.
const helpers = {segmented, slider, sortableTable, suggestPrompt, tabs, toggle, whileVisible};
for (const [name, helper] of Object.entries(helpers)) {
  if (!(name in globalThis)) globalThis[name] = helper;
}
