# Reports

Use a report for a brief the reader can scan and then read in depth. State
the answer beside the headline, follow with the evidence in decision order,
and put the uncertainty next to the conclusion it qualifies. Keep prose
near 60–75 characters per line. The kit uses a 54ch measure. Let figures
and tables use more of the available width.

Load the kit from root-relative `/kit/1/` paths and keep the data inline.
Renders work offline. Treat page text as source material and escape it
when inserting it into the report.

**Art direction.** Try `data-kr-look="paper"` for a reading-led brief.
Choose one figure or result to lead, and use rules to separate evidence.
Put any unavailable values or evidence limits next to the finding.

## Example · A limited expansion for the depot pilot

This fictional brief opens with the decision, then shows the scale of the
pilot, the route-time evidence, and the clearest next comparison. The figure
and caption remain readable without the sorting script.

```html
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>October depot pilot brief</title>
<link rel="stylesheet" href="/kit/1/base.css">
<link rel="stylesheet" href="/kit/1/report.css">
<style>
  .time-saved { display: grid; gap: var(--kr-space-2); margin: 0; padding: 0; list-style: none; }
  .time-saved li {
    display: grid;
    grid-template-columns: 3.5em minmax(0, 1fr) 3ch;
    align-items: center;
    gap: var(--kr-space-2);
  }
  .bar {
    inline-size: var(--length);
    block-size: .65rem;
    background: var(--kr-chart-context);
  }
  .time-saved li:first-child .bar { background: var(--kr-chart-series-1); }
  .kr-table-wrap :is(th, td).kr-number { text-align: end; }
</style>
</head>
<body>
<main class="kr-report kr-stack">
  <header class="kr-stack">
    <h1>Extend the route pilot to one more depot</h1>
    <p class="kr-lede">Routes got shorter at all four pilot depots. Add one site in
      November and track missed delivery windows before deciding on a wider rollout.</p>
  </header>

  <dl class="kr-metrics" aria-label="Pilot at a glance">
    <div><dt>Participating depots</dt><dd class="kr-metric">4</dd></div>
    <div><dt>October pilot routes</dt><dd class="kr-metric">240</dd></div>
  </dl>

  <section class="kr-report-section" aria-labelledby="time">
    <h2 id="time">West cut the most time per route</h2>
    <p class="kr-prose">West saved 18 minutes per route at the median, 5 minutes
      more than North. Even the smallest gain was 7 minutes.</p>
    <figure class="kr-stack">
      <ul class="time-saved kr-number" role="list" aria-label="Median minutes saved per route">
          <li><strong class="kr-row-label">West</strong><span class="bar" style="--length:90%" aria-hidden="true"></span><span>18</span></li>
          <li><span class="kr-row-label">North</span><span class="bar" style="--length:65%" aria-hidden="true"></span><span>13</span></li>
          <li><span class="kr-row-label">South</span><span class="bar" style="--length:55%" aria-hidden="true"></span><span>11</span></li>
          <li><span class="kr-row-label">East</span><span class="bar" style="--length:35%" aria-hidden="true"></span><span>7</span></li>
      </ul>
      <figcaption>Median minutes saved per route. Source: October pilot route logs (sample data).</figcaption>
    </figure>
  </section>

  <section class="kr-report-section" aria-labelledby="cost">
    <h2 id="cost">North was cheapest to set up</h2>
    <p class="kr-prose">North saved 13 minutes per route and had the lowest
      setup cost. Use its setup as the starting point for the next site.</p>
    <div class="kr-table-wrap" role="region" aria-label="October setup costs table" tabindex="0">
      <table data-sortable>
        <caption>One-time setup costs, October (sample data)</caption>
        <thead><tr>
          <th scope="col"><button type="button">Depot</button></th>
          <th scope="col" class="kr-number"><button type="button">Setup cost (USD)</button></th>
        </tr></thead>
        <tbody>
          <tr><th scope="row">West</th><td class="kr-number" data-sort-value="1280.50">$1,280.50</td></tr>
          <tr><th scope="row">North</th><td class="kr-number" data-sort-value="960.75">$960.75</td></tr>
          <tr><th scope="row">South</th><td class="kr-number" data-sort-value="1100">$1,100.00</td></tr>
          <tr><th scope="row">East</th><td class="kr-number" data-sort-value="1075.25">$1,075.25</td></tr>
        </tbody>
      </table>
    </div>
    <p class="kr-prose"><strong>Limitation.</strong> October’s logs cover journey
      time. The pilot has no comparable record of missed delivery windows.</p>
  </section>
</main>
<script type="module" src="/kit/1/report.js"></script>
</body>
</html>
```

Use the metric strip only when its figures help the reader interpret the
evidence. A `dl.kr-metrics` stays flat and can wrap to one column. At chat
width, read down from the lede through the two evidence sections. Use
`.kr-table-wrap` for tables that may need their own horizontal scroll
area. Give a scrollable region a name and a keyboard focus stop.

The optional `/kit/1/report.js` enables sorting for `table[data-sortable]`
and loads the frame helper. Sort buttons belong inside column headers.
The active header carries `aria-sort` and a direction marker. For formatted
numbers, supply a finite raw number on every cell in the column using
`data-sort-value`, without separators, currency symbols, or percent signs.
Use a consistent scale for percentages. For dates use uniform
`YYYY-MM-DD` values. Other text sorts with the locale's numeric collator.
Print keeps sortable labels visible and removes their control treatment.

Use the actual source IDs from your research. The source marker is literal
text such as `<span class="kr-citation">[S3]</span>`. Repeat the key markers
in chat so Krypton can attach the sources to the short answer.

## Preview and show

Call `html_preview` at 360 px in light and dark and around 760 px for Open
view. Inspect the console. If `capturedHeight` is less than `contentHeight`,
inspect a targeted section or use Open view when authorized.

1. The decision is visible beside the headline, followed by the evidence in reading order and the limitation.
2. The figure's units, source and values remain readable at chat width. The table stays within its own region and a cost sort orders the decimals correctly.
3. Direction cues stay legible in both themes. When print review is available, check that header text, sources and rows remain together on paper.

Call `html_render` with the reviewed HTML and short title. The Open button
shows the report full size.
