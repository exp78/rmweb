# Installing rmweb on the reMarkable Paper Pro

rmweb installs entirely under `/home/root/rmweb` (the only writable, OTA-surviving location). It never
modifies `/etc` or the rootfs, and it never disables xochitl — it only stops it while the browser is on
screen and restarts it on exit.

## First install (from the dev host)

```sh
# 1. Build the app and assemble + deploy the bundle (stops nothing; just copies files):
./scripts/build-wpeqt.sh
./scripts/bundle.sh
# 2. Wire it up on the device (idempotent):
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
