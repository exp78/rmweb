#!/usr/bin/env bash
set -uo pipefail
# Batch on-device verification of the ui+browser batch (error page, TLS lock, address-bar search,
# autofill, password store, reading-progress bar) plus a start-page regression grab.
# Every step deploys the CURRENT build/rmweb-wpeqt via run-wpeqt-on-device.sh in show mode,
# grabs the screen (RMWEB_GRAB_MS) and pulls grab + full log back to build/verify/<step>.{png,log}.
# Verdicts are HINTS (log greps) — the real check is eyeballing the PNGs.
# Prereqs: device on USB (10.11.99.1), ./scripts/build-wpeqt.sh run. Safe to re-run; ~4 min.
cd "$(dirname "$0")/.."
[ -f .env ] && . ./.env || true
HOST="${REMARKABLE_HOST:-10.11.99.1}"; DUSER="${REMARKABLE_USER:-${DEVICE_USER:-root}}"
OUT=build/verify; mkdir -p "$OUT"
SRV=build/verify-www; mkdir -p "$SRV"
MAC_IP=10.11.99.5; SRV_PORT=8765
SRV_PID=""

cleanup(){ [ -n "$SRV_PID" ] && kill "$SRV_PID" 2>/dev/null; }
trap cleanup EXIT

echo "== [0/9] waiting for the device =="
up=0
for i in $(seq 1 12); do
  if ssh -o ConnectTimeout=5 "$DUSER@$HOST" true 2>/dev/null; then up=1; break; fi
  echo "  attempt $i/12 ..."; sleep 10
done
[ "$up" = 1 ] || { echo "DEVICE UNREACHABLE — replug USB / long-press power, then re-run"; exit 1; }

fresh(){ # clear stale lock/log/grab (NOT the profile — learned autofill/passwords must survive)
  ssh "$DUSER@$HOST" 'rmdir /home/root/rmweb/.lock 2>/dev/null; rm -f /home/root/rmweb/wpeqt.log /home/root/rmweb/grab.png' || true
}
pull(){ # pull <step>: grab.png + wpeqt.log -> build/verify/<step>.{png,log}
  scp -q "$DUSER@$HOST:/home/root/rmweb/grab.png" "$OUT/$1.png" 2>/dev/null \
    && sips -Z 760 "$OUT/$1.png" >/dev/null 2>&1 && echo "[grab] $OUT/$1.png" || echo "[grab] MISSING ($1)"
  scp -q "$DUSER@$HOST:/home/root/rmweb/wpeqt.log" "$OUT/$1.log" 2>/dev/null || echo "[log] MISSING ($1)"
}
hint(){ # hint <logfile> <grep-pattern> <ok-text>
  if grep -aq "$2" "$1" 2>/dev/null; then echo "[hint] OK  — $3"; else echo "[hint] ??? — no '$2' in log (check grab)"; fi
}

echo "== [1/9] start page (regression) =="
fresh
SHOW_SECS=8 RMWEB_GRAB_MS=5000 ./scripts/run-wpeqt-on-device.sh show
pull start

echo "== [2/9] error page + Retry =="
fresh
SHOW_SECS=12 RMWEB_GRAB_MS=9000 ./scripts/run-wpeqt-on-device.sh show "http://$MAC_IP:9999/dead"
pull error-page
hint "$OUT/error-page.log" '\[nav\] load failed' 'load-failed fired'

echo "== [3/9] address-bar search =="
fresh
SHOW_SECS=9 RMWEB_DEBUG_SEARCH="wikipedia" RMWEB_GRAB_MS=6500 ./scripts/run-wpeqt-on-device.sh show
pull search

echo "== [4/9] TLS padlock (needs internet on the device) =="
fresh
SHOW_SECS=16 RMWEB_GRAB_MS=13000 ./scripts/run-wpeqt-on-device.sh show https://example.com
pull tls
hint "$OUT/tls.log" '\[tls\] secure=1' 'https without cert errors'

# Steps 5-8 need a local HTTP server on the Mac (the USB-link peer). Skipped if the link IP is absent.
if ifconfig | grep -q "$MAC_IP"; then
  cat > "$SRV/form.html" <<'EOF'
<!DOCTYPE html><html><head><meta charset="utf-8"><style>body{font-family:sans-serif}input{font-size:24px}</style></head><body>
<h2>rmweb verify form</h2>
<input name="email" type="email" placeholder="email" style="position:absolute;left:40px;top:200px;width:300px">
<input name="user" type="text" placeholder="login" style="position:absolute;left:40px;top:280px;width:300px">
<input name="pass" type="password" placeholder="password" style="position:absolute;left:40px;top:360px;width:300px">
</body></html>
EOF
  { echo '<!DOCTYPE html><html><head><meta charset="utf-8"><style>body{font-family:sans-serif;font-size:30px}</style></head><body>';
    for n in $(seq 1 300); do echo "<p>Line $n — the quick brown fox jumps over the lazy dog.</p>"; done
    echo '</body></html>'; } > "$SRV/long.html"
  echo '<!DOCTYPE html><html><head><meta charset="utf-8"><title>blank</title></head><body></body></html>' > "$SRV/blank.html"
  python3 -m http.server "$SRV_PORT" --bind "$MAC_IP" --directory "$SRV" >/dev/null 2>&1 &
  SRV_PID=$!
  sleep 1
  # Probe/commit coords: panel px = CSS px * dpr * zoom. A zoom left over in the profile shifts
  # every hit target, so pin it to 1.0 for the coordinate-based steps (RMWEB_DPR=2 forced too).
  ssh "$DUSER@$HOST" "sed -i 's/^zoom=.*/zoom=1.0/' /home/root/.rmweb/settings.txt" 2>/dev/null || true
  FORM="http://$MAC_IP:$SRV_PORT/form.html"

  echo "== [5/9] autofill: learn email, then prefill on next visit =="
  fresh
  SHOW_SECS=12 RMWEB_DPR=2 RMWEB_DEBUG_FORM="380,430,tester@example.com" RMWEB_GRAB_MS=9500 \
    ./scripts/run-wpeqt-on-device.sh show "$FORM"
  pull autofill-learn
  hint "$OUT/autofill-learn.log" 'autofill learned kind=1' 'email learned (kind=1)'
  fresh
  SHOW_SECS=8 RMWEB_DPR=2 RMWEB_DEBUG_PROBE="380,430" RMWEB_GRAB_MS=6000 \
    ./scripts/run-wpeqt-on-device.sh show "$FORM"
  pull autofill-prefill    # expect: keyboard opens prefilled + "Autofill" toast

  echo "== [6/9] password store: learn, then prefill =="
  fresh
  SHOW_SECS=12 RMWEB_DPR=2 RMWEB_DEBUG_FORM="380,750,s3cret!" RMWEB_GRAB_MS=9500 \
    ./scripts/run-wpeqt-on-device.sh show "$FORM"
  pull pw-learn
  hint "$OUT/pw-learn.log" 'password saved for' 'host password stored'
  fresh
  SHOW_SECS=8 RMWEB_DPR=2 RMWEB_DEBUG_PROBE="380,750" RMWEB_GRAB_MS=6000 \
    ./scripts/run-wpeqt-on-device.sh show "$FORM"
  pull pw-prefill          # expect: keyboard opens prefilled (masked echo)

  echo "== [7/9] reading-progress bar (auto page-turns on a long page) =="
  fresh
  # The auto-pager ALTERNATES direction (4 s down, 8 s up, ...), so grab at 6 s — right after the
  # first page-down — or the shot lands back at the top where the fill is zero-width (invisible).
  SHOW_SECS=9 RMWEB_DPR=2 RMWEB_AUTOPAGE_MS=4000 RMWEB_GRAB_MS=6000 \
    ./scripts/run-wpeqt-on-device.sh show "http://$MAC_IP:$SRV_PORT/long.html"
  pull progress            # expect: black fill along the bottom edge after the first turn

  echo "== [8/9] blank page -> clean render notice =="
  fresh
  # A genuinely blank page (loads fine, paints nothing) must show the "Couldn't display" notice as
  # a clean WHITE page state — no stale previous site bleeding through around the box.
  SHOW_SECS=22 RMWEB_GRAB_MS=16000 ./scripts/run-wpeqt-on-device.sh show "http://$MAC_IP:$SRV_PORT/blank.html"
  pull blank-notice
  hint "$OUT/blank-notice.log" '\[render\] nonWhite=0 blank=1' 'blank verdict after load finished'
else
  echo "== steps 5-8 SKIPPED: $MAC_IP not configured on this Mac (USB link down?) =="
fi

echo "== [9/9] done =="
echo "Review the grabs: ls -la $OUT/*.png"
