import { request } from './bridge';
export const assetUrls = new Map<string, string>();
export const localImage = /^images\/[0-9a-f-]{36}\.png$/;
export function installImages(images: Record<string, string>) {
  for (const [id, base64] of Object.entries(images)) {
    const bytes = Uint8Array.from(atob(base64), c => c.charCodeAt(0));
    const url = URL.createObjectURL(new Blob([bytes], {type: 'image/png'}));
    const key = `images/${id}.png`;
    const old = assetUrls.get(key);
    if (old) URL.revokeObjectURL(old);
    assetUrls.set(key, url);
  }
}
export function clearImages() {
  for (const url of assetUrls.values()) URL.revokeObjectURL(url);
  assetUrls.clear();
}
export async function pngBase64(file: Blob) {
  if (!['image/png', 'image/jpeg', 'image/webp', 'image/gif'].includes(file.type) || file.size > 8 * 1024 * 1024)
    throw new Error('Choose a PNG, JPEG, WebP or GIF up to 8 MiB.');
  const bitmap = await createImageBitmap(file);
  try {
    if (bitmap.width > 16384 || bitmap.height > 16384 || bitmap.width * bitmap.height > 32 * 1024 * 1024)
      throw new Error('Image exceeds the 32 million pixel limit.');
    const canvas = document.createElement('canvas');
    canvas.width = bitmap.width; canvas.height = bitmap.height;
    canvas.getContext('2d')!.drawImage(bitmap, 0, 0);
    const blob = await new Promise<Blob>((resolve, reject) => canvas.toBlob(
      value => value ? resolve(value) : reject(new Error('Could not convert the image.')), 'image/png'));
    if (blob.size > 8 * 1024 * 1024) throw new Error('PNG exceeds the 8 MiB limit.');
    const data = await new Promise<string>((resolve, reject) => {
      const reader = new FileReader(); reader.onload = () => resolve(String(reader.result).split(',')[1]);
      reader.onerror = () => reject(new Error('Could not read the image.')); reader.readAsDataURL(blob);
    });
    return {data, width: bitmap.width, height: bitmap.height};
  } finally { bitmap.close(); }
}
export async function insertImage(file: Blob) {
  const {data, width, height} = await pngBase64(file);
  const result = await request('insertImage', {png: data});
  installImages({[result.image]: result.png});
  return {...result, width, height};
}
export async function chooseImage(): Promise<File | undefined> {
  return new Promise(resolve => {
    const input = document.createElement('input');
    input.type = 'file'; input.accept = 'image/png,image/jpeg,image/webp,image/gif';
    input.addEventListener('change', () => resolve(input.files?.[0]), {once: true});
    input.addEventListener('cancel', () => resolve(undefined), {once: true});
    input.click();
  });
}
