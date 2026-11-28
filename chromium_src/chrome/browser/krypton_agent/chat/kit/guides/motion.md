# Motion

Update the information immediately. Add movement when it helps the reader
follow a change they caused. Keep the resting state complete in both
themes. Run offline with root-relative `/kit/1/` assets and inline data.
Treat page text as untrusted text. The frame helper follows the
`krypton-theme` event and handles visibility.

**Art direction.** Pick the look and hierarchy in the resting state.
Keep numbers and labels in place, and use movement to clarify only the
part that changes. Give interrupted and failed operations a readable state.

## 1. Start with an immediate update

For a delivery estimate, show the new range as soon as the reader chooses
a service. Keep the selected service and the estimate readable during any
effect. The same update works with keyboard selection and reduced motion.

```html
<fieldset id="service">
  <legend>Delivery service</legend>
  <label><input type="radio" name="speed" id="standard" value="standard" checked> Standard</label>
  <label><input type="radio" name="speed" id="express" value="express"> Express</label>
</fieldset>
<output id="estimate" for="standard express">3–5 days</output>
<script type="module">
import '/kit/1/ui.js';

const service = document.querySelector('#service');
const estimate = document.querySelector('#estimate');
const ranges = {standard: '3–5 days', express: '1–2 days'};
service.addEventListener('change', ({target}) => {
  estimate.value = ranges[target.value];
});
</script>
```

## 2. Let a CSS transition follow the input

Add a decorative delivery-time bar next to that estimate when the relative
speed matters. The visible range still changes immediately. Updating the
custom property while the transition is running retargets it from its
current position. Load `/kit/1/base.css` for theme tokens and visibility
styles.

```html
<span class="delivery-bar" aria-hidden="true"></span>
<style>
  .delivery-bar {
    display: block;
    inline-size: min(100%, 12rem);
    block-size: .35rem;
    background: var(--kr-accent);
    transform-origin: left;
    transform: scaleX(var(--time, 1));
    transition: transform 180ms ease-out;
  }
  .delivery-bar:dir(rtl) { transform-origin: right; }
  @media (prefers-reduced-motion: reduce) {
    .delivery-bar { transition: none; }
  }
</style>
```

Inside the same change handler, set `--time` on `.delivery-bar` to `'.4'`
for Express and `'1'` for Standard. The bar reaches its final length
immediately under reduced motion.

## 3. Use Motion for a finite effect with cleanup

If the output itself needs a brief confirmation, use this version of the
change handler in place of the immediate handler above. Load the local
classic script `/kit/1/lib/motion@14.14.0/motion.js` before the module. It
provides `Motion.animate` and related helpers on the global object.

```js
import {whileVisible} from '/kit/1/ui.js';

const service = document.querySelector('#service');
const estimate = document.querySelector('#estimate');
const ranges = {standard: '3–5 days', express: '1–2 days'};
let effect;
let active = false;
function settle() {
  effect?.cancel();
  effect = undefined;
  estimate.style.opacity = '1';
}
const unsubscribe = whileVisible(isActive => {
  active = isActive;
  if (!active) settle();
});
function update({target}) {
  estimate.value = ranges[target.value];
  settle();
  // Visibility includes the current reduced-motion preference.
  if (active) {
    effect = Motion.animate(estimate, {opacity: [.8, 1]},
                            {duration: .18, ease: 'easeOut'});
  }
}
function dispose() {
  service.removeEventListener('change', update);
  unsubscribe();
  settle();
}
service.addEventListener('change', update);
addEventListener('pagehide', event => {
  if (!event.persisted) dispose();
});
```

Call `dispose()` if you replace the widget in place. Hiding the render or
switching to reduced motion cancels the effect and restores the final
opacity. Rapid changes replace the prior effect while the value stays
current. For a loop the user asked for, start and stop its work only via
`whileVisible` and unsubscribe when removing it. The chart helper keeps
Chart.js animation off.

## Preview and show

Use `html_preview` to capture the resting view and one changed state in
light and dark. Check the console and exercise a quick reversal with
preview steps by clicking `#express` and then `#standard`.

1. The new value is present as soon as the control changes and the resting frame reads on its own.
2. Repeated input keeps the latest value and effect in sync. Focus and input remain available during the transition.
3. With runtime checks available, hide the render or enable reduced motion mid-effect and verify that it settles immediately. Continuous work stays idle while hidden.

Call `html_render` with the reviewed HTML, title, and height.
