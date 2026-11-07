// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#ifndef CHROME_RENDERER_KRYPTON_BOOST_PICKER_H_
#define CHROME_RENDERER_KRYPTON_BOOST_PICKER_H_
// Runs only after an explicit user request, in Chromium's private internal
// isolated world. It never focuses a DOM element or changes page content.
inline constexpr char kKryptonBoostPicker[] = R"JS(
(() => {
  globalThis.__kryptonBoostPicker?.();
  return new Promise(resolve => {
    const abort = new AbortController();
    const overlay = document.createElement('div');
    Object.assign(overlay.style, {
      position: 'fixed', pointerEvents: 'none', zIndex: '2147483647',
      outline: '2px solid Highlight', background: 'transparent',
      borderRadius: '2px', display: 'none'
    });
    document.documentElement.append(overlay);
    let finished = false;
    const finish = selector => {
      if (finished) return;
      finished = true;
      abort.abort();
      overlay.remove();
      delete globalThis.__kryptonBoostPicker;
      resolve(selector);
    };
    globalThis.__kryptonBoostPicker = () => finish('');
    const options = {capture: true, signal: abort.signal};
    const selectorFor = element => {
      const parts = [];
      for (let node = element; node instanceof Element; node = node.parentElement) {
        if (node.id && !/[{}]/.test(node.id)) {
          const id = '#' + CSS.escape(node.id);
          if (document.querySelectorAll(id).length === 1) {
            parts.unshift(id);
            return parts.join(' > ');
          }
        }
        const tag = CSS.escape(node.localName);
        let index = 1;
        for (let sibling = node.previousElementSibling; sibling; sibling = sibling.previousElementSibling)
          if (sibling.localName === node.localName) ++index;
        parts.unshift(tag + ':nth-of-type(' + index + ')');
      }
      return parts.join(' > ');
    };
    window.addEventListener('pointermove', event => {
      const target = document.elementFromPoint(event.clientX, event.clientY);
      if (!(target instanceof Element) || target === overlay) return;
      const rect = target.getBoundingClientRect();
      Object.assign(overlay.style, {display:'block', left:rect.left+'px', top:rect.top+'px', width:rect.width+'px', height:rect.height+'px'});
    }, options);
    const suppress = event => { event.preventDefault(); event.stopImmediatePropagation(); };
    for (const name of ['pointerdown', 'pointerup', 'mousedown', 'mouseup', 'dblclick'])
      window.addEventListener(name, suppress, options);
    window.addEventListener('click', event => {
      suppress(event);
      const element = document.elementFromPoint(event.clientX, event.clientY);
      finish(element instanceof Element && element !== overlay ? selectorFor(element) : '');
    }, options);
    window.addEventListener('contextmenu', event => { suppress(event); finish(''); }, options);
    window.addEventListener('keydown', event => { suppress(event); if (event.key === 'Escape') finish(''); }, options);
    window.addEventListener('pagehide', () => finish(''), options);
  });
})()
)JS";
#endif
