# rmweb production runtime env — the single source of truth, sourced by device/rmweb and the dev runner.
# Expects $R (bundle dir) set by the caller; falls back to RMWEB_ROOT / the default install path.
: "${R:=${RMWEB_ROOT:-/home/root/rmweb}}"

export LD_LIBRARY_PATH="$R/lib"
export GALLIUM_DRIVER=llvmpipe LIBGL_ALWAYS_SOFTWARE=1 EGL_PLATFORM=surfaceless   # llvmpipe = fast SW GL
export LIBGL_DRIVERS_PATH="$R/lib/dri" __EGL_VENDOR_LIBRARY_DIRS="$R/share/glvnd/egl_vendor.d"
export WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1
export WEBKIT_INJECTED_BUNDLE_PATH="$R/lib/wpe-webkit-2.0/injected-bundle"
export WEBKIT_SKIA_ENABLE_CPU_RENDERING=1
export WEBKIT_SKIA_CPU_PAINTING_THREADS="${RMWEB_SKIA_THREADS:-0}"  # 0=main-thread (safe); >0 for perf (tuned)
export WEBKIT_DISABLE_ASYNC_SCROLLING=1
export WEBKIT_FORCE_VBLANK_TIMER=1  # stabilizes frame cadence on e-ink
export GIO_EXTRA_MODULES="$R/lib/gio/modules"          # glib-networking OpenSSL TLS backend -> https:// works
export FONTCONFIG_PATH=/etc/fonts HOME=/home/root
# JS: interpreter default (stable). RMWEB_JIT=1 enables baseline JIT + polling traps (avoids DFG/FTL crashes/aborts; see jit-works-polling-traps + Phase 2 hardening). Added JSC_useBaselineJIT.
export JSC_useJIT="${RMWEB_JIT:-0}"
[ "${RMWEB_JIT:-0}" = 1 ] && { export JSC_useBaselineJIT=1; export JSC_usePollingTraps=1; export JSC_useDFGJIT=0; export JSC_useFTLJIT=0; }
for opt in ${RMWEB_JSC_OPTS:-}; do export "$opt"; done  # extra JSC_* (e.g. JSC_useJIT=1 JSC_verbose=1 for diagnostics)
export RMWEB_BLOCK RMWEB_UA RMWEB_SITECSS               # content/UA/readability levers (pass-through to the app)
# Display path: epaper QPA, basic render loop (so afterRendering fires on the GUI thread), SW vblank timer.
export QT_QPA_PLATFORM=epaper QT_QUICK_BACKEND="${RMWEB_QUICK_BACKEND:-epaper}" QSG_RENDER_LOOP=basic
export WEBKIT_FORCE_VBLANK_TIMER="${WEBKIT_FORCE_VBLANK_TIMER:-1}"
# On-screen URL keyboard (Qt Virtual Keyboard): IM module + bundle import/plugin paths EXTEND device defaults.
export QT_IM_MODULE=qtvirtualkeyboard
export QML_IMPORT_PATH="$R/qml" QML2_IMPORT_PATH="$R/qml"
export QT_PLUGIN_PATH="$R/plugins:${QT_PLUGIN_PATH:-/usr/lib/plugins}"
export RMWEB_PRESENT_DWELL RMWEB_DPR RMWEB_READER_FONT RMWEB_READER_DIR RMWEB_FULL_EVERY  # runtime levers
