# rmweb

A native **WPE WebKit** web browser for the **reMarkable Paper Pro** e-ink tablet.

> **Status: Mature reading browser (Phase 5 complete)**. Fast, verified on-device e-ink web reader
> with B2 chrome, reader mode, touch gestures, on-screen keyboard, bookmarks, history, zoom,
> phantom-touch protection and ~150ms page turns. Built on WPE WebKit + Mesa llvmpipe (CPU-only).

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

## License

[MIT](LICENSE) — see the file for details. Co-developed with AI assistance.

Ready for public release on GitHub.
