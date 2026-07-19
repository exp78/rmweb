#!/bin/sh
# rmweb installer — run ON the device after deploying the bundle to /home/root/rmweb. Idempotent: safe to
# re-run, which is also the recovery step after a firmware/OTA update. See the Phase 5 design spec.
set -u
R="${RMWEB_ROOT:-/home/root/rmweb}"
VER="0.8.0"
fail(){ echo "[install] ERROR: $*" >&2; exit 1; }

# 1. Integrity: the launcher's hard dependencies must be present.
for f in bin/rmweb-wpeqt rmweb rmweb-env.sh; do
  [ -e "$R/$f" ] || fail "missing $R/$f — deploy the bundle first (scripts/bundle.sh)"
done

# 2. Make the launcher + installer executable.
chmod +x "$R/rmweb" "$R/install.sh" 2>/dev/null || true

# 3. Stamp the version.
echo "$VER" > "$R/VERSION"

# 4. Layer B: register with rm-appload if it's installed (its apps dir lives under /home, so it survives
#    reboot). Format/path verified on-device — degrade gracefully (Strategy A is unaffected) if absent.
APPLOAD_DIR=
for d in /home/root/.config/rm-appload/apps /opt/etc/draft /home/root/.entware/etc/draft; do
  [ -d "$d" ] && { APPLOAD_DIR="$d"; break; }
done
if [ -n "$APPLOAD_DIR" ] && [ -f "$R/appload/rmweb.draft" ]; then
  cp "$R/appload/rmweb.draft" "$APPLOAD_DIR/rmweb.draft"
  echo "[install] registered rmweb icon in $APPLOAD_DIR"
else
  echo "[install] rm-appload not found — skipping icon (launch with: $R/rmweb)"
fi

echo "[install] rmweb $VER installed under $R"
echo "[install] run:        $R/rmweb [URL]"
echo "[install] after OTA:   re-run  $R/install.sh"
