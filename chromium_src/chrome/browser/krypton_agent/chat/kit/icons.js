import lucide from './lib/lucide-static@1.81.0/nodes.mjs';

const svgNamespace = 'http://www.w3.org/2000/svg';

function svgNode(tag, attrs) {
  const element = document.createElementNS(svgNamespace, tag);
  for (const [name, value] of Object.entries(attrs)) {
    element.setAttribute(name, value);
  }
  return element;
}

function symbol(size, label, attrs) {
  const dimension = Number.isFinite(size) && size > 0 ? size : 24;
  const svg = svgNode('svg', {
    class: 'kr-icon', viewBox: '0 0 24 24', focusable: 'false',
    width: dimension, height: dimension, ...attrs,
  });
  if (dimension === size) svg.style.setProperty('--kr-icon-size', size + 'px');
  if (typeof label === 'string' && label.trim()) {
    svg.setAttribute('role', 'img');
    svg.setAttribute('aria-label', label);
  } else {
    svg.setAttribute('aria-hidden', 'true');
  }
  return svg;
}

export function icon(name, {size, label} = {}) {
  if (typeof name !== 'string' || !Object.hasOwn(lucide, name)) return null;
  const svg = symbol(size, label, {
    fill: 'none', stroke: 'currentColor', 'stroke-width': 2,
    'stroke-linecap': 'round', 'stroke-linejoin': 'round',
  });
  for (const [tag, attrs] of lucide[name]) svg.append(svgNode(tag, attrs));
  return svg;
}

export async function brand(slug, {size, label, color} = {}) {
  if (typeof slug !== 'string' || !/^[a-z0-9][a-z0-9_-]*$/.test(slug)) return null;
  const first = slug[0];
  const group = first <= '9' ? '0-9' : first;
  const {default: logos} = await import('./lib/simple-icons@16.38.1/' + group + '.mjs');
  if (!Object.hasOwn(logos, slug)) return null;
  const [hex, d] = logos[slug];
  const svg = symbol(size, label, {fill: color === 'brand' ? '#' + hex : 'currentColor'});
  svg.append(svgNode('path', {d}));
  return svg;
}

// Original hero artwork for a partly cloudy observation.
export function weatherArt() {
  const svg = svgNode('svg', {
    class: 'kr-icon kr-icon--art', viewBox: '0 0 144 112',
    'aria-hidden': 'true', focusable: 'false', fill: 'none',
    'stroke-linecap': 'round', 'stroke-linejoin': 'round',
  });
  const shapes = [
    ['path', {d: 'M4 81C25 46 77 17 133 15M9 95c22-28 70-52 129-53M26 104c31-20 75-30 111-29',
      stroke: 'currentColor', opacity: .32}],
    ['circle', {cx: 92, cy: 41, r: 28, fill: 'var(--kr-illustration-accent, #e6ac4c)'}],
    ['path', {d: 'M92 13v56m-23-43h46M64 41h56m-48 15h40',
      stroke: 'var(--kr-hero-surface, Canvas)', opacity: .47}],
    ['path', {d: 'M28 81c0-8 7-14 15-14 4-12 15-19 27-17 9 2 15 8 18 16 12-2 22 5 22 15 0 8-6 13-15 13H43c-9 0-15-5-15-13Z',
      fill: 'var(--kr-hero-surface, Canvas)', stroke: 'currentColor', 'stroke-width': 1.5}],
    ['path', {d: 'M12 105h30m8 0h20m40-13h23', stroke: 'currentColor', opacity: .55}],
  ];
  for (const [tag, attrs] of shapes) svg.append(svgNode(tag, attrs));
  return svg;
}
