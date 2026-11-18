// Build explicitly outside GN. No network is needed after npm ci.
import {build} from 'vite';
import react from '@vitejs/plugin-react';
import fs from 'node:fs/promises';
import path from 'node:path';
import crypto from 'node:crypto';
import {fileURLToPath} from 'node:url';
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const repository = path.dirname(root);
const vendor = path.join(repository, 'chromium_src/third_party/krypton_documents');
const output = path.join(vendor, 'prebuilt');
const tokenPath = path.resolve(repository, '../KryptonUI/design/tokens.json');
const hash = bytes => crypto.createHash('sha256').update(bytes).digest('hex');
const tokenBytes = await fs.readFile(tokenPath);
const tokens = JSON.parse(tokenBytes);
const color = value => value.length === 9 ? `#${value.slice(3)}${value.slice(1,3)}` : value;
const css = [':root {', `--document-font: "${tokens.settings.fontFamily}", sans-serif;`,
  `--document-font-size: ${tokens.settings.bodySize}px;`,
  `--document-gap: ${tokens.spacing.fieldPadding}px;`,
  `--document-padding: ${tokens.spacing.windowMargin * 2}px;`,
  `--document-radius: ${tokens.radius.button}px;`,
  `--document-card-radius: ${tokens.radius.card}px;`,
  `--document-accent: ${color(tokens.theme.defaultAccent)};`];
for (const scheme of ['light', 'dark']) for (const [name, key] of Object.entries({
  page:'pageBackground',surface:'overlay',text:'text',muted:'textMuted',line:'separator',hover:'buttonHover',
})) css.push(`--${scheme}-${name}: ${color(tokens[scheme][key])};`);
css.push('}');
await fs.writeFile(path.join(root, 'src/tokens.generated.css'), css.join('\n')+'\n');
const lockBytes = await fs.readFile(path.join(root,'package-lock.json'));
const lock = JSON.parse(lockBytes);
if (Object.keys(lock.packages).some(name=>name.includes('@tiptap-pro/')))
  throw new Error('Proprietary editor package is forbidden.');
const manifest = JSON.parse(await fs.readFile(path.join(root,'package.json')));
for (const version of Object.values({...manifest.dependencies,...manifest.devDependencies}))
  if (!/^\d+\.\d+\.\d+$/.test(version)) throw new Error('Direct package versions must be exact stable releases.');
const tiptapVersions = new Set(Object.entries(manifest.dependencies).filter(([name])=>name.startsWith('@tiptap/')).map(([,version])=>version));
if (tiptapVersions.size!==1) throw new Error('All Tiptap packages must use one version.');
const usedPackages = new Map();
const disabledOrigins = new Set();
const namespace = 'http://www.w3.org/';
function offline(source) {
  // Remove static external service/help/CDN targets in the library's distributed
  // code. Do not change runtime user links. CSP is the independent enforcement
  // layer. Refuse unrecognized remaining origins in the final artifact below.
  return source.replaceAll('https:\\/\\/', 'offline:\\/\\/')
    .replaceAll('http:\\/\\/', 'offline:\\/\\/')
    .replace(/https?:\/\/[^\\\s"'`<>()${}]+/g, url => {
    if (url.startsWith(namespace)) return url; // SVG/XML identifiers, not requests.
    disabledOrigins.add(url); return 'about:blank#offline';
  });
}
await build({root, base:'/', resolve:{alias:{
  '@excalidraw/mermaid-to-excalidraw':path.join(root,'src/disabled-diagrams.ts'),
}}, plugins:[react(), {
  name:'krypton-offline-editors',
  async transform(_code,id) {
    if (!id.includes('node_modules')) return;
    let directory=path.dirname(id.split('?')[0]);
    while (directory.includes('node_modules')) {
      try {const p=JSON.parse(await fs.readFile(path.join(directory,'package.json'),'utf8'));
        if(p.name&&p.version) {usedPackages.set(directory,p); break;}
      } catch { /* Try the package root. */ }
      directory=path.dirname(directory);
    }
  },
  renderChunk(code) {return {code:offline(code),map:null};},
}], build:{outDir:'.bundle',emptyOutDir:true,sourcemap:false,assetsInlineLimit:0,
  minify:true, target:'chrome157', cssMinify:true}});

// Only remove this script's own generated vendor subtree, never a build graph.
if (output!==path.join(repository,'chromium_src','third_party','krypton_documents','prebuilt'))
  throw new Error('Unexpected output path.');
await fs.rm(output,{recursive:true,force:true});
await fs.mkdir(output,{recursive:true});
await fs.cp(path.join(root,'.bundle'),output,{recursive:true});
await fs.cp(path.join(root,'trusted'),path.join(output,'trusted'),{recursive:true});
// Notes and Easel tabs use the same Krypton favicon as other internal pages.
await fs.copyFile(path.join(repository,'resources/core/favicons/favicon_ntp_32.png'),
  path.join(output,'trusted/favicon.png'));
await fs.cp(path.join(root,'node_modules/@excalidraw/excalidraw/dist/prod/fonts'),path.join(output,'fonts'),{recursive:true});

async function files(directory) {
  const result=[];
  for(const entry of await fs.readdir(directory,{withFileTypes:true})) {
    const p=path.join(directory,entry.name);
    if(entry.isDirectory()) result.push(...await files(p)); else if(entry.isFile()) result.push(p);
    else throw new Error('Symlink/special file in vendor output.');
  }
  return result.sort();
}
// Refuse network-bearing runtime artifacts, including CSS and HTML. W3C XML
// namespaces are exempt; source maps and service workers are never shipped.
for(const file of await files(output)) if(/\.(?:js|css|html)$/.test(file)) {
  let source=await fs.readFile(file,'utf8');
  source=offline(source);
  await fs.writeFile(file,source);
  const stripped=source.replaceAll(/https?:\/\/www\.w3\.org\/[\w/.-]*/g,'');
  if(/(?:https?:\/\/|["'`(]\/\/)[a-z\d][a-z\d.-]*\.[a-z]{2}/i.test(stripped) || /https?:\\\/\\\//.test(stripped))
    throw new Error(`Remote origin remains in ${file}`);
  if(file.endsWith('.map')) throw new Error('Source maps may not be vendored.');
}
const licenses = path.join(vendor,'licenses');
await fs.mkdir(licenses,{recursive:true});
await fs.cp(path.join(root,'font-licenses'),path.join(licenses,'fonts'),{recursive:true});
for(const family of await fs.readdir(path.join(output,'fonts'))) {
  await fs.access(path.join(licenses,'fonts',family,'LICENSE'));
}
const packageEvidence=[];
for (const [directory,pkg] of [...usedPackages.entries()].sort((a,b)=>a[1].name.localeCompare(b[1].name))) {
  const entries=await fs.readdir(directory);
  let names=entries.filter(name=>/^(?:licen[cs]e|copying|notice)(?:[._-].*)?$/i.test(name));
  let licenseDirectory=directory;
  if(!names.length) {
    licenseDirectory=path.join(root,'package-licenses',pkg.name.replaceAll('/','__').replaceAll('@',''));
    names=(await fs.readdir(licenseDirectory)).filter(name=>/^LICENSE/i.test(name));
  }
  if(!names.length) throw new Error(`Missing license file: ${pkg.name}`);
  const destination=path.join(licenses,pkg.name.replaceAll('/','__').replaceAll('@','')+'-'+pkg.version);
  await fs.mkdir(destination,{recursive:true});
  const licenseParts=[];
  for(const name of names) if((await fs.stat(path.join(licenseDirectory,name))).isFile()) {
    const bytes=await fs.readFile(path.join(licenseDirectory,name));
    licenseParts.push(`--- ${name} ---\n${bytes.toString('utf8')}`);
  }
  if(!licenseParts.length) throw new Error(`No readable license: ${pkg.name}`);
  const license=licenseParts.join('\n\n');
  await fs.writeFile(path.join(destination,'LICENSE'),license);
  const entry=lock.packages[path.relative(root,directory).replaceAll(path.sep,'/')];
  if(!entry?.integrity) throw new Error(`Missing integrity: ${pkg.name}`);
  await fs.writeFile(path.join(destination,'README.chromium'),
    `Name: ${pkg.name}\nShort Name: ${pkg.name}\nURL: ${typeof pkg.repository==='string'?pkg.repository:pkg.repository?.url||pkg.homepage||entry.resolved}\nVersion: ${pkg.version}\nLicense: ${pkg.license}\nLicense File: LICENSE\nSecurity Critical: yes\nShipped: yes\n\nVendored by web_documents/tools/vendor.mjs. Static remote URL literals disabled; application integrations remain offline.\n`);
  packageEvidence.push({name:pkg.name,version:pkg.version,license:pkg.license,
    integrity:entry.integrity,license_sha256:hash(license)});
}
const allLicenses=[];
for(const file of await files(licenses)) if(path.basename(file)==='LICENSE')
  allLicenses.push(`===== ${path.relative(licenses,file)} =====\n${await fs.readFile(file,'utf8')}`);
await fs.writeFile(path.join(vendor,'LICENSE'),allLicenses.join('\n\n'));
const runtimeFiles=await files(output);
const resource = file=>'IDR_KRYPTON_DOCUMENTS_'+path.relative(output,file).replaceAll(path.sep,'/').replace(/[^a-zA-Z0-9]/g,'_').toUpperCase();
const ids=runtimeFiles.map(resource);
if(new Set(ids).size!==ids.length) throw new Error('Duplicate resource identifier.');
const escape=value=>value.replaceAll('&','&amp;').replaceAll('"','&quot;');
const includes=runtimeFiles.map(file=>{
  const relative=path.relative(output,file).replaceAll(path.sep,'/');
  return `      <include name="${resource(file)}" file="prebuilt/${escape(relative)}" type="BINDATA" resource_path="${escape(relative)}"/>`;
});
await fs.writeFile(path.join(vendor,'krypton_documents_resources.grd'),
  `<?xml version="1.0" encoding="UTF-8"?>\n<grit latest_public_release="0" current_release="1">\n  <outputs>\n    <output filename="grit/krypton_documents_resources.h" type="rc_header"><emit emit_type="prepend"></emit></output>\n    <output filename="grit/krypton_documents_resources_map.cc" type="resource_file_map_source"/>\n    <output filename="grit/krypton_documents_resources_map.h" type="resource_map_header"/>\n    <output filename="krypton_documents_resources.pak" type="data_package"/>\n  </outputs>\n  <release seq="1">\n    <includes>\n${includes.join('\n')}\n    </includes>\n  </release>\n</grit>\n`);
const hashes={};
for(const file of runtimeFiles) hashes[path.relative(vendor,file).replaceAll(path.sep,'/')]=hash(await fs.readFile(file));
await fs.writeFile(path.join(vendor,'sha256-manifest.json'),JSON.stringify({
  format:1,token_sha256:hash(tokenBytes),package_lock_sha256:hash(lockBytes),
  packages:packageEvidence,disabled_static_urls:[...disabledOrigins].sort(),files:hashes,
},null,2)+'\n');
console.log(JSON.stringify({files:runtimeFiles.length,packages:packageEvidence.length,
  bytes:(await Promise.all(runtimeFiles.map(file=>fs.stat(file)))).reduce((sum,value)=>sum+value.size,0),
  manifest_sha256:hash(await fs.readFile(path.join(vendor,'sha256-manifest.json')))},null,2));
