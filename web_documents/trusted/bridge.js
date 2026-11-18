// Only this first-party frame has WebUI bindings. Never import an editor here.
(() => {
  'use strict';
  const origin = 'chrome-untrusted://krypton-documents';
  const frame = document.getElementById('editor');
  const kind = location.hostname === 'notes' ? 'notes' : 'easel';
  const uuid = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/;
  let id = location.pathname.slice(1) || 'new';
  const validId = value => value === 'new' || (typeof value === 'string' && uuid.test(value));
  if (!validId(id)) { document.body.textContent = 'Invalid document id.'; return; }
  const operations = new Set(['load', 'save', 'list', 'rename', 'delete', 'insertImage', 'export']);
  const pending = new Map();
  window.kryptonDocumentThemeChanged = dark => {
    frame.contentWindow.postMessage({channel:'krypton-documents', dark:!!dark},origin);
  };
  window.kryptonDocumentReply = (token, result) => {
    const entry = pending.get(token);
    if (!entry) return;
    pending.delete(token);
    if (entry.operation === 'load' && entry.updateRoute && uuid.test(result.id || '')) {
      id = result.id;
      history.replaceState(null, '', '/' + id);
    }
    if (typeof result.title === 'string') document.title = result.title;
    frame.contentWindow.postMessage({channel: 'krypton-documents', token, result}, origin);
  };
  window.addEventListener('message', event => {
    if (event.source !== frame.contentWindow || event.origin !== origin) return;
    const message = event.data;
    if (!message || message.channel !== 'krypton-documents') return;
    if (message.navigate && validId(message.navigate)) {
      location.href = '/' + message.navigate;
      return;
    }
    const {token, request} = message;
    if (typeof token !== 'string' || token.length > 64 || !request ||
        !operations.has(request.operation) || !validId(request.id) || pending.size >= 2) return;
    const keys = Object.keys(request);
    if (keys.length > 6 || keys.some(key => key.length > 32 || typeof request[key] !== 'string') ||
        keys.reduce((sum, key) => sum + request[key].length, 0) > 13 * 1024 * 1024) return;
    pending.set(token, {operation:request.operation, updateRoute:request.passive!=='true'});
    chrome.send('documentRequest', [token, request]);
  });
  frame.src = origin + '/index.html#' + kind + '/' + id;
})();
