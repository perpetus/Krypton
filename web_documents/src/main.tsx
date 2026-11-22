import React, {Suspense, lazy, useEffect, useRef, useState} from 'react';
import {createRoot} from 'react-dom/client';
import {DocumentWriter, documentKind, load, navigate, request, type LoadedDocument} from './bridge';
import {clearImages, installImages} from './images';
import './tokens.generated.css';
import './style.css';
const Notes = lazy(() => import('./notes'));
const Easel = lazy(() => import('./easel'));
function App() {
  const [data, setData] = useState<LoadedDocument>();
  const [error, setError] = useState('');
  const [status, setStatus] = useState('Loading…');
  const [title, setTitle] = useState('');
  const [documents, setDocuments] = useState<Array<{id:string; title:string}>>();
  const [dark, setDark] = useState(matchMedia('(prefers-color-scheme: dark)').matches);
  const writer = useRef<DocumentWriter>();
  useEffect(() => {
    let active = true;
    load().then(value => {
      if (!active) return;
      installImages(value.images); setData(value); setTitle(value.title);
      setDark(value.dark);
      writer.current = new DocumentWriter(value.revision, setStatus); setStatus('Saved locally');
    }).catch(error => active && setError(error.message));
    const theme = (event:Event) => setDark((event as CustomEvent<boolean>).detail);
    window.addEventListener('krypton-document-theme',theme);
    const unload = (event: BeforeUnloadEvent) => {
      if (writer.current?.dirty) { void writer.current.flush().catch(() => {}); event.preventDefault(); event.returnValue = ''; }
    };
    const hide = () => { void writer.current?.flush().catch(() => {}); };
    window.addEventListener('beforeunload', unload);
    window.addEventListener('pagehide', hide);
    return () => {
      active = false; writer.current?.stop(); clearImages();
      window.removeEventListener('krypton-document-theme',theme);
      window.removeEventListener('beforeunload', unload); window.removeEventListener('pagehide', hide);
    };
  }, []);
  const action = (run: () => Promise<unknown>) => { setError(''); void run().catch(error => setError(error.message)); };
  const rename = () => {
    if (title && title !== data?.title) action(async () => {
      await request('rename', {title}); setData(previous => previous && {...previous, title});
    });
  };
  return <main data-theme={dark ? 'dark' : 'light'}>
    <header><button onClick={() => action(async () => {
      await writer.current?.flush(); setDocuments((await request('list')).documents);
    })}>{documentKind === 'notes' ? 'Notes' : 'Easels'}</button>
      <input aria-label="Document title" value={title} maxLength={256} onChange={e => setTitle(e.target.value)}
        onBlur={rename} onKeyDown={e => {if (e.key === 'Enter') {rename(); e.currentTarget.blur();}}}/>
      <span role="status">{status}</span>
      <button onClick={() => action(async () => {await writer.current?.flush(); await request('export', {format: 'document'});})}>Export</button>
      <button onClick={() => action(async () => {
        if (!confirm('Delete this document? This cannot be undone.')) return;
        await writer.current?.flush(); await request('delete', {revision: writer.current!.revision});
      })}>Delete</button>
    </header>
    {error && <div className="notice" role="alert">{error}<button onClick={() => setError('')}>Dismiss</button></div>}
    {documents && <dialog open aria-label="Your documents"><h2>{documentKind === 'notes' ? 'Notes' : 'Easels'}</h2>
      <button onClick={() => navigate('new')}>Create new</button>
      {documents.map(row => <button key={row.id} onClick={() => navigate(row.id)}>{row.title}</button>)}
      <button onClick={() => setDocuments(undefined)}>Close</button></dialog>}
    {data && writer.current && <Suspense fallback={<p className="loading">Loading editor…</p>}>
      {documentKind === 'notes' ? <Notes data={data} writer={writer.current} report={setError}/> :
        <Easel data={data} writer={writer.current} dark={dark} report={setError}/>}</Suspense>}
  </main>;
}
createRoot(document.getElementById('root')!).render(<App/>);
