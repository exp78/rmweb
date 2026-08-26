# Installing rmweb on the reMarkable Paper Pro

rmweb installs entirely under `/home/root/rmweb` (the only writable, OTA-surviving location). It never
modifies `/etc` or the rootfs, and it never disables xochitl — it only stops it while the browser is on
screen and restarts it on exit.

## Install from a prebuilt archive (no toolchain needed)

`scripts/package.sh` produces a self-contained release tarball (`dist/rmweb-<version>.tar.gz`, ~110 MB —
it carries the app plus every runtime library, so no build tools are needed to install it). Prebuilt
archives are attached to [GitHub Releases](https://github.com/exp78/rmweb/releases) — install v0.9.0
without any toolchain:

```sh
# 1. Download the archive from Releases and copy it to the tablet over USB:
scp rmweb-0.9.0.tar.gz root@10.11.99.1:/home/root/
# 2. Extract + wire up on the device (no toolchain here):
ssh root@10.11.99.1 'mkdir -p /home/root/rmweb \
  && gunzip -c /home/root/rmweb-0.9.0.tar.gz | tar -C /home/root/rmweb -xf - \
  && /home/root/rmweb/install.sh'
```

Re-running the same steps upgrades in place. To build the archive yourself instead:
`./scripts/build-wpeqt.sh && ./scripts/package.sh`, then the same two steps with `dist/rmweb-0.9.0.tar.gz`.

## Build + deploy from source (dev host)

```sh
# Build the app and assemble + deploy the bundle straight to the device over USB-SSH:
./scripts/build-wpeqt.sh
./scripts/bundle.sh
# Wire it up on the device (idempotent):
ssh root@10.11.99.1 '/home/root/rmweb/install.sh'
```

## Running

- From the device shell:  `/home/root/rmweb/rmweb [URL]`
- From the home screen:    tap the **rmweb** icon in the AppLoad launcher (requires XOVI + AppLoad; see below).

The browser takes over the screen (xochitl is stopped). Tap the **⏻** button at the right of the toolbar
to quit — xochitl (your normal reMarkable UI) comes back automatically, WITH XOVI/AppLoad if they were
running. xochitl is always restored on exit, crash, or kill; a reboot always restores it too.

## Home-screen icon (optional, layer B: XOVI + AppLoad)

The icon needs the community extension stack on the device: **XOVI** (LD_PRELOAD hook framework) +
**AppLoad** (the rm-appload XOVI extension that adds a third-party-app launcher to the shell).
With XOVI present, `install.sh` registers rmweb automatically (manifest + icon land in
`/home/root/xovi/exthome/appload/rmweb/`). Without it, rmweb still runs from the shell (above).

Verified working on Paper Pro OS 3.28 (beta) on 2026-07-26:

```sh
# 1. XOVI bundle (aarch64, ships qt-resource-rebuilder + start/stock scripts):
#    https://github.com/asivery/rm-xovi-extensions/releases (xovi-aarch64.tar.gz)
scp xovi-aarch64.tar.gz root@10.11.99.1:/tmp/
ssh root@10.11.99.1 'tar -xzf /tmp/xovi-aarch64.tar.gz -C /home/root'

# 2. AppLoad: unzip the release, put appload.so into extensions.d.
#    https://github.com/asivery/rm-appload/releases
#    !! OS 3.28: the v0.5.3 release only supports <= 3.27 and crash-loops xochitl
#    ("Couldn't resolve the hashed identifier ... required by AppLoad hooks in main UI").
#    For 3.28 build appload.so from PR #59 (branch `3.28` of rmitchellscott/rm-appload);
#    xovi/make.sh builds fine inside this repo's rmweb-sdk docker image (qmake6 + rcc are in
#    /opt/rmpp-sdk/sysroots/aarch64-codexsdk-linux/usr/{bin,libexec}, XOVI_REPO=<xovi checkout>).
ssh root@10.11.99.1 'cp appload.so /home/root/xovi/extensions.d/'

# 3. REQUIRED once (and after each OS update): rebuild the QML hashtable, else AppLoad
#    crash-loops xochitl on 3.27+. Screen is blank for ~1-2 min, then it exits by itself.
ssh root@10.11.99.1 'echo | /home/root/xovi/rebuild_hashtable'

# 4. Start XOVI (tethered): restarts xochitl with the hooks; the AppLoad entry appears in the
#    shell sidebar with the rmweb icon inside. Re-run `install.sh` once to register the icon.
ssh root@10.11.99.1 '/home/root/rmweb/install.sh && /home/root/xovi/start'
```

XOVI is **tethered by design**: its systemd drop-in lives on a tmpfs, so a reboot always returns the
device to the stock shell (this is what makes a bad extension un-brickable). To get XOVI back after a
reboot, run `/home/root/xovi/start` over SSH again; `/home/root/xovi/stock` returns to stock without a
reboot. Do NOT wire XOVI into real autostart — that is the one path to a bootloop.

## After a firmware update (OTA)

The bundle under `/home/root/rmweb` survives OTA — and so do XOVI/AppLoad themselves (they live under
`/home/root`; an OTA only rewrites `/etc` + `/usr`). XOVI just has to be re-hooked against the new
xochitl: rebuild the QML hashtable and start XOVI again (steps 3-4 above — the new OS build's QML hashes
differ), then re-run the installer to re-assert the icon hook:

```sh
ssh root@10.11.99.1 '/home/root/rmweb/install.sh'
```

## Logs

Runtime output goes to `/home/root/rmweb/rmweb.log` (kept under `/home` so it survives a watchdog reboot).
