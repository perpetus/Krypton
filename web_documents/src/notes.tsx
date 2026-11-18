import React, {useEffect, useRef} from 'react';
import {Editor, createBlockMarkdownSpec} from '@tiptap/core';
import StarterKit from '@tiptap/starter-kit';
import {Markdown} from '@tiptap/markdown';
import DragHandle from '@tiptap/extension-drag-handle';
import Placeholder from '@tiptap/extension-placeholder';
import TaskList from '@tiptap/extension-task-list';
import TaskItem from '@tiptap/extension-task-item';
import {Table, TableRow, TableHeader, TableCell} from '@tiptap/extension-table';
import Image from '@tiptap/extension-image';
import Link from '@tiptap/extension-link';
import {Details, DetailsSummary, DetailsContent} from '@tiptap/extension-details';
import Emoji, {emojis} from '@tiptap/extension-emoji';
import Mathematics from '@tiptap/extension-mathematics';
import 'katex/dist/katex.min.css';
import {openLink, type LoadedDocument, type DocumentWriter} from './bridge';
import {assetUrls, chooseImage, insertImage, localImage} from './images';

// The model stores only relative local assets. Blob URLs belong to node views,
// never to the Markdown, so reloading/exporting does not persist session URLs.
const LocalImage = Image.extend({
  parseHTML: () => [{tag: 'img[src]', getAttrs: element =>
    localImage.test((element as HTMLElement).getAttribute('src') || '') ? null : false}],
  parseMarkdown: (token, helpers) => helpers.createNode('image', {
    src: localImage.test(token.href || '') ? token.href : '', alt: token.text || '', title: token.title || '',
  }),
  renderMarkdown: node => {
    const src = node.attrs?.src || '';
    const alt = String(node.attrs?.alt || '').replace(/[\\\[\]]/g, '\\$&').replace(/\r?\n/g, ' ');
    return localImage.test(src) ? `![${alt}](${src})` : '';
  },
  addNodeView() {return ({node}) => {
    const dom = document.createElement('img');
    const update = (value: typeof node) => {
      const url = assetUrls.get(value.attrs.src);
      if (url) dom.src = url; else dom.removeAttribute('src');
      dom.alt = value.attrs.alt || ''; return true;
    };
    update(node); return {dom, update};
  };},
});
function sanitizePaste(html: string) {
  const template = document.createElement('template'); template.innerHTML = html;
  template.content.querySelectorAll('script,style,iframe,object,embed,link,meta,svg,math').forEach(node => node.remove());
  template.content.querySelectorAll('*').forEach(element => {
    if (element.tagName === 'IMG' && !localImage.test(element.getAttribute('src') || '')) {element.remove(); return;}
    for (const attribute of [...element.attributes]) {
      if (!['src', 'href', 'alt', 'title', 'colspan', 'rowspan', 'data-type', 'data-checked'].includes(attribute.name))
        element.removeAttribute(attribute.name);
    }
    const href = element.getAttribute('href');
    if (href && !/^https?:/i.test(href)) element.removeAttribute('href');
  });
  return template.innerHTML;
}
export default function Notes({data, writer, report}: {data:LoadedDocument; writer:DocumentWriter; report:(error:string)=>void}) {
  const container = useRef<HTMLDivElement>(null);
  const editor = useRef<Editor>();
  const addImage = async (file: Blob) => {
    const value = await insertImage(file);
    if (editor.current && !editor.current.isDestroyed)
      editor.current.chain().focus().setImage({src:`images/${value.image}.png`}).run();
  };
  useEffect(() => {
    const instance = new Editor({
      element: container.current!, content: data.content, contentType: 'markdown',
      extensions: [StarterKit.configure({link:false}), Markdown,
        Link.configure({openOnClick:false, isAllowedUri: url => /^https?:/i.test(url)}),
        Placeholder.configure({placeholder:'Start writing…'}), TaskList, TaskItem.configure({nested:true}),
        Table.configure({resizable:true}), TableRow, TableHeader, TableCell, LocalImage,
        Details.extend(createBlockMarkdownSpec({nodeName:'details', content:'block', allowedAttributes:['open']})).configure({persist:true}),
        DetailsSummary, DetailsContent,
        Emoji.extend({renderMarkdown: node =>
          emojis.find(item=>item.name===node.attrs?.name)?.emoji || `:${node.attrs?.name || ''}:`,
        }).configure({emojis:emojis.filter(item=>item.emoji).map(item=>({...item,fallbackImage:undefined}))}),
        Mathematics.configure({katexOptions:{throwOnError:false, trust:false}}),
        DragHandle.configure({render() {const handle=document.createElement('button'); handle.className='drag-handle';
          handle.textContent='⠿'; handle.setAttribute('aria-label','Drag block'); return handle;}})],
      editorProps: {
        transformPastedHTML: sanitizePaste,
        handlePaste: (_view, event) => {
          const files = [...(event.clipboardData?.files || [])];
          if (!files.length) return false;
          event.preventDefault(); void addImage(files[0]).catch(error => report(error.message)); return true;
        },
        handleDrop: (_view, event) => {
          const files = [...(event.dataTransfer?.files || [])];
          if (!files.length) return false;
          event.preventDefault(); void addImage(files[0]).catch(error => report(error.message)); return true;
        },
        handleClick: (_view, _pos, event) => {
          const anchor = (event.target as HTMLElement)?.closest('a[href]');
          if (!anchor) return false; event.preventDefault(); openLink(anchor.getAttribute('href')!, event); return true;
        },
      },
      onUpdate: ({editor}) => writer.edit(() => editor.getMarkdown()),
    });
    editor.current = instance;
    return () => {editor.current=undefined; instance.destroy();};
  }, [data.id, writer]);
  const run = (callback: (value: Editor) => void) => {if (editor.current) callback(editor.current);};
  return <><nav aria-label="Note formatting">
    <button onClick={() => run(e => e.chain().focus().toggleBold().run())}><b>B</b></button>
    <button onClick={() => run(e => e.chain().focus().toggleItalic().run())}><i>I</i></button>
    <button onClick={() => run(e => e.chain().focus().toggleHeading({level:2}).run())}>Heading</button>
    <button onClick={() => run(e => e.chain().focus().toggleBulletList().run())}>List</button>
    <button onClick={() => run(e => e.chain().focus().toggleTaskList().run())}>Tasks</button>
    <button onClick={() => run(e => e.chain().focus().insertTable({rows:3, cols:3, withHeaderRow:true}).run())}>Table</button>
    <button onClick={() => run(e => e.chain().focus().setDetails().run())}>Details</button>
    <button onClick={() => run(e => {const latex=prompt('Math (LaTeX)'); if(latex) e.chain().focus().insertInlineMath({latex}).run();})}>Math</button>
    <button onClick={() => run(e => {const url=prompt('Link URL'); if(url && /^https?:/i.test(url)) e.chain().focus().setLink({href:url}).run();})}>Link</button>
    <button onClick={() => {void chooseImage().then(file => file && addImage(file)).catch(error => report(error.message));}}>Image</button>
  </nav><section className="note" ref={container} aria-label="Note editor"/></>;
}
