#!/bin/sh
# AppLoad entry point for rmweb (registered via exthome/appload/rmweb/external.manifest.json).
# AppLoad spawns external apps as CHILDREN of xochitl (QProcess), but the rmweb launcher
# stops xochitl to take the framebuffer — and `systemctl stop xochitl` SIGTERMs every
# process in xochitl's cgroup, which would kill this launcher before the browser starts.
# Re-parent into an independent systemd scope first: the cgroup kill then can't reach us,
# and the launcher's EXIT trap brings xochitl (with XOVI/AppLoad) back on quit (⏻ button).
set -u
R="${RMWEB_ROOT:-/home/root/rmweb}"
if command -v systemd-run >/dev/null 2>&1; then
  exec systemd-run --unit=rmweb-appload --scope --quiet "$R/rmweb"
fi
# Fallback without systemd-run: run directly — xochitl's stop will kill us, but the
# launcher's TERM trap still restarts xochitl, so the device never strands on a blank panel.
exec "$R/rmweb"
