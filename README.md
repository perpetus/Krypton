# Krypton

See [credits and provenance](CREDITS.md) and the
[public source preparation notes](docs/public-source-preparation.md).

Krypton is a Chromium-based browser for Windows, built as a thin layer on top
of [Helium](https://github.com/imputnet/helium). Its UI is being rebuilt
outside Chromium as [KryptonUI](https://github.com/perpetus/KryptonUI); see
[docs/kryptonui-integration.md](docs/kryptonui-integration.md).

**Browser version:** Krypton 0.22.1.1 uses Chromium 158.0.8127.36. This version
was staged in a separate development runtime by updating Windows version
metadata and verifying that the compiled code stayed byte-identical.
The [157 preparation report](docs/helium-157-78-preparation.md) records the
previous source baseline.

## How the layers fit

```
 Chromium 158 (Helium's prepared archive)
   + Helium            upstream/helium-windows/helium-chromium   pinned, never edited,
                                                                 less its browser-chrome UI
   + helium-windows    upstream/helium-windows                   pinned, never edited
   + Krypton           patches/, chromium_src/, resources/, flags.krypton.gn
```

Helium's browser-chrome UI patches (toolbar, tabs, location bar, app menu,
frame, side panel, layouts) are not applied: KryptonUI replaces that layer,
and its hooks patch Chromium's own window code directly, so Krypton is not
held up while Helium re-ports its UI to a new Chromium. `patches/skip-helium`
lists them. Everything else Helium does is applied. The integration patches
connect normal BrowserViews to the required native shell. The 157 patch series,
full source preparation, targeted GN checks and the Chromium 157 browser,
driver, native installer and Zucchini builds pass. Native browser checks and
network acceptance remain pending.
There is no automatic runtime fallback.

Four rules keep updates cheap:

1. **Upstream is pristine.** Helium and helium-windows are git submodules at a
   release tag or an explicitly recorded custom core commit. Nothing in
   `upstream/` is edited, renamed or rebranded;
   `tools/sync_helium.py` refuses to run if it has local changes.
2. **Helium wins for what Krypton applies.** Krypton's layer is applied last
   and adapts to Helium, never the reverse. When the two disagree, Helium's
   behaviour stands unless Krypton has a reason it can state in its patch
   header. `build.py` enforces this for GN args: `flags.krypton.gn` may add
   arguments but may not reassign one Helium sets, except the approved named
   Cast build exceptions (`enable_mdns` and `enable_service_discovery`).
3. **Skip whole patches, never parts of them.** Leaving out Helium's UI is
   done by listing patches in `patches/skip-helium`. Where a kept Helium patch
   refers to a skipped one, a small Krypton patch removes the reference
   (`patches/krypton/without-helium-ui/`).
4. **Only Krypton's changes carry Krypton's name.** Code from Helium,
   ungoogled-chromium, Brave and the rest keeps its own identifiers, file
   names and preference keys. User-visible product text and art say Krypton;
   internal pages use `krypton://`. Legal text, importer source names and
   website-visible identities retain their originals. Krypton patches touch
   only the files they must, and
   refer to upstream code by its upstream name (`helium::`,
   `components/helium_services`, `brave_profile_import_impl.cc`).

## Repository layout

| Path | What it is |
|---|---|
| `upstream/helium-windows/` | helium-windows, pinned. Its own `helium-chromium/` submodule is the Helium core it was released with. |
| `patches/series`, `patches/krypton/` | Krypton's patches, applied after both upstream series. |
| `patches/skip-helium` | Helium patches Krypton leaves out: its browser-chrome UI. |
| `chromium_src/` | New files Krypton owns inside the Chromium tree (the KryptonUI contract and native service adapters). |
| `resources/core/` | Krypton's art, under Helium's file names. The build lays it over Helium's resources and lets Helium's lists place it. |
| `flags.krypton.gn` | Krypton's GN args: add-only. |
| `deps.ini` | Additional Krypton inputs, including the checksum-pinned official uBlock Origin release. |
| `build.py` | helium-windows' build, step for step, with Krypton's additions marked `Krypton:`. |
| `package.py` | Runs helium-windows' packaging against Krypton's tree. |
| `tools/krypton/layout.py` | Every path and the layer order, stated once. |
| `tools/validate_series.py` | Applies all layers to a small Chromium subset in about a minute. |
| `tools/sync_helium.py` | Moves to a new Helium release and reports what Krypton must react to. |
| `version.txt` | Canonical Windows product version shared by compilation and packaging; Chromium's engine version stays upstream. |
| `helium-core-ref.txt` | Optional custom-port override; absent for the released Windows/core pair. |
| `patches/skip-helium-windows` | Windows patch exclusions; currently empty because upstream removed the obsolete backports. |
| `tools/sync_prepared_tree.py` | Synchronizes an independently prepared source upgrade while preserving `out/Default` and verifying tracking hashes. |
| `build/` | Generated and ignored: the source tree, downloads, validation subset. |

## Krypton's layer

| Patch | Why Krypton carries it |
|---|---|
| `identity/branding.patch` | Product name and company in `BRANDING`. |
| `identity/service-endpoints.patch` | Removes Helium service/update endpoints and disables its crash collector; the later Sentry patch selects Krypton's separately configured crash endpoint. |
| `services/sentry-crash-reporting.patch` | Configurable HTTPS Crashpad endpoint and build metadata for Krypton's Sentry project; preserves Ask/Automatic/Disabled consent and policy restrictions. |
| `windows/install-identity.patch` | Profile folder, app IDs, ProgIDs, URL scheme, COM classes and policy key distinct from Helium's, so both can be installed. |
| `build/simd-and-max-optimization.patch` | x86-64-v3 code generation and the args that select it. |
| `net/doh-minimal-headers.patch` | Smaller DNS-over-HTTPS requests. |
| `services/use-original-web-store.patch` | Extensions come from the Chrome Web Store's own endpoints, not Helium's proxy. |
| `services/direct-webstore-protocol.patch` | Sends the actual browser version for direct CRX downloads and recognizes the store endpoint for existing trust checks and updates. |
| `services/official-ublock-origin.patch` | Uses the official release identity and removes the services-managed filter bootstrap, toggle and extension reloads. |
| `chromium-157/` | Modular native targets, direct production dependencies and unused service-header cleanup for Chromium 157. |
| `services/component-updates-upstream-fallback.patch` | Establishes the component fallback; later privacy patches replace Widevine's consent condition. |
| `privacy-features/` | Default-off optional-feature registry, native settings, Widevine, Live Caption and downloaded autoplay list; unpublished Cast discovery gates. |
| `identity/visible-branding.patch` | Internal scheme, product art and neutral service wording; the build pass updates product strings and translations. |
| `identity/persisted-profile-branding.patch` | Upgrade generated profile/cache/shortcut defaults while preserving custom and managed names; see the [runtime audit](docs/persisted-branding-audit.md). |
| `importer/helium-thorium-profile-import.patch` | Import from Helium and Thorium profiles; fix Arc's Windows profile path and enable Dia profile discovery on Windows. |
| `without-helium-ui/custom-shortcuts.patch` | Helium's custom shortcuts listed its layout commands; those come from skipped patches. |
| `webui/0001-monochrome-settings.patch` | A monochrome light/dark Chromium Settings theme, built from existing WebUI resources; system high-contrast colors stay native. |

Every patch starts with a header saying what it changes and why.

The bundled blocker is the official uBlock Origin Chromium release (currently
1.76.6), pinned with its SHA-256 in `deps.ini`. Upstream's original package is
still a preparation input for its host-component patches. After those patches
and substitutions finish, `tools/krypton/official_ublock.py` replaces the entire
extension payload with the official archive. Only native build metadata and the
public key from the official signed release are added; scripts, defaults and
the filter catalog remain unchanged. Patch validation performs the same stage.

Filter lists update directly using uBlock Origin's own catalog and controls,
independently of the services switches. Extension code updates with the pinned
package in a browser build. The official release ID is
`fkgkibajhfbepljeaefdnfnegdcjomkh`; it starts with its own settings and cache.
The old component's profile data is left in place. Custom rules and settings
can be transferred with uBlock Origin's backup and restore controls.

## Building

Requirements are helium-windows' (see `upstream/helium-windows/README.md`):
Visual Studio set up as Chromium's Windows build instructions describe,
Python 3.11 or newer, and 7-Zip.

```powershell
git clone --recurse-submodules https://github.com/perpetus/Krypton.git
cd Krypton
python build.py --configure-only -j 8          # prepare, stage UI, generate Ninja graph
python -c "import build, os; os.chdir('build/src'); build._run_build_process('ninja', '-C', 'out/Default', '-j', '8', 'chrome')"
python build.py --resume --configure-only -j 8 # restage/configure an existing tree
python package.py            # installer, mini installer and portable zip
```

`build.py` takes helium-windows' options as well (`--dev`, `--clone`,
`--arm`, `-j`). It refuses to prepare over an existing tree, because upstream's
prepare patches in place and fails on a tree that is already patched.

Windows x64 builds require a KryptonUI checkout, selected by
`--krypton-ui <dir>`, `KRYPTON_UI_DIR`, or the default sibling `../KryptonUI`. The complete
Release Runtime component (module, bootstrap DLL and six settings images) is
staged before GN runs. Compile parallelism is configurable, with no fixed
eight-job ceiling. The pinned Chromium source requires Windows SDK
`10.0.28000.0`.

Use `--prepare-only` to create the full source tree without compilation, or
`--configure-only` to also stage KryptonUI and generate the build graph. The
browser service completion and final installer integration remain pending; see
[the implementation status](docs/kryptonui-integration.md).

To resume an already configured tree from an ordinary PowerShell window,
run native Ninja through the Visual Studio environment helper:

```powershell
python -c "import build, os; os.chdir('build/src'); build._run_build_process('ninja', '-C', 'out/Default', '-j', '8', 'chrome')"
```

This example selects eight compilation jobs while reusing the existing graph,
outputs and compiler cache. It does not prepare, regenerate or clean the source
tree. Use native Ninja for local builds rather than autoninja or Siso. The
equivalent launcher is `python tools/local_build.py [-j N]`: omit `-j` for Ninja's
automatic choice, choose any positive count, or explicitly use `-j 0` for
unlimited jobs. Linker processes default to one. Browser LLD/ThinLTO workers
default to automatic; helper-link workers remain at one, independently of
compilation. KryptonUI's helper accepts `-Jobs N`
and `-Jobs 0` with the same meaning.

Official Windows browser links use ThinLTO optimization level 3 and code-generation
level 3, alongside the existing `-O3`, AVX2/FMA, PGO and section elimination.
See [optimized Windows build settings](docs/windows-build-optimization.md) for
the supported flags, validation and concurrency controls.

## Checking patches without a build

```powershell
python tools/validate_series.py extract   # once per Chromium version
python tools/validate_series.py apply     # after any patch change
```

`extract` pulls only the ~1,600 files the series touch from Helium's prepared
archive (downloading it if needed) plus the uBlock and WinSparkle sources
Helium patches. `apply` copies that subset and applies Helium, helium-windows
and Krypton with the same `patch` flags the build uses, reporting every failure
and any fuzzy Krypton hunk instead of stopping at the first. It then reports
`[REF]` for any name that only a skipped Helium patch defines but the patched
tree still uses, which would otherwise surface as a compile error.

## Updating Helium

```powershell
python tools/sync_helium.py          # newest helium-windows release
python tools/validate_series.py extract
python tools/validate_series.py apply
```

See [docs/upstream-sync.md](docs/upstream-sync.md).

## Status

| | |
|---|---|
| Helium 0.21.3 on Chromium 157.0.8103.78 | 280 selected patches apply; 74 browser-chrome UI patches skipped |
| helium-windows 0.21.3.1 (upstream prerelease) | All 26 patches apply without a compatibility replacement |
| Krypton patches on top | All 55 patches apply, none with fuzz; includes the opaque page-card corner overlay hook, compositor defaults and consent-preserving Sentry transport |
| Kept patches referring to skipped ones | Zero dangling names in the validated source subset |
| Krypton GN args | .78 GN regeneration passes; requested performance switches are enabled locally, with IPRA off |
| Windows efficiency controls | Built and staged on Chromium 158. Includes background renderer QoS, Energy Saver tiers, metered update handling, Segment Heap and one fewer Crashpad process in official Release. Runtime and performance verification pending. |
| [Native performance and memory fixes](docs/performance-fixes-2026-11-14.md) | Maximized page-card corners, idle GPU devices, site-style IPC and palette gating are updated; production UI/host builds and development staging pass without warnings; native/performance acceptance remains pending |
| [Opaque page-card corner overlays](docs/page-corner-overlays-2026-11-14.md) | Rounded maximized cards restored; single-page opaque frames use sibling corner layers with no page-path mask. Production chrome/UI builds and staging pass without warnings; benchmark-session pixel/performance acceptance is complete. Split/Glance/acrylic retain masks. |
| [Compositor defaults and device-pixel corners](docs/compositor-trio-2026-11-14.md) | Three measured compositor features are enabled by default, with parameters unchanged. Host/normal release-max UI builds and staging pass without warnings. The benchmark session accepted maximized/restored corners, including the invisible restored right-edge half-pixel coverage difference, and confirmed the trio gain. SCE returned false and is excluded. Changes remain uncommitted and development-only. |
| `gn gen`, compile, run | The .78 production targets build successfully with the requested flags and preserved release-max UI. Final native Ninja dry run reports no work. Rust target-feature, SDK-header and ThinLTO cache warnings are documented. Runtime/performance verification remains deferred. See [upgrade notes](docs/helium-157-78-preparation.md). |
| KryptonUI host adapter | BrowserView-owned shell, real tabs/navigation, Chromium omnibox, toolbar service pane, menus, anchors and resolved backdrop wired; browser links successfully; remaining services and distribution qualification pending |
| Persistent workspaces | Saved per-profile workspace catalog and dedicated menu, WinUI create/edit dialog, tab assignment, and Chromium session/Recently Closed hooks implemented; native verification pending; shared-window profile switching remains a later stage |
| Extension integration | Installed extension models, native menu, saved pins, toolbar icons and Chromium popup hosts wired; native GUI verification pending |
| Tab hover cards | Chromium thumbnail, tab resource usage and localized hover-card labels wired to theme-aware WinUI cards; native GUI verification pending |
| Vertical site tools | Chromium page actions, extensions and site/security commands projected into the WinUI tools panel; native acceptance pending |
| Essentials | Profile-backed saved entries and session membership implemented; native restart/restore acceptance pending |
| [Crash-recovery tab duplication](docs/crash-tab-restore-2026-11-14.md) | Saved Essential recreation now waits for crash recovery or dismissal, preventing startup copies from competing with recovered tab identities/pinned state. Incremental production build passes without warnings; manual crash-recovery acceptance remains pending. |
| [Hosted crash reporting](docs/sentry-crash-reporting-2026-11-14.md) | Existing Crashpad transport configured for Sentry, retaining Ask/Automatic/Disabled consent. Production builds and staging pass without warnings; all 22 PE/PDB files are verified in Sentry. A user-authorized native crash produced a valid local dump; the running profile's Disabled setting was preserved, so hosted ingestion and event symbolication remain unverified. |
| Tab and empty-space context menus | Real Chromium commands, target-specific saved workspace moves, compact/right-side preferences and native Appearance settings wired; native GUI verification pending |
| Windows browser updates | Pinned-key helper verification, signed GitHub releases, deferred installation, and About/relaunch integration implemented; live release acceptance pending. See [Windows updates](docs/windows-updates.md). |

What changed from the previous Krypton tree, and what was left behind, is in
[docs/migration-from-legacy.md](docs/migration-from-legacy.md).

## License

GPL-3.0, as Helium. Parts derived from ungoogled-chromium are under its BSD
license; see `LICENSE.ungoogled_chromium`.

See [CREDITS.md](CREDITS.md) for upstream contributors, source provenance and
third-party license exceptions, including the MPL-covered Boost color adapter.

The on-demand page capture, universal command launcher and new-profile host
initialization crash correction are Release-built and staged in the development
runtime. UI/C11 checks pass without warnings; the host has a ThinLTO cache-pruning
warning. Native behavior, privacy and performance acceptance remain pending.

| Additional feature | Status |
|---|---|
| Local translation and voice | The Chromium 158 development build includes both local engines, native pack management and voice model selection. The translation panel and explicit pack requests were updated. Release-max build and staging verified. Native acceptance pending and performance unmeasured. |
| Windows performance defaults | 28 requested field-trial defaults implemented in source, preserving feature overrides and other platforms. Combined Chromium 158 production build in progress; runtime/performance acceptance pending. |
| Notes and Easel | URL maps and the untrusted origin source key are corrected, production-built and development-staged. All 380 packed resources and runtime manifests verified; editor/native/privacy acceptance pending. |
| Hosted-profile shutdown | Deferred close sequencing is production-built and development-staged in the 16 November combined host build. No new dump was available; runtime acceptance pending. |
| Screenshot overlay | Unified dimmed overlay, element hover, drag/resize handles, Copy/Download and visible/full-page controls are production-built and development-staged. Native acceptance pending; performance unmeasured. |
