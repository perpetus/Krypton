// Online/AI and diagram import are intentionally absent from the local canvas.
export async function parseMermaidToExcalidraw(): Promise<never> {
  throw new Error('Diagram import is unavailable.');
}
export default parseMermaidToExcalidraw;
