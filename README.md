<p align="center">
  <img src="assets/krypton.png" width="112" height="112" alt="Krypton Browser logo">
</p>

<h1 align="center">Krypton Browser for Windows</h1>

<p align="center">
  <strong>The free, private browser for Windows 10 and 11, with vertical tabs, split view, workspaces and uBlock Origin built in.</strong>
</p>

<p align="center">
  <a href="https://krypton.run/download/"><strong>Download for Windows</strong></a>
  ·
  <a href="https://krypton.run">Website</a>
  ·
  <a href="https://krypton.run/whats-new/">What's new</a>
  ·
  <a href="https://github.com/perpetus/krypton-windows/releases/latest">Latest release</a>
</p>

<p align="center">
  <a href="https://github.com/perpetus/krypton-windows/releases/latest"><img src="https://img.shields.io/github/v/release/perpetus/krypton-windows?label=release&color=3b5bdb" alt="Latest Krypton release"></a>
  <img src="https://img.shields.io/badge/Windows-10%20%7C%2011-0078D6" alt="Windows 10 and 11">
  <img src="https://img.shields.io/badge/Chromium-158-3b5bdb" alt="Chromium 158">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-GPL--3.0-blue" alt="GPL-3.0 license"></a>
</p>

<p align="center">
  <img src="assets/krypton-browser-windows.webp" width="900" alt="Krypton browser on Windows with vertical tabs in the sidebar, pinned Essentials and a page open beside them">
</p>

Krypton keeps every tab in a sidebar, opens pages side by side and blocks ads
before they load. It's free, open source, and made for Windows. Krypton
0.22.1.2 uses Chromium 158.0.8127.36.

This repository holds the browser itself: Krypton's Chromium changes, privacy
features and build tools. The interface lives in
[KryptonUI](https://github.com/perpetus/KryptonUI), and downloads in
[krypton-windows](https://github.com/perpetus/krypton-windows).

## Features

- **Vertical tabs and workspaces.** Keep tabs in a sidebar, group them into
  Spaces and folders, and pin the sites you use every day as Essentials.
  Horizontal tabs are there too.
- **Split view and Glance.** Put up to four pages in one window, or open a link
  over the page with a Glance and keep your place.
- **Ad blocking built in.** The official uBlock Origin release is included and
  updates its filter lists by itself.
- **Private by default.** Krypton sends no usage statistics, and it asks before
  sending a crash report.
- **Separate profiles per Space.** Keep work and personal history, cookies and
  sign-ins apart.
- **On-device translation and voice typing.** Download a language pack once, and
  pages translate without their text leaving your computer.
- **Live Folders.** Follow RSS and Atom feeds, GitHub pull requests and issues
  from the sidebar.
- **Command bar.** Press Ctrl+T to search tabs, Spaces, history, bookmarks and
  browser actions. Ctrl+Tab shows tab previews.
- **Screenshots, Notes and Easel.** Capture an element, an area or a full page,
  write local Markdown notes, and sketch on a canvas.
- **Make it yours.** Workspace themes, compact layouts, and Boosts to change how
  individual websites look.
- **Extensions.** Install Chromium extensions and keep using the ones you rely
  on.
- **Quiet, signed updates.** Updates are verified and install after you close
  Krypton, often as a small delta download.

See the [0.22.1.2 release notes](https://github.com/perpetus/krypton-windows/releases/tag/v0.22.1.2)
for what's new.

## Download

Get the installer from [krypton.run](https://krypton.run/download/) or the
[latest release](https://github.com/perpetus/krypton-windows/releases/latest).
Krypton runs on 64-bit Windows 10 (version 1903 or later) and Windows 11, on a
processor with AVX2 (x86-64-v3).

## FAQ

### Is Krypton free?

Yes. It's free to use and free software under the GPL-3.0 licence.

### Does Krypton collect my data?

No usage statistics are sent. If Krypton crashes, it asks before sending a
crash report, and you can turn reports off.

### Will my extensions work?

Yes. Krypton is Chromium-based, so Chromium extensions install as usual.

### Is there a Mac, Linux or phone version?

Not yet. Krypton is made for Windows.

## Repositories

| Repository | What it holds |
| --- | --- |
| [Krypton](https://github.com/perpetus/Krypton) | The browser: Chromium changes, privacy features and build tools. |
| [KryptonUI](https://github.com/perpetus/KryptonUI) | The native interface: tabs, workspaces, menus and window controls, in WinUI 3 and C++. |
| [KryptonUpdater](https://github.com/perpetus/KryptonUpdater) | The Windows installers and signed automatic updates. |
| [krypton-windows](https://github.com/perpetus/krypton-windows) | Every release: installers, checksums, source archives and update packages. |

Krypton is an independent project in active development.
[Report a problem](https://github.com/perpetus/krypton-windows/issues/new/choose)
· [Credits and attribution](CREDITS.md) · [License](LICENSE)
