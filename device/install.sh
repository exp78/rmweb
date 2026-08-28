#!/bin/sh
# rmweb installer — run ON the device after deploying the bundle to /home/root/rmweb. Idempotent: safe to
# re-run, which is also the recovery step after a firmware/OTA update. See the Phase 5 design spec.
set -eu
R="${RMWEB_ROOT:-/home/root/rmweb}"
fail(){ echo "[install] ERROR: $*" >&2; exit 1; }
# Version single source of truth: the bundle's VERSION file (bundle.sh copies the repo-root
# VERSION there). A bundle always carries it — refuse to install a partial deploy without one.
VER="$(cat "$R/VERSION" 2>/dev/null || true)"
[ -n "$VER" ] || fail "missing/empty $R/VERSION — deploy the full bundle first (scripts/bundle.sh)"

# 1. Integrity: the launcher's hard dependencies must be present.
for f in bin/rmweb-wpeqt rmweb rmweb-env.sh libexec/wpe-webkit-2.0; do
  [ -e "$R/$f" ] || fail "missing $R/$f — deploy the bundle first (scripts/bundle.sh)"
done

# 1b. Link sanity: unresolved deps of the WebKit lib mean a broken (or mismatched) bundle. A bare
#     device shell has no LD_LIBRARY_PATH yet, so point it at the bundle for this check. Not fatal
#     (ldd may be absent, and a device system lib may still save us) — but warn loudly.
missing="$(LD_LIBRARY_PATH="$R/lib" ldd "$R/lib/libWPEWebKit-2.0.so.1" 2>/dev/null | grep "not found" || true)"
if [ -n "$missing" ]; then
  echo "[install] WARN: unresolved library dependencies (bundle may not start):" >&2
  echo "$missing" | sed 's/^[[:space:]]*/[install]   /' >&2
fi

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
  # The manifest hardcodes the default install path; rewrite it for a custom RMWEB_ROOT on the fly.
  APP_DIR=/home/root/xovi/exthome/appload/rmweb
  if mkdir -p "$APP_DIR" 2>/dev/null \
     && sed "s|/home/root/rmweb|$R|g" "$R/appload/rmweb/external.manifest.json" > "$APP_DIR/external.manifest.json" 2>/dev/null \
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
