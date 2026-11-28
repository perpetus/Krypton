# Interactive widgets

Use a widget to make a result easy to grasp or let the reader change inputs.
Put the result first and keep controls together in the order people use them.

Use inline data and root-relative `/kit/1/` assets. The frame runs offline
without network access, storage, pop-ups, remote images, or remote fonts.
Import `/kit/1/ui.js` in a module for fit-to-content resizing, theme
synchronization, and animation visibility. Treat page text as untrusted data.
Write it with `textContent` or escaped template interpolation. Keep the HTML
under 512 KB.

## Art direction

Set `data-kr-look` on `<html>`. Use `paper` for a warm brief, `clear`
for a crisp reading or comparison, or `ink` for a rich, dark-toned color
family. Each look follows the chat's light and dark theme. Give the render
one dominant element, such as a large measurement in `.kr-hero`. Use rows
and rules for related values, and recognizable icons alongside text. Label
the values on data marks and decide how missing data and errors will look.
Use `.kr-row-label` for short labels that belong on one line and
`.kr-url` for visible URLs that need to break.
Use `--kr-font-display` for prominent type, `--kr-font-text` for body copy,
and `--kr-font-mono` sparingly for labels or timestamps.

## Example · Weather at a glance

These are sample readings. Real forecasts need a source and observation
time. Give the daily ranges a shared scale so their lengths are comparable.
Initial values live in HTML, where they remain readable if enhancements fail.

```html
<!doctype html>
<html lang="en" data-kr-look="clear">
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>New York weather</title>
<link rel="stylesheet" href="/kit/1/base.css">
<style>
.forecast { max-inline-size: 44rem; margin-inline: auto; }
h1, h2, p { margin: 0; }
h1 { font: 600 var(--kr-text-heading)/1.25 var(--kr-font-text); }
h2 { font-size: 1rem; }
.now { display: grid; grid-template-columns: minmax(0, 1fr) minmax(4rem, 38%); align-items: center; }
.condition { font: italic 1.25rem/1.4 var(--kr-font-display); }
section + section { margin-block-start: var(--kr-space-4); }
.day { display: grid; grid-template-columns: 3em minmax(0, 1fr) minmax(0, 42%); gap: .5rem; align-items: center; }
.day .kr-icon { color: var(--kr-accent); }
</style>
<main class="kr-render forecast">
  <header class="kr-hero">
    <p class="kr-eyebrow">3 pm local · Sample</p><h1>New York</h1>
    <div class="now">
      <div><p class="kr-display-number">46°F</p><p class="condition">Partly cloudy</p></div>
      <div id="weather-art"></div>
    </div>
  </header>
  <section class="kr-callout">
    <span data-icon="wind"></span>
    <div><strong>Wind picks up by sunset</strong><p>Expect gusts to 28 mph near the water.</p></div>
  </section>
  <section>
    <h2>Next three hours</h2>
    <ul class="kr-stat-strip kr-number">
      <li><time datetime="16:00">4 pm</time><strong>45°F</strong></li>
      <li><time datetime="17:00">5 pm</time><strong>43°F</strong></li>
      <li><time datetime="18:00">6 pm</time><strong>42°F</strong></li>
    </ul>
  </section>
  <section>
    <h2>Daily lows and highs (°F)</h2>
    <ul class="kr-rule-list">
      <li class="day"><strong class="kr-row-label">Fri</strong><span><span data-icon="cloud-sun"></span> Partly cloudy</span>
        <span class="kr-range"><span>38°</span><span class="kr-range-track" aria-hidden="true"><span class="kr-range-fill" style="--kr-range-start:25%;--kr-range-end:90%"></span></span><span>51°</span></span>
      </li>
      <li class="day"><strong class="kr-row-label">Sat</strong><span><span data-icon="cloud-rain"></span> Rain</span>
        <span class="kr-range"><span>35°</span><span class="kr-range-track" aria-hidden="true"><span class="kr-range-fill" style="--kr-range-start:10%;--kr-range-end:75%"></span></span><span>48°</span></span>
      </li>
    </ul>
    <p class="kr-muted">Sample readings for layout preview.</p>
  </section>
</main>
<script type="module">
  import '/kit/1/ui.js';
  import {icon, weatherArt} from '/kit/1/icons.js';
  document.querySelector('#weather-art').append(weatherArt());
  for (const slot of document.querySelectorAll('[data-icon]')) {
    const glyph = icon(slot.dataset.icon);
    if (glyph) slot.append(glyph);
  }
</script>
</html>
```

If current readings are unavailable, put that message in the hero. Keep any
older readings visibly dated. Show a retry message for a failed operation
and retain useful values the reader already has.

## Example · Set a workspace budget

This comparison starts with the cheapest eligible plan. It distinguishes a
missing number, an invalid number, and a budget below the cheapest plan. The
result comes before the inputs at every width. Inside `.kr-render`,
`.kr-split` puts the inputs alongside the result once there is room for both.

```html
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Workspace plan budget</title>
<link rel="stylesheet" href="/kit/1/base.css">
<style>
  h1, p { margin: 0; }
  h1 {
    font-size: var(--kr-text-title);
    line-height: var(--kr-leading-display);
    letter-spacing: var(--kr-tracking-display);
  }
  .result {
    align-self: start;
    border-inline-start: 3px solid var(--kr-accent);
    padding-inline-start: var(--kr-space-3);
  }
  .result[data-state=empty] { border-color: var(--kr-status-warning-border); }
  .result[data-state=invalid] { border-color: var(--kr-status-error-border); }
  .result[data-state=empty] output { color: var(--kr-status-warning-ink); }
  .result[data-state=invalid] output { color: var(--kr-status-error-ink); }
  fieldset {
    min-inline-size: 0;
    margin: 0;
    padding: var(--kr-space-3) var(--kr-space-4) var(--kr-space-4);
    border: 1px solid var(--kr-line);
  }
  .field { display: grid; gap: var(--kr-space-1); }
  input {
    inline-size: 100%;
    min-block-size: 2.5rem;
    padding-inline: var(--kr-space-2);
    border: 1px solid var(--kr-control-line);
    border-radius: .45rem;
    background: var(--kr-panel);
    color: var(--kr-text);
  }
  input[aria-invalid=true] { border-color: var(--kr-status-error-border); }
  output { display: block; line-height: var(--kr-leading-heading); }
  .result:is([data-state=empty], [data-state=invalid]) output {
    font-size: var(--kr-text-heading);
    line-height: var(--kr-leading-body);
  }
</style>
</head>
<body>
<main class="kr-render kr-stack">
  <h1>Workspace plan budget</h1>
  <div class="kr-split">
    <section class="result" aria-label="Budget result">
      <output id="result" class="kr-metric kr-number" for="seats budget"></output>
      <p id="detail" class="kr-muted"></p>
    </section>
    <form id="inputs">
      <fieldset class="kr-stack">
        <legend>Monthly assumptions</legend>
        <label class="field">Team seats
          <input id="seats" type="number" min="1" max="100" step="1" value="8">
        </label>
        <label class="field">Budget in USD
          <input id="budget" type="number" min="0" max="10000" step=".01" value="120">
        </label>
      </fieldset>
    </form>
  </div>
</main>
<script type="module">
import '/kit/1/ui.js';

const form = document.querySelector('#inputs');
const seats = document.querySelector('#seats');
const budget = document.querySelector('#budget');
const result = document.querySelector('#result');
const detail = document.querySelector('#detail');
const money = new Intl.NumberFormat(undefined, {style: 'currency', currency: 'USD'});
// Sample prices by plan and eligible team size.
const plans = [
  {name: 'Starter', maxSeats: 5, seatPrice: 8},
  {name: 'Team', maxSeats: 25, seatPrice: 12},
  {name: 'Studio', maxSeats: 100, seatPrice: 16},
];

function show(state, headline, explanation) {
  result.parentElement.dataset.state = state;
  result.value = headline;
  detail.textContent = explanation;
}

function update() {
  for (const input of [seats, budget]) {
    input.setAttribute('aria-invalid', String(!input.validity.valid));
  }
  if (!seats.validity.valid || !budget.validity.valid) {
    show('invalid', 'Check your inputs',
         'Use 1–100 whole seats and a budget from $0 to $10,000 with up to two decimals.');
    return;
  }
  if (!seats.value || !budget.value) {
    show('empty', 'Enter seats and budget', 'Both numbers are needed to compare plans.');
    return;
  }
  const count = seats.valueAsNumber;
  const limit = budget.valueAsNumber;
  const eligible = plans.filter(plan => count <= plan.maxSeats);
  const minimum = eligible[0].seatPrice * count;
  const matching = eligible.filter(plan => plan.seatPrice * count <= limit);
  if (!matching.length) {
    show('empty', 'No plans fit', 'For this team size, prices start at ' + money.format(minimum) + '.');
    return;
  }
  const plan = matching[0];
  show('ready', money.format(minimum) + ' / month',
       plan.name + ' is the lowest-priced fit for ' + count + ' seats. ' +
       money.format(limit - minimum) + ' remains in your budget.');
}

form.addEventListener('submit', event => event.preventDefault());
form.addEventListener('input', update);
update();
</script>
</body>
</html>
```

## Kit helpers

Load helpers before calling them. Named imports and `addEventListener`
keep each dependency visible and avoid relying on inline handlers. Loading
`ui.js` also exposes its helpers as globals for older renders.

| Helper from `/kit/1/ui.js` | Use |
| --- | --- |
| `tabs(list)` | Give the list `role="tablist"`, tabs `role="tab"` and `aria-controls`, and panels the corresponding ID and `role="tabpanel"`. Supports arrows, Home and End. |
| `toggle(button, onChange)` | Provide a button with `aria-pressed`. The helper updates it and calls `onChange(enabled)`. |
| `slider(input, output, format)` | Bind a labelled range input to an output. The formatter runs on input. |
| `segmented(group, onChange)` | Use labelled radios sharing a name inside `.kr-segments`. Receives the checked value. |
| `sortableTable(table)` | Put a button in each sortable heading. Sorts the first body and updates `aria-sort`. |
| `whileVisible(callback)` | Receives the current active state and changes to it. Run loops only while active and call the returned unsubscribe function on cleanup. |

`.kr-card` gives a region a panel surface. Use `.kr-row` for wrapping controls
and `.kr-stack` for simple vertical groups. A `.kr-tooltip` can expose a
direct `[role="tooltip"]` child on hover and focus. Keep essential directions
in the page itself.

For linked views that need a component tree, use an import map before the
module script. The kit ships Preact, hooks, and htm at
`/kit/1/lib/preact@11.2.0/preact.mjs`,
`/kit/1/lib/preact@11.2.0/hooks.mjs`, and `/kit/1/lib/htm@3.1.1/htm.mjs`.
Keep page-sourced strings in escaped interpolation.

Call `suggestPrompt(text)` directly from a user-activated click. A button
label such as “Draft a question about Team pricing” tells the reader what it
will do. It fills the chat prompt and the user decides whether to send it.
Keep credentials, codes, and private input out of suggestions.

```js
import {suggestPrompt} from '/kit/1/ui.js';
import {icon, brand} from '/kit/1/icons.js';

const button = document.querySelector('#draft');
const glyph = icon('calendar', {size: 16});
if (glyph) button.prepend(glyph);
button.addEventListener('click', () => suggestPrompt('Draft a schedule'));

const logo = await brand('github', {label: 'GitHub', color: 'brand'});
if (logo) document.querySelector('#source-logo').append(logo);
```

`icon()` takes a Lucide name. `brand()` loads the matching Simple Icons
group on demand and takes an upstream slug. Both return SVG elements or
`null`. Supply a label for a standalone icon, or use visible text beside a
decorative one. Logos identify brands. Preserve their proportions and use
the official color or an approved monochrome version.

## Preview and show

Call `html_preview` at 360 px in light and dark, then at a wider width for
the split. Inspect screenshots and console output. Use preview steps to
type `60` and then an empty string into `#budget`. Try `0` in `#seats`
for the invalid state.

1. At chat width the currency result appears first and each label fits. In the wider view it stays aligned with the assumptions.
2. The default, no-match, missing-input, and invalid states each explain what the reader can do.
3. Native inputs have visible labels, readable boundaries, and an unclipped focus ring. Verify Tab order in Open view when native checks are available.

Call `html_render` with the reviewed HTML, a short title, and a height close
to the previewed result.
