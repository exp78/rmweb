# rmweb

A native **WPE WebKit** web browser for the **reMarkable Paper Pro** e-ink tablet.

> **Status: v0.8.0 — beta.** The primary use case is **reading**; general browsing is basic.
> Implemented and verified on-device: reader mode (Mozilla Readability), B2 chrome painted into the
> frame (with C++ hit-test), touch/pen input via evdev with a phantom-touch guard, on-screen URL
> keyboard, bookmarks/history/settings persisted in the profile dir, HTML start page (`rmweb:` scheme),
> page/reader zoom, content blocking (WebKit UserContentManager filter), and a no-brick launcher that
> stops/restores xochitl. E-ink-safe: CPU-only llvmpipe + Skia, ~120–250 ms page turns, low RAM.
> Implemented (host-tested, on-device verification pending): persistent cookies (sqlite), per-URL
> scroll restore, in-page find (`/text` in the address bar), downloads to `~/Downloads`, tabs-lite
> (open-pages switcher on the start page), reader dark theme (start page → Settings).
> A 2026-07-18 code review ([docs/review-2026-07-18.md](docs/review-2026-07-18.md)) found open
> security/robustness issues — not release-ready yet.

## Why it's interesting

The Paper Pro is a GPU-less, CPU-only aarch64 e-ink device (i.MX8M Mini, Yocto scarthgap).
rmweb renders the web entirely in software — **Skia CPU raster + Mesa llvmpipe (software EGL)** —
and presents through reMarkable's e-ink display path.

## Architecture (short)

```
input (touch/pen) → shell (Qt6/QML chrome) → engine (WPE, software GL)
        ARGB8888 frame → display (Qt6 + epaper QPA) → imx-drm → E-Ink 1620×2160
```

Five isolated modules: `engine`, `display`, `input`, `shell`, `platform`.
Full design: [`docs/superpowers/specs/2026-06-24-rmweb-browser-design.md`](docs/superpowers/specs/2026-06-24-rmweb-browser-design.md).
Verified hardware facts: [`docs/device-profile.md`](docs/device-profile.md).

## Target device

reMarkable Paper Pro ("Ferrari"), Codex Linux (scarthgap), aarch64, kernel 6.12.49, **no GPU**.
Everything installs under `/home/root/rmweb` (the rootfs is full). Cross-compiled with the
official reMarkable "ferrari" Yocto SDK.

## Build & Install

```bash
./scripts/fetch-sdk.sh          # download Yocto SDK once
./scripts/build-wpeqt.sh        # build rmweb-wpeqt
./scripts/bundle.sh             # create device bundle
./scripts/run-wpeqt-on-device.sh show https://example.com
```

Full instructions: [`docs/install.md`](docs/install.md)

## Roadmap / Planned

Not implemented yet (earlier docs claimed some of these by mistake — see the review above):
password manager, autofill, history search, on-device JS console, user/content scripts,
TLS indicator, reading-progress bar, performance dashboard.

## License

[MIT](LICENSE) — see the file for details. Co-developed with AI assistance.
