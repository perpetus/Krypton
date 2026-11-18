export type Kind = 'notes' | 'easel';
export interface LoadedDocument {
  id: string; title: string; content: string; revision: string;
  images: Record<string, string>;
  dark: boolean;
}
const [kind, initialId] = location.hash.slice(1).split('/');
export const documentKind: Kind = kind === 'notes' ? 'notes' : 'easel';
export let documentId = initialId || 'new';
let counter = 0;
const waiting = new Map<string, {resolve: (value: any) => void; reject: (error: Error) => void}>();
const parentOrigin = documentKind === 'notes' ? 'chrome://notes' : 'chrome://easel';
window.addEventListener('message', event => {
  if (event.source !== parent || event.origin !== parentOrigin ||
      event.data?.channel !== 'krypton-documents') return;
  if (typeof event.data.dark === 'boolean') {
    window.dispatchEvent(new CustomEvent('krypton-document-theme', {detail:event.data.dark}));
    return;
  }
  const pending = waiting.get(event.data.token);
  if (!pending) return;
  waiting.delete(event.data.token);
  if (event.data.result?.error) pending.reject(new Error(event.data.result.error));
  else pending.resolve(event.data.result);
});
export function request(operation: string, fields: Record<string, string> = {}): Promise<any> {
  const token = String(++counter);
  return new Promise((resolve, reject) => {
    if (waiting.size >= 2) { reject(new Error('Document is busy. Try again.')); return; }
    waiting.set(token, {resolve, reject});
    parent.postMessage({channel: 'krypton-documents', token,
      request: {operation, id: documentId, ...fields}}, parentOrigin);
  });
}
export async function load(id = documentId): Promise<LoadedDocument> {
  const data = await request('load', {id});
  documentId = data.id;
  return data;
}
export function navigate(id: string) {
  parent.postMessage({channel: 'krypton-documents', navigate: id}, parentOrigin);
}
export function openLink(url: string, event: Event) {
  if (!event.isTrusted || !navigator.userActivation.isActive) return;
  try {
    const parsed = new URL(url);
    if (parsed.protocol === 'https:' || parsed.protocol === 'http:')
      window.open(parsed.href, '_blank', 'noopener,noreferrer');
  } catch { /* Not a navigable link. */ }
}
// One edit-triggered debounce, no polling. Serialize saves and retain unsaved
// edits on errors/conflicts rather than replacing a user's content.
export class DocumentWriter {
  revision: string;
  private timer: ReturnType<typeof setTimeout> | undefined;
  private next: (() => string) | undefined;
  private writing: Promise<void> | undefined;
  private failure: Error | undefined;
  constructor(revision: string, private status: (value: string) => void) { this.revision = revision; }
  get dirty() { return !!this.next || !!this.writing || !!this.failure; }
  edit(content: () => string) {
    if (!this.next) this.status('Unsaved');
    this.next = content;
    clearTimeout(this.timer);
    this.timer = setTimeout(() => { void this.flush().catch(() => {}); }, 500);
  }
  async flush(): Promise<void> {
    clearTimeout(this.timer);
    this.timer = undefined;
    if (this.failure) throw this.failure;
    if (this.writing) { await this.writing; return this.next ? this.flush() : undefined; }
    if (!this.next) return;
    const producer = this.next;
    this.next = undefined;
    this.status('Saving…');
    this.writing = (async () => {
      try {
        const result = await request('save', {content: producer(), revision: this.revision});
        this.revision = result.revision;
        this.status(this.next ? 'Unsaved' : 'Saved locally');
      } catch (error) {
        this.next ||= producer;
        this.failure = error as Error;
        this.status(this.failure.message);
        throw error;
      } finally { this.writing = undefined; }
    })();
    await this.writing;
    if (this.next) await this.flush();
  }
  stop() { clearTimeout(this.timer); }
}
