# rmweb

A native **WPE WebKit** web browser for the **reMarkable Paper Pro** e-ink tablet.

> Status: **early development** (foundations). Starting as a fast e-ink *reading* browser,
> growing toward a general-purpose browser within the hardware limits.
> First known attempt to run WPE WebKit on reMarkable e-ink.

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

## Build / install

Cross-compilation toolchain, build env, and install steps land as Phase 0–5 progress.
See the spec for the phased roadmap.

## License

TBD before public release.
