import React, {useEffect, useRef, useState} from 'react';
import {Excalidraw, MainMenu, FONT_FAMILY, convertToExcalidrawElements,
  getSceneVersion, exportToBlob} from '@excalidraw/excalidraw';
import type {ExcalidrawImperativeAPI, BinaryFiles} from '@excalidraw/excalidraw/types';
import '@excalidraw/excalidraw/index.css';
import {load, openLink, request, navigate, type DocumentWriter, type LoadedDocument} from './bridge';
import {chooseImage, insertImage, pngBase64} from './images';

function safeElements(elements: any[]) {
  return elements.filter(element => !['iframe','embeddable','magicframe'].includes(element.type));
}
// Excalidraw draws its dark theme by inverting the canvas, so handing it the
// dark page colour rendered an almost white background. Its own default
// (white, inverted to dark) follows the theme, as Clear canvas does; page
// colours saved by earlier versions are treated as that default.
const DEFAULT_BACKGROUND = '#ffffff';
function background(saved: unknown) {
  const styles = getComputedStyle(document.documentElement);
  const pages = ['--light-page', '--dark-page'].map(name =>
    styles.getPropertyValue(name).trim().toLowerCase());
  return typeof saved === 'string' && saved && !pages.includes(saved.trim().toLowerCase())
    ? saved : DEFAULT_BACKGROUND;
}
function savedState(state: any) {
  return {viewBackgroundColor: state.viewBackgroundColor, scrollX: state.scrollX,
    scrollY: state.scrollY, zoom: state.zoom, currentItemRoughness: state.currentItemRoughness,
    currentItemFontFamily: state.currentItemFontFamily};
}
export default function Easel({data, writer, dark, report}: {
  data:LoadedDocument; writer:DocumentWriter; dark:boolean; report:(error:string)=>void;
}) {
  const api = useRef<ExcalidrawImperativeAPI>();
  const lastVersion = useRef('');
  const transferring = useRef(false);
  const [busy, setBusy] = useState(false);
  const [destinations, setDestinations] = useState<Array<{id:string; title:string}>>();
  const scene = useRef<any>();
  if (!scene.current) {
    const parsed = JSON.parse(data.content);
    const files: BinaryFiles = {};
    for (const [id, png] of Object.entries(data.images)) files[id] = {
      id: id as any, mimeType:'image/png', dataURL:('data:image/png;base64,' + png) as any, created:0,
    };
    scene.current = {elements:safeElements(parsed.elements || []), files,
      appState:{...parsed.appState, collaborators:new Map(),
        currentItemRoughness: parsed.appState?.currentItemRoughness ?? 0,
        currentItemFontFamily: parsed.appState?.currentItemFontFamily ?? FONT_FAMILY.Nunito,
        viewBackgroundColor:background(parsed.appState?.viewBackgroundColor)},
      capture:!!parsed.kryptonCapture};
    lastVersion.current = getSceneVersion(scene.current.elements) + ':' + scene.current.appState.viewBackgroundColor;
  }
  const addImage = async (file: Blob) => {
    const added = await insertImage(file);
    const current = api.current;
    if (!current) return;
    const state = current.getAppState();
    current.addFiles([{id:added.image, mimeType:'image/png', created:Date.now(),
      dataURL:'data:image/png;base64,' + added.png}]);
    const element = convertToExcalidrawElements([{type:'image', x:-state.scrollX, y:-state.scrollY,
      width:added.width, height:added.height, fileId:added.image, status:'saved', scale:[1,1]} as any]);
    current.updateScene({elements:[...current.getSceneElements(), ...element]});
  };
  useEffect(() => () => {api.current=undefined;}, []);
  const action = (run:()=>Promise<unknown>) => {void run().catch(error=>report(error.message));};
  const transferCapture = async (id: string) => {
    if(transferring.current) return;
    transferring.current=true; setBusy(true);
    try {
    await writer.flush();
    const image = scene.current.elements.find((element:any) => element.type==='image');
    const png = image && data.images[image.fileId];
    if (!png) throw new Error('Capture image is missing.');
    const destination = await request('load', {id, passive:'true'});
    const inserted = await request('insertImage', {id, png});
    const target = JSON.parse(destination.content);
    const element = convertToExcalidrawElements([{...image, id:crypto.randomUUID(), fileId:inserted.image}]);
    target.elements = [...safeElements(target.elements || []), ...element];
    delete target.files;
    await request('save', {id, content:JSON.stringify(target), revision:destination.revision});
    // Remove only the temporary capture document, after destination commit.
    await request('delete', {id:data.id, revision:writer.revision});
    navigate(destination.id);
    } finally {transferring.current=false; setBusy(false);}
  };
  return <><nav aria-label="Easel tools">
    <button onClick={() => action(async () => {const file=await chooseImage(); if(file) await addImage(file);})}>Insert image</button>
    <button onClick={() => action(async () => {
      if (!api.current) return;
      await writer.flush();
      const current = api.current;
      const blob = await exportToBlob({elements:[...current.getSceneElements()], appState:current.getAppState(),
        files:current.getFiles(), mimeType:'image/png', maxWidthOrHeight:8192});
      const {data:png} = await pngBase64(blob);
      await request('export', {format:'png', png});
    })}>Export PNG</button>
    {scene.current.capture && <button onClick={() => action(async () => {
      setDestinations((await request('list')).documents.filter((row:any)=>row.id!==data.id));
    })}>Add capture to another easel…</button>}
  </nav>
  {destinations && <dialog open aria-label="Choose an easel"><h2>Add capture to</h2>
    <button onClick={()=>setDestinations(undefined)}>Keep in this new easel</button>
    {destinations.map(row=><button key={row.id} onClick={()=>action(()=>transferCapture(row.id))}>{row.title}</button>)}
  </dialog>}
  <section className="easel" onDropCapture={event=>{
    if(!event.dataTransfer.files.length) return; event.preventDefault(); event.stopPropagation();
    action(()=>addImage(event.dataTransfer.files[0]));
  }} onPasteCapture={event=>{
    if(!event.clipboardData.files.length) return; event.preventDefault(); event.stopPropagation();
    action(()=>addImage(event.clipboardData.files[0]));
  }}>
    <Excalidraw excalidrawAPI={value=>{api.current=value;}} initialData={scene.current} theme={dark?'dark':'light'}
      viewModeEnabled={busy}
      langCode="en" isCollaborating={false} aiEnabled={false} autoFocus={false} handleKeyboardGlobally={false}
      validateEmbeddable={false} renderEmbeddable={()=>null}
      UIOptions={{canvasActions:{export:false, loadScene:false, saveToActiveFile:false, saveAsImage:false, toggleTheme:false}, tools:{image:false}}}
      onLinkOpen={(element,event)=>{event.preventDefault(); if(element.link) openLink(element.link,event.detail.nativeEvent as unknown as Event);}}
      onPaste={data=>!data.files && !data.mixedContent?.some(item=>item.type==='imageUrl') &&
        !data.elements?.some(element=>['image','iframe','embeddable','magicframe'].includes(element.type))}
      onChange={(elements,state)=>{
        if(transferring.current) return;
        const version=getSceneVersion(elements)+':'+state.viewBackgroundColor;
        if(version===lastVersion.current) return;
        lastVersion.current=version;
        // Serialize once after an edit burst, never on pointer/viewport frames.
        writer.edit(()=>JSON.stringify({type:'excalidraw', version:2,
          elements:safeElements([...api.current!.getSceneElements()]), appState:savedState(api.current!.getAppState()),
          ...(scene.current.capture?{kryptonCapture:true}:{})}));
      }}>
      <MainMenu><MainMenu.DefaultItems.ClearCanvas/></MainMenu>
    </Excalidraw>
  </section></>;
}
