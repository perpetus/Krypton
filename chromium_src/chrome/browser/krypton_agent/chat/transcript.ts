import {TranscriptCallbackRouter, TranscriptConnection} from './transcript.mojom-webui.js';
import type {SourceCitation, TranscriptItem} from './transcript.mojom-webui.js';

const iconUrls = new Map<string, string>();
const transcriptConnection = TranscriptConnection.getRemote();

// Segoe Fluent Icons code points.
const glyphs: Record<string, string> = {
  reasoning: '',
  command: '',
  file_change: '',
  file_search: '',
  web_search: '',
  tool: '',
  subagent: '',
  todo: '',
  plan: '',
  notice: '',
  error: '',
  approval: '',
};

function element<K extends keyof HTMLElementTagNameMap>(
    tag: K, className = '', text = ''): HTMLElementTagNameMap[K] {
  const node = document.createElement(tag);
  if (className) {
    node.className = className;
  }
  if (text) {
    node.textContent = text;
  }
  return node;
}

function glyph(kind: string): HTMLElement {
  const icon = element('span', 'glyph', glyphs[kind] ?? glyphs['tool']);
  icon.setAttribute('aria-hidden', 'true');
  return icon;
}

function sourceIcon(source: SourceCitation): HTMLElement {
  if (source.faviconPng.length) {
    let url = iconUrls.get(source.id);
    if (!url) {
      url = URL.createObjectURL(new Blob([new Uint8Array(source.faviconPng)],
                                         {type: 'image/png'}));
      iconUrls.set(source.id, url);
    }
    const image = document.createElement('img');
    image.src = url;
    image.alt = '';
    image.className = 'source-icon';
    return image;
  }
  const icon = element('span', 'source-icon source-initial',
                       source.site.charAt(0).toUpperCase());
  icon.setAttribute('aria-hidden', 'true');
  return icon;
}

function openSource(id: string, event: MouseEvent) {
  if (event.isTrusted) {
    transcriptConnection.openSource(id);
  }
}

function sourceList(sources: SourceCitation[]): HTMLDetailsElement {
  const details = element('details', 'sources');
  const summary = document.createElement('summary');
  const stack = element('span', 'source-stack');
  stack.setAttribute('aria-hidden', 'true');
  for (const source of sources.slice(0, 5)) {
    stack.append(sourceIcon(source));
  }
  summary.append(stack);
  if (sources.length > 5) {
    summary.append(`+${sources.length - 5} `);
  }
  summary.append('Sources');
  details.append(summary);

  const items = document.createElement('div');
  const buttons: HTMLButtonElement[] = [];
  let alsoRead = false;
  let citedNumber = 0;
  for (const source of sources) {
    if (!source.cited && !alsoRead) {
      items.append(element('div', 'source-group', 'Also read'));
      alsoRead = true;
    }
    const button = element('button', 'source-row');
    button.type = 'button';
    const labels = document.createElement('span');
    const title = element('bdi', 'source-title',
                          (source.cited ? `${++citedNumber}. ` : '') +
                              (source.title || source.site));
    const url = element('span', 'source-address',
                        `${source.site} · ${source.displayUrl}`);
    labels.append(title, url);
    button.append(sourceIcon(source), labels);
    button.addEventListener('click', event => openSource(source.id, event));
    buttons.push(button);
    items.append(button);
  }
  details.append(items);
  details.addEventListener('keydown', event => {
    if (event.key !== 'ArrowDown' && event.key !== 'ArrowUp') {
      return;
    }
    const current =
        buttons.findIndex(button => button === document.activeElement);
    const next = current + (event.key === 'ArrowDown' ? 1 : -1);
    if (next >= -1 && next < buttons.length) {
      event.preventDefault();
      (buttons[next] ?? summary).focus();
    }
  });
  return details;
}

// ----------------------------------------------------------------- markdown
// Model text becomes DOM nodes only; no HTML is parsed and nothing loads.

type Citations = Map<string, {source: SourceCitation, number: number}>;

function safeUrl(value: string): URL|null {
  try {
    const url = new URL(value);
    return (url.protocol === 'https:' || url.protocol === 'http:') &&
            !url.username && !url.password ?
        url :
        null;
  } catch {
    return null;
  }
}

const inlinePattern = new RegExp(
    [
      '`([^`]+)`',                                          // 1 code
      '\\[(S[0-9]{1,3})\\]',                                // 2 citation
      '\\[([^\\]]{1,512})\\]\\((https?:\\/\\/[^)\\s]+)\\)',  // 3,4 link
      '\\*\\*([^*]+)\\*\\*|__([^_]+)__',                    // 5,6 bold
      '~~([^~]+)~~',                                        // 7 strike
      '\\*([^*\\s][^*]*)\\*|\\b_([^_\\s][^_]*)_\\b',        // 8,9 italic
      '(https?:\\/\\/[^\\s<>()]+[^\\s<>().,;:!?\'"])',      // 10 bare link
    ].join('|'),
    'g');

function inline(target: Node, text: string, citations: Citations) {
  let offset = 0;
  for (const match of text.matchAll(inlinePattern)) {
    const start = match.index ?? offset;
    target.appendChild(document.createTextNode(text.slice(offset, start)));
    if (match[1] !== undefined) {
      target.appendChild(element('code', '', match[1]));
    } else if (match[2] !== undefined) {
      const item = citations.get(match[2]);
      if (item) {
        const chip = element('button', 'source-chip');
        chip.type = 'button';
        chip.setAttribute('aria-label',
                          `Source ${item.number}: ${item.source.site}`);
        chip.append(sourceIcon(item.source), String(item.number));
        chip.addEventListener('click',
                              event => openSource(item.source.id, event));
        target.appendChild(chip);
      }
    } else if (match[3] !== undefined || match[10] !== undefined) {
      const url = safeUrl(match[4] ?? match[10] ?? '');
      if (url) {
        const anchor = element('a', '', match[3] ?? url.href);
        anchor.href = url.href;
        anchor.rel = 'noreferrer';
        anchor.title = url.href;
        target.appendChild(anchor);
      } else {
        target.appendChild(document.createTextNode(match[0]));
      }
    } else if (match[5] !== undefined || match[6] !== undefined) {
      const strong = document.createElement('strong');
      inline(strong, match[5] ?? match[6] ?? '', citations);
      target.appendChild(strong);
    } else if (match[7] !== undefined) {
      const strike = document.createElement('s');
      inline(strike, match[7], citations);
      target.appendChild(strike);
    } else {
      const emphasis = document.createElement('em');
      inline(emphasis, match[8] ?? match[9] ?? '', citations);
      target.appendChild(emphasis);
    }
    offset = start + match[0].length;
  }
  target.appendChild(document.createTextNode(text.slice(offset)));
}

function codeBlock(language: string, lines: string[]): HTMLElement {
  const block = element('div', 'code-block');
  const header = element('div', 'code-header');
  header.append(element('span', 'code-language', language || 'text'));
  const copy = element('button', 'code-copy', 'Copy');
  copy.type = 'button';
  const code = element('code', '', lines.join('\n'));
  copy.addEventListener('click', () => {
    navigator.clipboard.writeText(code.textContent ?? '')
        .then(() => {
          copy.textContent = 'Copied';
          setTimeout(() => copy.textContent = 'Copy', 1500);
        })
        .catch(() => {});
  });
  header.append(copy);
  const pre = document.createElement('pre');
  pre.append(code);
  block.append(header, pre);
  return block;
}

function tableCells(line: string): string[] {
  let row = line.trim();
  if (row.startsWith('|')) {
    row = row.slice(1);
  }
  if (row.endsWith('|')) {
    row = row.slice(0, -1);
  }
  return row.split('|').map(cell => cell.trim());
}

const listItem = /^(\s*)([-*+]|\d{1,9}[.)])\s+(.*)$/;

function markdown(text: string, citations: Citations): DocumentFragment {
  const content = document.createDocumentFragment();
  const lines = text.split('\n');
  let paragraph: string[] = [];
  const flush = () => {
    if (paragraph.length) {
      const block = document.createElement('p');
      paragraph.forEach((line, index) => {
        if (index) {
          block.append(document.createElement('br'));
        }
        inline(block, line, citations);
      });
      content.append(block);
      paragraph = [];
    }
  };
  for (let index = 0; index < lines.length; ++index) {
    const line = lines[index] ?? '';
    const fence = /^\s*(```+|~~~+)\s*([\w+#.-]*)/.exec(line);
    if (fence) {
      flush();
      const marker = fence[1] ?? '```';
      const code: string[] = [];
      while (++index < lines.length &&
             !(lines[index] ?? '').trim().startsWith(marker)) {
        code.push(lines[index] ?? '');
      }
      content.append(codeBlock(fence[2] ?? '', code));
      continue;
    }
    if (!line.trim()) {
      flush();
      continue;
    }
    const heading = /^(#{1,6})\s+(.*)$/.exec(line);
    if (heading) {
      flush();
      const level = Math.min(6, (heading[1] ?? '#').length + 1);
      const block = document.createElement(`h${level}` as 'h2');
      inline(block, heading[2] ?? '', citations);
      content.append(block);
      continue;
    }
    if (/^\s*([-*_])(\s*\1){2,}\s*$/.test(line)) {
      flush();
      content.append(document.createElement('hr'));
      continue;
    }
    if (/^\s*>/.test(line)) {
      flush();
      const quoted: string[] = [];
      while (index < lines.length && /^\s*>/.test(lines[index] ?? '')) {
        quoted.push((lines[index] ?? '').replace(/^\s*>\s?/, ''));
        ++index;
      }
      --index;
      const quote = document.createElement('blockquote');
      quote.append(markdown(quoted.join('\n'), citations));
      content.append(quote);
      continue;
    }
    const next = lines[index + 1] ?? '';
    if (line.includes('|') && /^\s*\|?\s*:?-{2,}:?\s*(\|\s*:?-{2,}:?\s*)*\|?\s*$/.test(next)) {
      flush();
      const table = document.createElement('table');
      const head = document.createElement('thead');
      const headRow = document.createElement('tr');
      for (const cell of tableCells(line)) {
        const th = document.createElement('th');
        inline(th, cell, citations);
        headRow.append(th);
      }
      head.append(headRow);
      table.append(head);
      const body = document.createElement('tbody');
      index += 2;
      while (index < lines.length && (lines[index] ?? '').includes('|') &&
             (lines[index] ?? '').trim()) {
        const row = document.createElement('tr');
        for (const cell of tableCells(lines[index] ?? '')) {
          const td = document.createElement('td');
          inline(td, cell, citations);
          row.append(td);
        }
        body.append(row);
        ++index;
      }
      --index;
      table.append(body);
      const wrapper = element('div', 'table');
      wrapper.append(table);
      content.append(wrapper);
      continue;
    }
    if (listItem.test(line)) {
      flush();
      const block: string[] = [];
      while (index < lines.length) {
        const current = lines[index] ?? '';
        if (!current.trim()) {
          const following = lines[index + 1] ?? '';
          if (!listItem.test(following) && !/^\s{2,}\S/.test(following)) {
            break;
          }
        } else if (!listItem.test(current) && !/^\s{2,}\S/.test(current)) {
          break;
        }
        block.push(current);
        ++index;
      }
      --index;
      content.append(list(block, citations));
      continue;
    }
    paragraph.push(line);
  }
  flush();
  return content;
}

function list(lines: string[], citations: Citations): HTMLElement {
  const first = listItem.exec(lines[0] ?? '');
  const indent = (first?.[1] ?? '').length;
  const ordered = /\d/.test(first?.[2] ?? '');
  const root = document.createElement(ordered ? 'ol' : 'ul');
  if (ordered) {
    root.setAttribute('start', String(parseInt(first?.[2] ?? '1', 10) || 1));
  }
  let item: {node: HTMLLIElement, text: string[], children: string[]}|null =
      null;
  const close = () => {
    if (!item) {
      return;
    }
    const task = /^\[([ xX~])\]\s+(.*)$/.exec(item.text[0] ?? '');
    if (task) {
      item.node.classList.add('task');
      item.node.dataset['state'] = task[1] === ' ' ? 'open' :
          task[1] === '~'                          ? 'running' :
                                                     'done';
      item.text[0] = task[2] ?? '';
    }
    inline(item.node, item.text.join(' '), citations);
    if (item.children.length) {
      item.node.append(list(item.children, citations));
    }
    root.append(item.node);
    item = null;
  };
  for (const line of lines) {
    const match = listItem.exec(line);
    if (match && (match[1] ?? '').length <= indent) {
      close();
      item = {
        node: document.createElement('li'),
        text: [match[3] ?? ''],
        children: [],
      };
    } else if (item && match) {
      item.children.push(line);
    } else if (item && line.trim()) {
      if (item.children.length) {
        item.children.push(line);
      } else {
        item.text.push(line.trim());
      }
    }
  }
  close();
  return root;
}

// -------------------------------------------------------------------- items

interface Message {
  article: HTMLElement;
  body: HTMLElement;
  text: string;
  sources: SourceCitation[];
  items: TranscriptItem[];
  finished: boolean;
}

// Expanded items stay expanded across streaming re-renders.
const expanded = new Set<string>();
const startedAt = new Map<string, number>();
const endedAt = new Map<string, number>();

function verb(item: TranscriptItem): string {
  const running = item.status === 'running' || item.status === 'waiting';
  const declined = item.status === 'declined';
  switch (item.kind) {
    case 'command':
      return declined ? 'Declined' : running ? 'Running' : 'Ran';
    case 'file_change':
      return running ? 'Editing' : 'Edited';
    case 'file_search':
      return running ? 'Searching' : 'Searched';
    case 'web_search':
      return running ? 'Searching the web' : 'Searched the web';
    case 'subagent':
      return running ? 'Running subagent' : 'Subagent';
    case 'todo':
      return 'Plan';
    case 'plan':
      return 'Proposed plan';
    case 'approval':
      return item.status === 'approved' ? 'Allowed' :
          declined                      ? 'Declined' :
                                          'Asked to';
    case 'tool':
      return running ? 'Using' : 'Used';
    default:
      return item.title || '';
  }
}

function statusLabel(status: string): string {
  switch (status) {
    case 'failed':
      return 'Failed';
    case 'declined':
      return 'Declined';
    case 'interrupted':
      return 'Stopped';
    case 'waiting':
      return 'Waiting';
    default:
      return '';
  }
}

function reasoningRow(item: TranscriptItem): HTMLElement {
  const running = item.status === 'running';
  const details = element('details', 'item reasoning');
  details.open = expanded.has(item.id);
  details.addEventListener('toggle', () => {
    if (details.open) {
      expanded.add(item.id);
    } else {
      expanded.delete(item.id);
    }
  });
  const summary = element('summary', 'item-line');
  summary.append(glyph('reasoning'));
  // Durations are known only for thinking this page watched.
  if (running && !startedAt.has(item.id)) {
    startedAt.set(item.id, Date.now());
  }
  const started = startedAt.get(item.id);
  if (!running && started !== undefined && !endedAt.has(item.id)) {
    endedAt.set(item.id, Date.now());
  }
  const ended = endedAt.get(item.id);
  const label = running ? 'Thinking' :
      item.title        ? item.title :
      started !== undefined && ended !== undefined ?
                          `Thought for ${
                              Math.max(1, Math.round((ended - started) / 1000))}s` :
                          'Thoughts';
  summary.append(
      element('span', running ? 'item-label shimmer' : 'item-label', label));
  if (!details.open && item.text) {
    const lines = item.text.trim().split('\n');
    summary.append(element('span', 'item-detail preview',
                           (lines[lines.length - 1] ?? '').slice(0, 240)));
  }
  details.append(summary);
  const body = element('div', 'item-body reasoning-text');
  body.append(markdown(item.text, new Map()));
  details.append(body);
  return details;
}

function itemRow(item: TranscriptItem): HTMLElement {
  if (item.kind === 'reasoning') {
    return reasoningRow(item);
  }
  const running = item.status === 'running' || item.status === 'waiting';
  const expandable = Boolean(item.output) || item.kind === 'todo' ||
      item.kind === 'plan' || (item.kind === 'subagent' && Boolean(item.text));
  const container = element(expandable ? 'details' : 'div',
                            `item ${item.kind} status-${item.status}`);
  if (container instanceof HTMLDetailsElement) {
    container.open = expanded.has(item.id) ||
        ((item.kind === 'todo' || item.kind === 'plan') &&
         !expanded.has(`${item.id}:closed`));
    container.addEventListener('toggle', () => {
      if (container.open) {
        expanded.add(item.id);
        expanded.delete(`${item.id}:closed`);
      } else {
        expanded.delete(item.id);
        expanded.add(`${item.id}:closed`);
      }
    });
  }
  const line = element(expandable ? 'summary' : 'div', 'item-line');
  line.append(glyph(item.kind));
  const label = verb(item);
  if (item.kind === 'notice' || item.kind === 'error') {
    line.append(element('span', 'item-label', item.text || item.title));
  } else {
    line.append(element('span', running ? 'item-label shimmer' : 'item-label',
                        label));
    const name = item.kind === 'tool' ? item.title :
        item.kind === 'approval'      ? item.title.toLowerCase() :
                                        '';
    if (name) {
      line.append(element('span', 'item-name', name));
    }
    const detail = item.detail.split('\n')[0] ?? '';
    if (detail) {
      line.append(element(
          item.kind === 'command' || item.kind === 'file_change' ||
                  item.kind === 'file_search' || item.kind === 'approval' ?
              'code' :
              'span',
          'item-detail', detail));
    }
    if (item.text && item.kind !== 'todo' && item.kind !== 'plan' &&
        item.kind !== 'subagent') {
      line.append(element('span', 'item-meta', item.text));
    }
  }
  const status = statusLabel(item.status);
  if (running) {
    line.append(element('span', 'spinner'));
  } else if (status) {
    line.append(element('span', `item-status ${item.status}`, status));
  }
  container.append(line);
  if (item.kind === 'todo') {
    const body = element('div', 'item-body');
    body.append(markdown(
        item.text.split('\n').map(step => `- ${step}`).join('\n'), new Map()));
    container.append(body);
  } else if (item.kind === 'plan') {
    const body = element('div', 'item-body plan-text');
    body.append(markdown(item.text, new Map()));
    container.append(body);
  } else if (expandable) {
    const body = element('div', 'item-body');
    if (item.kind === 'subagent' && item.text) {
      body.append(element('div', 'item-progress', item.text));
    }
    if (item.output) {
      body.append(element('pre', 'item-output', item.output));
    }
    container.append(body);
  }
  return container;
}

function renderMessage(id: string, message: Message) {
  const citations: Citations = new Map();
  let number = 0;
  for (const source of message.sources) {
    if (source.cited) {
      citations.set(source.id, {source, number: ++number});
    }
  }
  if (id.startsWith('user-')) {
    message.body.replaceChildren(message.text);
    return;
  }
  if (id.startsWith('error-')) {
    message.body.replaceChildren(glyph('error'), element('span', '', message.text));
    return;
  }
  const content = document.createDocumentFragment();
  const items = [...message.items].sort((a, b) => a.offset - b.offset);
  let position = 0;
  let group: HTMLElement|null = null;
  for (const item of items) {
    const offset = Math.min(Math.max(item.offset, position), message.text.length);
    if (offset > position) {
      content.append(markdown(message.text.slice(position, offset), citations));
      position = offset;
      group = null;
    }
    if (!group) {
      group = element('div', 'work');
      content.append(group);
    }
    group.append(itemRow(item));
  }
  if (position < message.text.length) {
    content.append(markdown(message.text.slice(position), citations));
  }
  message.body.replaceChildren(content);
}

// ------------------------------------------------------------------ wiring

const transcript = document.querySelector('#transcript');
const working = document.querySelector<HTMLElement>('#working');
if (transcript && working) {
  const messages = new Map<string, Message>();
  const pending = new Set<string>();
  const router = new TranscriptCallbackRouter();
  const scroller = document.scrollingElement ?? document.documentElement;
  const atBottom = () =>
      scroller.scrollHeight - scroller.scrollTop - scroller.clientHeight < 48;

  let renderScheduled = false;
  let follow = true;
  const renders = new Map<string, {frame: HTMLIFrameElement, visible: boolean}>();
  const renderIds = new WeakMap<HTMLIFrameElement, string>();
  const renderWindows = new WeakMap<MessageEventSource, string>();
  const heights = new Map<HTMLIFrameElement, number>();
  let heightScheduled = false;
  let darkTheme = false;
  const sendTheme = (frame: HTMLIFrameElement) => {
    frame.contentWindow?.postMessage({type: 'krypton-theme', dark: darkTheme}, '*');
  };
  const sendVisibility = ({frame, visible}: {frame: HTMLIFrameElement, visible: boolean}) => {
    frame.contentWindow?.postMessage({
      type: 'krypton-render-visible', visible: visible && !document.hidden,
    }, '*');
  };
  const intersection = new IntersectionObserver(entries => {
    for (const entry of entries) {
      if (!(entry.target instanceof HTMLIFrameElement)) {
        continue;
      }
      const id = renderIds.get(entry.target);
      const render = id === undefined ? undefined : renders.get(id);
      if (render && render.visible !== entry.isIntersecting) {
        render.visible = entry.isIntersecting;
        sendVisibility(render);
      }
    }
  });
  document.addEventListener('visibilitychange', () => {
    for (const render of renders.values()) {
      sendVisibility(render);
    }
  });
  window.addEventListener('message', ({source, origin, data}) => {
    if (origin !== 'null' || !source || !data || typeof data !== 'object') {
      return;
    }
    const id = renderWindows.get(source);
    const render = id === undefined ? undefined : renders.get(id);
    if (!render || id === undefined || source !== render.frame.contentWindow) {
      return;
    }
    if (data.type === 'krypton-suggest' &&
        typeof data.text === 'string' && data.text.length > 0 &&
        data.text.length <= 120000 && render.visible &&
        navigator.userActivation.isActive) {
      transcriptConnection.suggestPrompt(id, data.text);
    } else if (data.type === 'krypton-render-size' &&
               typeof data.height === 'number' && Number.isFinite(data.height) &&
               !document.documentElement.classList.contains('preview')) {
      heights.set(render.frame, Math.max(80, Math.min(2000, Math.ceil(data.height))));
      if (!heightScheduled) {
        heightScheduled = true;
        requestAnimationFrame(() => {
          heightScheduled = false;
          const stick = follow || atBottom();
          for (const [frame, height] of heights) {
            frame.height = String(height);
          }
          heights.clear();
          if (stick) {
            scroller.scrollTop = scroller.scrollHeight;
          }
        });
      }
    }
  });
  window.addEventListener('scroll', () => {
    follow = atBottom();
  }, {passive: true});
  const schedule = (id: string) => {
    pending.add(id);
    if (renderScheduled) {
      return;
    }
    renderScheduled = true;
    requestAnimationFrame(() => {
      renderScheduled = false;
      const stick = follow || atBottom();
      for (const key of pending) {
        const message = messages.get(key);
        if (message) {
          renderMessage(key, message);
        }
      }
      pending.clear();
      if (stick) {
        scroller.scrollTop = scroller.scrollHeight;
        follow = true;
      }
    });
  };

  // The working indicator counts the turn's seconds while it runs.
  let workingSince = 0;
  let ticker = 0;
  const clock = element('span', 'working-time');
  const shimmerLabel = element('span', 'shimmer', 'Working');
  working.replaceChildren(element('span', 'spinner'), shimmerLabel, clock);
  const tick = () => {
    const seconds = Math.floor((Date.now() - workingSince) / 1000);
    clock.textContent =
        `${Math.floor(seconds / 60)}:${String(seconds % 60).padStart(2, '0')}`;
  };
  const setWorking = (on: boolean) => {
    if (on === !working.hidden) {
      return;
    }
    working.hidden = !on;
    clearInterval(ticker);
    if (on) {
      workingSince = Date.now();
      tick();
      ticker = setInterval(tick, 1000);
      if (follow) {
        scroller.scrollTop = scroller.scrollHeight;
      }
    }
  };

  router.append.addListener((id: string, text: string) => {
    if (messages.has(id)) {
      return;
    }
    const role = id.startsWith('user-') ? 'user' :
        id.startsWith('error-')        ? 'error' :
                                         'assistant';
    const article = element('article', role);
    const body = element('div', 'body');
    article.append(body);
    article.setAttribute('aria-busy', 'true');
    transcript.append(article);
    messages.set(id, {article, body, text, sources: [], items: [], finished: false});
    schedule(id);
  });

  router.update.addListener((id: string, text: string) => {
    const message = messages.get(id);
    if (message) {
      message.text = text;
      schedule(id);
    }
  });

  router.finish.addListener((id: string) => {
    const message = messages.get(id);
    if (message) {
      message.finished = true;
      message.article.removeAttribute('aria-busy');
    }
  });

  router.setSources.addListener((id: string, sources: SourceCitation[]) => {
    const message = messages.get(id);
    if (message && !message.sources.length && sources.length) {
      message.sources = sources;
      message.article.append(sourceList(sources));
      schedule(id);
    }
  });

  router.setItem.addListener((id: string, item: TranscriptItem) => {
    const message = messages.get(id);
    if (!message) {
      return;
    }
    const index = message.items.findIndex(existing => existing.id === item.id);
    if (index >= 0) {
      message.items[index] = item;
    } else {
      message.items.push(item);
    }
    schedule(id);
  });

  router.setWorking.addListener(setWorking);

  router.showHtml.addListener((id, renderId, title, width, height) => {
    if (!/^[0-9a-f]{32}$/i.test(renderId) || renders.has(renderId)) {
      return;
    }
    const frame = document.createElement('iframe');
    frame.setAttribute('sandbox', 'allow-scripts');
    frame.referrerPolicy = 'no-referrer';
    frame.title = title;
    frame.width = String(width);
    frame.height = String(height);
    frame.setAttribute('allow', "fullscreen 'none'");
    const render = {frame, visible: !id};
    renders.set(renderId, render);
    renderIds.set(frame, renderId);
    frame.addEventListener('load', () => {
      if (frame.contentWindow) {
        renderWindows.set(frame.contentWindow, renderId);
      }
      sendTheme(frame);
      sendVisibility(render);
    });
    frame.src = 'chrome-untrusted://krypton-agent-render/' + renderId;
    if (!id) {
      document.documentElement.classList.add('preview');
      document.body.replaceChildren(frame);
      return;
    }
    const message = messages.get(id);
    if (message) {
      frame.className = 'html-render';
      const card = element('section', 'render-card');
      const header = element('div', 'render-header');
      const open = element('button', 'render-open', 'Open');
      open.type = 'button';
      open.setAttribute('aria-label', 'Open render in a tab: ' + title);
      open.addEventListener('click', () => {
        if (navigator.userActivation.isActive) {
          transcriptConnection.openRender(renderId);
        }
      });
      header.append(element('span', 'render-title', title), open);
      card.append(header, frame);
      message.article.insertBefore(card, message.body);
      intersection.observe(frame);
    } else {
      renders.delete(renderId);
    }
  });

  router.setTheme.addListener((dark: boolean) => {
    darkTheme = dark;
    document.documentElement.dataset['theme'] = dark ? 'dark' : 'light';
    for (const {frame} of renders.values()) {
      sendTheme(frame);
    }
  });

  transcriptConnection.connect(router.$.bindNewPipeAndPassRemote());
}
