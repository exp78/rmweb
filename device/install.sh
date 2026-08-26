#!/bin/sh
# rmweb installer — run ON the device after deploying the bundle to /home/root/rmweb. Idempotent: safe to
# re-run, which is also the recovery step after a firmware/OTA update. See the Phase 5 design spec.
set -eu
R="${RMWEB_ROOT:-/home/root/rmweb}"
# Version single source of truth: the bundle's VERSION file (bundle.sh copies the repo-root
# VERSION there). Fall back to the built-in default so the script still works outside a bundle.
VER="$(cat "$R/VERSION" 2>/dev/null || true)"
[ -n "$VER" ] || VER="0.9.0"
fail(){ echo "[install] ERROR: $*" >&2; exit 1; }

# 1. Integrity: the launcher's hard dependencies must be present.
for f in bin/rmweb-wpeqt rmweb rmweb-env.sh; do
  [ -e "$R/$f" ] || fail "missing $R/$f — deploy the bundle first (scripts/bundle.sh)"
done

# 2. Make the launcher + installer executable.
chmod +x "$R/rmweb" "$R/install.sh" 2>/dev/null || true

# 3. Stamp the version.
echo "$VER" > "$R/VERSION"

# 4. Layer B: register with AppLoad (the Paper Pro XOVI extension) if XOVI is installed. New-format
#    layout: /home/root/xovi/exthome/appload/rmweb/{external.manifest.json,icon.png} — lives under
#    /home, which survives reboots AND OTA updates (an OTA only rewrites the rootfs: /etc + /usr;
#    afterwards /home/root/xovi/rebuild_hashtable + xovi/start re-hook XOVI into the new xochitl).
#    The entry point is appload-entry.sh (systemd-run scope wrapper — see its header). Degrade
#    gracefully if XOVI is absent: Strategy A (the standalone launcher) is unaffected.
if [ -d /home/root/xovi ] && [ -f "$R/appload/rmweb/external.manifest.json" ]; then
  # Optional integration: a failed copy must not abort the install (set -e) — degrade gracefully.
  APP_DIR=/home/root/xovi/exthome/appload/rmweb
  if mkdir -p "$APP_DIR" 2>/dev/null \
     && cp "$R/appload/rmweb/external.manifest.json" "$APP_DIR/external.manifest.json" 2>/dev/null \
     && cp "$R/appload/rmweb/icon.png" "$APP_DIR/icon.png" 2>/dev/null \
     && chmod +x "$R/appload-entry.sh" 2>/dev/null; then
    echo "[install] registered rmweb icon in $APP_DIR (start XOVI with: /home/root/xovi/start)"
  else
    echo "[install] WARN: could not write $APP_DIR — icon not registered"
  fi
else
  echo "[install] XOVI/AppLoad not found — skipping icon (launch with: $R/rmweb)"
fi

echo "[install] rmweb $VER installed under $R"
echo "[install] run:        $R/rmweb [URL]"
echo "[install] after OTA:   re-run  $R/install.sh"
