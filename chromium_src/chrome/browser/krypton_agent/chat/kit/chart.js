import './lib/chart.js@4.6.2/chart.umd.min.js';
import {whileVisible} from './ui.js';

const seriesToken = index => '--kr-chart-series-' + (index % 4 + 1);

export function chart(canvas, configuration, {datasetColors = false} = {}) {
  const options = configuration.options ?? {};
  const bindings = [];
  if (datasetColors) {
    for (const [index, dataset] of (configuration.data?.datasets ?? []).entries()) {
      const token = datasetColors === true ? seriesToken(index) : datasetColors[index];
      if (!token) continue;
      for (const property of ['backgroundColor', 'borderColor']) {
        if (dataset[property] === undefined) bindings.push({dataset, property, token});
      }
    }
  }

  const legendColor = options.plugins?.legend?.labels?.color === undefined;
  const tooltipDefaults = ['backgroundColor', 'borderColor', 'titleColor',
                           'bodyColor', 'footerColor', 'borderWidth']
      .filter(property => options.plugins?.tooltip?.[property] === undefined);

  function withPalette(apply) {
    const css = getComputedStyle(document.documentElement);
    const colors = new Map();
    let probe;
    function read(name) {
      if (colors.has(name)) return colors.get(name);
      let value = css.getPropertyValue(name).trim();
      if (value.startsWith('light-dark(')) {
        // Chart colour utilities need a resolved colour to parse and blend.
        if (!probe) {
          probe = document.createElement('span');
          probe.hidden = true;
          document.body.append(probe);
        }
        probe.style.color = value;
        value = getComputedStyle(probe).color;
      }
      colors.set(name, value);
      return value;
    }
    try {
      apply(read);
    } finally {
      probe?.remove();
    }
  }

  function colorDatasets(read) {
    for (const binding of bindings) {
      const {dataset, property, token, previous} = binding;
      if (dataset[property] !== previous) continue;
      binding.previous = Array.isArray(token) ? token.map(read) : read(token);
      dataset[property] = binding.previous;
    }
  }

  if (bindings.length) withPalette(colorDatasets);
  const instance = new globalThis.Chart(canvas, {
    ...configuration,
    options: {...options, responsive: true, maintainAspectRatio: false, animation: false},
  });
  function applyColors(read) {
    instance.options.color = read('--kr-text');
    for (const [id, scale] of Object.entries(instance.options.scales ?? {})) {
      const explicit = options.scales?.[id];
      if (scale.ticks && explicit?.ticks?.color === undefined) {
        scale.ticks.color = read('--kr-muted');
      }
      if (scale.title && explicit?.title?.color === undefined) {
        scale.title.color = read('--kr-text');
      }
      if (scale.grid && explicit?.grid?.color === undefined) {
        scale.grid.color = read('--kr-chart-grid');
      }
      if (scale.border && explicit?.border?.color === undefined) {
        scale.border.color = read('--kr-chart-grid');
      }
    }
    const {legend, tooltip} = instance.options.plugins || {};
    if (legendColor && legend?.labels) legend.labels.color = read('--kr-text');
    if (tooltip) {
      const colors = {
        backgroundColor: read('--kr-panel'),
        borderColor: read('--kr-control-line'),
        titleColor: read('--kr-text'),
        bodyColor: read('--kr-text'),
        footerColor: read('--kr-muted'),
        borderWidth: 1,
      };
      for (const property of tooltipDefaults) tooltip[property] = colors[property];
    }
    colorDatasets(read);
  }
  function recolor() {
    withPalette(applyColors);
    instance.update('none');
  }
  recolor();
  addEventListener('krypton-theme', recolor);
  const stopObserving = whileVisible(active => {
    if (!active) instance.stop();
  });
  return {
    instance,
    dispose() {
      removeEventListener('krypton-theme', recolor);
      stopObserving();
      instance.destroy();
    },
  };
}
