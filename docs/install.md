# Installing rmweb on the reMarkable Paper Pro

rmweb installs entirely under `/home/root/rmweb` (the only writable, OTA-surviving location). It never
modifies `/etc` or the rootfs, and it never disables xochitl — it only stops it while the browser is on
screen and restarts it on exit.

## Install from a prebuilt archive (no toolchain needed)

`scripts/package.sh` produces a self-contained release tarball (`dist/rmweb-<version>.tar.gz`, ~112 MB —
it carries the app plus every runtime library, so no build tools are needed to install it):

```sh
# 1. Produce the archive (on a machine with the build toolchain):
./scripts/build-wpeqt.sh && ./scripts/package.sh
# 2. Copy it to the tablet over USB:
scp dist/rmweb-0.5.0.tar.gz root@10.11.99.1:/home/root/
# 3. Extract + wire up on the device (no toolchain here):
ssh root@10.11.99.1 'mkdir -p /home/root/rmweb \
  && gunzip -c /home/root/rmweb-0.5.0.tar.gz | tar -C /home/root/rmweb -xf - \
  && /home/root/rmweb/install.sh'
```

Re-running the same steps upgrades in place. (A download URL for the archive lands when the project is
published to GitHub Releases; until then, copy the file across yourself.)

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
- From the home screen:    tap the **rmweb** icon (requires XOVI + rm-appload; see below).

The browser takes over the screen (xochitl is stopped). Tap the **⏻** button at the right of the toolbar
to quit — xochitl (your normal reMarkable UI) comes back automatically. xochitl is always restored on
exit, crash, or kill; a reboot always restores it too.

## Home-screen icon (optional, layer B)

The icon needs the community launcher **XOVI + rm-appload** installed on the device. With it present,
`install.sh` registers rmweb automatically. Without it, rmweb still runs from the shell (above).

## After a firmware update (OTA)

The bundle under `/home/root/rmweb` survives OTA, but re-run the installer to re-assert the icon hook:

```sh
ssh root@10.11.99.1 '/home/root/rmweb/install.sh'
```

If the home-screen icon is gone after an OTA, reinstall XOVI/rm-appload, then re-run `install.sh`.

## Logs

Runtime output goes to `/home/root/rmweb/rmweb.log` (kept under `/home` so it survives a watchdog reboot).
