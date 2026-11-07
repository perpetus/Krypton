# Credits and provenance

Krypton is developed by the Krypton Authors. It is a Windows browser built on
Chromium, Helium and helium-windows, with its native chrome provided by
KryptonUI. Krypton retains the source identities and legal notices of the work
it builds upon.

| Project or contributors | Contribution | License or notice |
| --- | --- | --- |
| [The Chromium Authors](https://www.chromium.org/) | Chromium, Blink, V8, browser services, host UI APIs and build infrastructure | BSD-style Chromium license and the licenses of Chromium's components. [Root notice](licenses/Chromium-BSD.txt). A browser distribution must retain Chromium's generated third-party credits and component license notices. |
| [Helium / imputnet and contributors](https://github.com/imputnet/helium) | Privacy-focused browser base, patches and browser service behavior | GPL-3.0 and component-specific licenses. The current core pin is recorded in `helium-core-ref.txt`. |
| [helium-windows contributors](https://github.com/imputnet/helium-windows) | Windows port, preparation, packaging and upstream patch integration | GPL-3.0 and retained upstream notices. `upstream/helium-windows` remains the pinned browser-source submodule. |
| [The ungoogled-chromium Authors](https://github.com/ungoogled-software/ungoogled-chromium) | Chromium preparation and privacy foundations inherited through Helium | BSD 3-Clause; [LICENSE.ungoogled_chromium](LICENSE.ungoogled_chromium). |
| [Brave Software and contributors](https://github.com/brave/brave-core) | Importer implementations inherited through the upstream browser layers | Original file headers and MPL notices are retained. See [MPL-2.0](licenses/MPL-2.0.txt). No separate Brave source submodule is included in Krypton's public tree. |
| [Zen Browser and contributors](https://github.com/zen-browser/desktop) | Boost color-transform mathematics and browser UI/interaction references | The native Blink color adapter retains its MPL-2.0 header. Its Oklab duotone/inversion adaptation is in `chromium_src/third_party/blink/renderer/core/style/krypton_boost_colors.h`; [full license](licenses/MPL-2.0.txt). The implementation reference was revision `7a3552d2a9d7cba6464387e41a3fbdfab555ee7d`. |
| [Raymond Hill / gorhill and uBlock Origin contributors](https://github.com/gorhill/uBlock) | Bundled uBlock Origin content blocker | GPL-3.0. `deps.ini` pins the official release archive and its checksum; [component provenance](chromium_src/third_party/ublock/README.chromium). Its release license and filter-list notices remain part of the bundled component. |
| [Microsoft and contributors](https://github.com/microsoft/microsoft-ui-xaml) | Windows APIs, WinUI 3, Windows App SDK and C++/WinRT used by KryptonUI | Upstream package licenses and redistribution terms; see [KryptonUI's credits](https://github.com/perpetus/KryptonUI/blob/main/CREDITS.md). |
| [KryptonUI and its contributors](https://github.com/perpetus/KryptonUI) | Shared UI behavior, native Windows chrome, menus, workspaces and design assets | GPL-3.0 for original code, with the third-party exceptions and asset notices listed in its [credits](https://github.com/perpetus/KryptonUI/blob/main/CREDITS.md). |

Browser names and logos used by importers, onboarding and settings identify
their respective products. Their owners retain all trademark and artwork
rights; support for importing from a product does not imply affiliation or
endorsement.

The source licenses of third-party components remain in effect. Krypton's
[GPL-3.0 license](LICENSE) does not replace file-level MPL/BSD/MIT notices or
grant rights in third-party trademarks. Preserve the upstream licenses, generated
Chromium credits and bundled-component notices when distributing the browser.

Only Helium is retained as a browser-source submodule. Other browser research
checkouts are excluded from the public source repository; their contributions
and derived-file notices remain credited here and in KryptonUI.
