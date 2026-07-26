# CLAUDE.md — rmweb (WPE WebKit browser for reMarkable Paper Pro)

## What this is
A native web browser for the **reMarkable Paper Pro** e-ink tablet, engine = **WPE WebKit**,
rendered **entirely on CPU** (no GPU on the device). MVP = reading browser → later full browser.
Read `docs/superpowers/specs/2026-06-24-rmweb-browser-design.md` (design) and
`docs/device-profile.md` (verified hardware facts) before working.

## Connecting to the device
- SSH: `ssh root@10.11.99.1` (USB ethernet; **key auth is set up**, no password needed).
- Creds/host in repo-local `.env` (gitignored). Device password also at *Settings → General → Help*.
- It is **BusyBox** — use `head -n N` (NOT `head -N`), `ps -ef`, etc.
- **Always verify on the real device** after a change (it's usually connected over USB).

## Non-negotiable constraints
- **No GPU/EGL/GLES** → WPE needs software GL (**Mesa llvmpipe, surfaceless EGL**). Page paint = Skia CPU.
- **rootfs `/` is full** → install ONLY under **`/home/root/rmweb`**; bundle missing libs, set rpath.
- **Cross-compile only** (no on-device compiler) via the official **ferrari Yocto SDK** (scarthgap, glibc 2.39, aarch64, `-mcpu=cortex-a53`).
- Display path (MVP) = **Qt6 + official `epaper` QPA** (`/usr/lib/plugins/platforms/libepaper.so`)
  with **xochitl stopped** (`systemctl stop xochitl`, restore on exit). It does the e-ink packing +
  waveforms for us. Direct `/dev/dri/card0` DRM is a *later* upgrade (panel packing is undocumented).

## Architecture (5 isolated modules)
`engine` (WPE→ARGB frames) · `display` (Qt6+epaper QPA) · `input` (evdev touch=event2/pen=event3) ·
`shell` (QML chrome) · `platform` (lifecycle: stop/restore xochitl, install, OTA hook).
Data flow: input → shell → engine renders → ARGB SHM → display (QImage→QtQuick sw scene→epaper)→e-ink.

## Reuse vs bundle
Reuse on-device (link dynamically): Qt 6.8.2, cairo, icu74, glib2.78, freetype, harfbuzz, openssl3,
libcurl, libxml2, libpng/jpeg, libdrm, libudev/systemd. Bundle (build): WPE WebKit, libwpe/WPEBackend,
Mesa(llvmpipe), libsoup3 (+sqlite3/libpsl/nghttp2), libwebp, libxkbcommon, libepoxy, gnutls/glib-networking.

## Working agreement
- Respond to the user in **Russian**.
- Per phase: implement → **verify on device** → **code-review subagent** → **simplify subagent**.
- Track work in the task list (phases 0→6). Use subagents for parallel/independent work.
- Local git now; publish to GitHub later. Commit trailer: `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.

## Build/deploy flow (Phase 0 ✅ verified 2026-06-24)
`./scripts/fetch-sdk.sh` → `docker build -f toolchain/Dockerfile -t rmweb-sdk .` →
cross-compile C/C++ via `./scripts/build.sh '<cmd>'` or CMake via `./scripts/cmake-build.sh <srcdir> <name>` →
deploy+run via `./scripts/deploy.sh <bin>` (plain) or `./scripts/run-on-device.sh <bin> [args]` (stops xochitl,
runs via epaper, restores xochitl). `hello` ran on the Paper Pro (aarch64, reMarkable Ferrari).

## Display path (Phase 1 🔶 in progress — see docs/research-reuse.md)
- Present via **Qt6 QtQuick (QML) ONLY** (NOT QtWidgets/QRasterWindow — those never reach the panel).
- Run with `QT_QPA_PLATFORM=epaper QT_QUICK_BACKEND=epaper`, **xochitl stopped**. Size the Window to
  `Screen.width/height` (official recipe — don't force geometry from C++ after creation).
- Scenegraph `libqsgepaper` auto-refreshes, BUT color (Gallery 3/ACeP2) content needs a **FULL refresh**
  (`EPFrameBuffer::setForceFull(true)`); partial/fast waveforms leave the screen white or show only a fragment.
- `docs/research-reuse.md` = the external-knowledge map (display, refresh strategy to reuse from
  netsurf-reMarkable + KOReader, WPE build reuse via Igalia meta-webkit, lifecycle/persistence, what NOT to reuse).

## Status
Phase 0 ✅ done. Phase 1 ✅ DONE & verified on device (2026-06-25): a standalone Qt6 QML app presents a full
test pattern on the Paper Pro e-ink via the epaper QPA (cure = Window sized to `Screen.width/height`; QtQuick
only; `QT_QPA_PLATFORM=epaper QT_QUICK_BACKEND=epaper`; xochitl stopped/restored). The real rMPP refresh API
is recorded for Phase 4 (`EPFramebuffer::swapBuffers/ghostControl`, exported by libqsgepaper — see research-reuse.md).
Phase 2 ✅ DONE (2026-06-25): **WPE WebKit 2.48.5 (Skia CPU, software) cross-built** for aarch64 + **Mesa softpipe**
(software EGL, surfaceless, no GPU); a headless `engine/wpe_render.c` **rendered a real web page to PNG**
(`build/wpe-render.png` — bar + colored boxes + anti-aliased text). Recipe: `scripts/build-wpe.sh {deps|build|render}`
→ `engine/*.incontainer.sh` (builds on a persistent, case-sensitive docker volume → resumable); all gotchas in
research-reuse.md §8 (sysroot pkg-config, glibc-2.39 loader repoint, `/usr` symlinks, `load_html`, fonts).
Phase 3 ✅ DONE (2026-06-25): a **web page rendered by WPE WebKit is shown on the Paper Pro e-ink**. `engine/wpeqt`
is a Qt6 app (= WPE UIProcess); `WpeEngine` drives WPE headless on a worker thread → `buffer-rendered` BGRA → `QImage`
→ `QQuickPaintedItem` → epaper QPA (xochitl stopped). Device bundle = `/home/root/rmweb` (`scripts/bundle.sh`);
build `scripts/build-wpeqt.sh`; run `scripts/run-wpeqt-on-device.sh {save|show}`. Engine also proven standalone
on-device (3a: `scripts/render-on-device.sh`). All integration gotchas in research-reuse.md §8 (QT_NO_KEYWORDS,
worker-thread GMainContext, BGRA==ARGB32, /usr/libexec overlay, BusyBox no-timeout).
Phase 4 (scope A) ✅ DONE (2026-06-30): **finger touch + scroll + reading shell work end-to-end on device.** Hard-won
facts, all verified on-device and written up in `docs/research/` (4 sourced docs):
- **Touch:** the epaper QPA posts finger touch with a NULL window → Qt drops it (and that path crashes WebKit).
  So we read the finger digitizer **directly from evdev** — node **event3 = "Elan touch input"** (event2 = pen;
  the old device-profile mapping was BACKWARDS), `EVIOCGRAB`'d (the grab also silences the QPA's crashing touch
  dispatch). `TouchReader` decodes Protocol-B (SLOT 47 / TRACKING_ID 57 (−1=lift) / POS_X 53 / POS_Y 54 / SYN),
  maps `x*1620/2064,y*2160/2832`, debounces 0.8 s, emits page-turn swipes. See `remarkable-touch-input.md`.
  **Phantom-touch guard (2026-07-01):** the epaper present induces capacitive noise on the Elan digitizer →
  `TouchReader` floods with phantom taps/swipes while rendering. Fix: `bumpTouchGuard()` called in `presentNext()`
  (issue) and `onFrameSwapped()` (completion); `emitGesture()` drops swipe/tap while `touchGuarded()`. Default
  tail = 350 ms (tunable: `RMWEB_TOUCH_GUARD_MS`). Lock-free (`std::atomic<gint64>`, both threads use monotonic).
- **Rendering (`wpe-rendering-protocol.md`):** (1) the headless view must be **mapped** or WebKit suspends
  painting — `set_visible(FALSE)→(TRUE)` after sizing the toplevel; verify `wpe_view_get_mapped()`. (2) NEVER call
  `wpe_view_buffer_released()` with an embedded WebKitWebView (double-free). (3) launcher sets
  `WEBKIT_SKIA_CPU_PAINTING_THREADS=0` + `WEBKIT_SKIA_ENABLE_CPU_RENDERING=1`. (4) read pixels via
  **`wpe_buffer_shm_get_data/_stride`** — `wpe_buffer_import_to_pixels()` returns a garbage size on scrolled frames.
- **Scroll:** a bare `scrollBy` changes scrollY but emits no buffer; a tiny DOM mutation forces an immediate
  repaint (`flip-latency≈23 ms`). Verified scrolled content renders correctly (saved frame PNG showed "Line 10–43").
- **JIT:** `JSC_useJIT=0`. **NOT W^X (re-tested 2026-06-27, earlier "W^X" claim was WRONG):** a direct probe
  (`build/jittest.c`) executes both `mmap(RWX)` and `RW→mprotect(RX)` machine code fine, so executable memory
  IS allowed. The JSC JIT is still broken here another way: full JIT (DFG/FTL) `abort()`s once hot code tiers
  up (`sig=6` on `rmweb-wpeqt`, a JSC RELEASE_ASSERT — not a segfault); baseline-only JIT
  (`JSC_useDFGJIT=0 JSC_useFTLJIT=0`) does NOT abort but renders BLANK and the page goes silent (JS
  miscompiles / state corrupts). So all JIT tiers are broken — likely a JSC codegen issue for this
  toolchain (cortex-a53 + `-mbranch-protection`/PAC, or pointer compression). Keep the interpreter; lighten
  heavy pages via content-blocking instead. Toggles: `RMWEB_JIT=1`, `RMWEB_JSC_OPTS="JSC_x=y ..."`.
  **Measured 2026-07-26:** rbc.ru on the interpreter — load finished @40s (network FINE, WPENetworkProcess
  idle), then WPEWebProcess pegged 93–98% CPU for 150s straight and the page NEVER hydrates past its SSR
  skeleton. Wikipedia-class (server-rendered, light JS) works great. Refined with `RMWEB_NOJS=1` (diag
  lever, disables JavaScript entirely — ours too): with JS OFF the load STILL wasn't done at 80s
  ("Loading 65%" — the page drags megabytes of assets through the USB link) and CPU stayed ~86% —
  so the cost is three-way: network volume, CSS/layout/paint pipeline, JS execution. **The working
  lever is `RMWEB_UA=mobile`:** rbc.ru with the iPhone UA serves SSR HEADLINES (readable news at ~80s,
  no hydration needed) instead of the desktop JS-app skeleton. Surfaced as a start-page Settings row
  "Sites: mobile (lighter)/desktop" (`rmweb:toggle-ua` — flips `settings.ua`, applies live via
  `webkit_settings_set_user_agent(..., nullptr|kMobileUA)`, persists). ua=mobile is ON in this device's
  profile. Desktop-mode heavy SPAs remain the platform ceiling, not a shell bug.
- **Device:** a process **segfault reboots the device** (watchdog/memfault, ~100 s) — logs go to `/home/root` to
  survive; a SIGSEGV backtrace handler is compiled in (`-rdynamic`).
**Phase 4 "~6 s per page-turn" SOLVED (2026-06-26):** the culprit was **Mesa softpipe** — the single-threaded,
no-SIMD reference rasterizer — spending ~6 s compositing the 1620×2160 TextureMapper layer on the WebProcess
compositor thread (NOT the panel / libqsgepaper, NOT WebKit's DisplayLink; ruled out by /proc CPU sampling +
a safe LD_PRELOAD SIGUSR2 backtrace into `swrast_dri.so`). Rebuilt Mesa 24.0.9 with **llvmpipe** (multi-core +
SIMD JIT) → frame render ~93 ms, page turns land on e-ink in **~120–250 ms** (verified, swipe + auto-page).
Bundle now ships `libLLVM-16.so.1` + deps (`engine/mesa-llvmpipe.incontainer.sh`); run with
`GALLIUM_DRIVER=llvmpipe`. Present = one grayscale frame per turn (sig-dedup drops idle/duplicate renders;
`RMWEB_FULL_EVERY=0` = no colour flash = least flicker). See the `six-second-render-softpipe` memory.
Phase 4 (scope A) shipped the reading shell: B2 chrome (hand-painted into the WPE frame + C++ hit-test),
reader mode (Mozilla Readability), on-screen URL keyboard, page/reader zoom, tap-to-follow-links, loading +
"couldn't render" indicators, mobile-UA-as-opt-in + readability CSS. Then code-review + simplify checkpoints.

Phase 5 ✅ DONE (verified on device 2026-07-01). **Phase 2 Engine Hardening (2026-07-09):** JIT stabilized (opt-in baseline JIT: `RMWEB_JIT=1` → `JSC_useBaselineJIT=1 JSC_usePollingTraps=1 JSC_useDFGJIT=0 JSC_useFTLJIT=0`, interpreter stays default; diagnostic `RMWEB_JSC_OPTS`); WebProcess crash recovery with exponential-backoff reload (`onWebProcessTerminated`, main.cpp:767); richer `crashHandler` logging PID/TID; touch-guard default raised to 450 ms (`touchGuardTailUs`, `RMWEB_TOUCH_GUARD_MS`); `seq_cst` atomics; present/buffer null-checks; perf env (`RMWEB_SKIA_THREADS`, `WEBKIT_FORCE_VBLANK_TIMER=1`). Known doc bug: `Q_LOGGING_CATEGORY(lcEngine)` is declared (main.cpp:62) but **never used** — all logging is plain `qInfo`/`qWarning`; its comment ("filter with WEBKIT_DEBUG=rmweb.engine") is wrong — Qt logging categories are filtered via `QT_LOGGING_RULES` (e.g. `QT_LOGGING_RULES=rmweb.engine.debug=true`).

**Phase 6 Batches 3/4 — claims NOT confirmed by code (corrected 2026-07-19).** These entries claimed dark mode (`RMWEB_READER_THEME`), typography presets (serif/sans, line-height/width), smooth auto-scroll with tap-to-pause, article export, night mode, focus mode (chrome auto-hide), style presets (News/Book/Academic/Minimal), a reading-progress bar with time estimate, per-URL scroll-position restore, and Phase 7 hooks (tab stubs, form/login detection signals, download hooks). Grep over `engine/wpeqt/` shows **none of these exist**: the reader stylesheet is a single hardcoded light theme (`kReaderCss`, main.cpp:173 — it does include table/code/img rules); the only reader tuning knob is `RMWEB_READER_FONT` (font size, main.cpp:222); `RMWEB_AUTOPAGE_MS` is a *diagnostic* auto-page driver (main.cpp:1712), not a user-facing auto-scroll; `RMWEB_READER_DIR` is the directory the vendored Readability.js is *loaded from* (main.cpp:164), not an export target. The only progress UI is the "Loading NN%" badge driven by WebKit's estimated-load-progress (main.cpp:1098). The original claims were erroneous; see `docs/review-2026-07-18.md` HIGH#1.

**Phase 7 Batch 1 (partial, 2026-07-25) — VERIFIED ON-DEVICE.** Implemented:
persistent cookies (sqlite via `webkit_network_session_get_cookie_manager` — the 2022 API moved cookie
management off WebKitWebContext; `RMWEB_COOKIES=0` opts out, policy = no-third-party), per-URL scroll
restore (`scroll.txt`, capture from the pageBy JS `sy=` answer in onJsDone, restore at LOAD_FINISHED+800 ms
suppressed by a user page-turn; `m_curUrl` now clears at LOAD_COMMITTED so a scroll completion in the
commit→finish window isn't mis-attributed), in-page find (address bar `/text` + Go, FindController with
case-insensitive wrap-around, repeat same term = next match; toast "Match found"/"No matches" — the
found-text matchCount is G_MAXUINT unless COUNT_MATCHES is passed, don't print it raw),
downloads (decide-policy RESPONSE → unsupported MIME → `webkit_policy_decision_download`; destination via
`WebKitDownload::decide-destination` (basename-sanitized) to `/home/root/Downloads`, `RMWEB_DOWNLOADS`
override; **2022 API: `webkit_download_set_destination` takes an absolute PATH, not a file:// URI**),
tabs-lite (every visited page = an MRU tab, cap 8, `tabs.txt`; the start page shows "Open tabs" with
per-tab × via `rmweb:close-tab:<url>` — urlDecode'd, decide-policy guard extended to a command parser;
**use U+00D7 × — the device font lacks U+2715**), reader dark theme (`readerDark` in settings, toggled
from the start page Settings line, `kReaderCssDark` applies on next Reader activation), toast overlay in
the chrome (`WpeView::setNotice`, 4 s), `urlDecode` in url.h. DIAG: `RMWEB_DEBUG_FIND=term`; the dev runner
now also forwards `QT_LOGGING_RULES`. Save-debounce generalized to 4 stores (history/settings/scroll/tabs).
**SIGTERM→`_Exit(0)`** (termHandler): the runner's timed kill used to land in the crash-prone WebKit
teardown and the watchdog REBOOTED the device ~1–2 min later (3 reboots this session); with the handler
reboots are much rarer but NOT zero — the 2026-07-26 verification session still saw the device reboot
after ~30% of timed kills (xochitl stopped, heavy WebKit state). If ssh drops mid-session: wait
~40–60 s, the device comes back; grabs in /home/root/rmweb survive the reboot. On-device verified: cookies.sqlite written, `[scroll] restore y=1191` (+ visual),
find highlights + toast, `Saved testfile.zip` in ~/Downloads, dark reader grab, Open tabs × glyphs.
Still NOT implemented: JS console, user scripts (password manager, autofill, history search, TLS
indicator and the progress bar landed in the later batches below). Known quirk: the restore JS targets the same inner-scroller detection as pageBy (Wikipedia
scrolls an inner div, not the document).

**Phase 7 Batch 2, form filling (2026-07-25) — VERIFIED ON-DEVICE.** One tap probe classifies everything
tappable (engine/wpeqt/fieldprobe.h: `TapHit` None/Link/Tick/Field + `parseTapProbe` line protocol +
`jsStringEscape`; tests/fieldprobe_test.cpp). Priority: text field (focus + `window.__rmwebField` stash +
`fieldFocused(value, masked)` → keyboard opens ON the field's current value, password masked `*` in the
address bar) > select (cycles options in-page, toasts the new label) > checkbox/radio (`c.click()`, toast
on/off) > link/button; a `<label>` resolves via `label.control` (a label wrapping several radios resolves
to the FIRST one — spec behaviour). Go in field mode commits via `setFieldText` (main.cpp:578): focus →
`select()`+`execCommand('insertText')` (editing pipeline, React sees real beforeinput/input) with a
NATIVE-setter fallback + manual `input` event, then `change`, `blur()`, and a hidden-marker bump to force
one composite (same trick as pageBy — bare DOM edits commit no buffer here). Gotchas learned the hard way:
**never pass JS containing `%` (e.g. the `%` modulo) through `g_strdup_printf`** — it eats a vararg and
segfaults in vasprintf (double it: `%%`); a late programmatic value-set does NOT invalidate a PASSWORD
control on this port (value updates, pixels stay stale through setter/execCommand/blur/display-nudge/full
`<html>` repaint alike) — the only working repaint is to CLONE the input with the new text as its `value`
ATTRIBUTE (`replaceWith`, re-stash) plus a transient full-viewport ~invisible veil (`rgba(0,0,0,0.01)`,
removed after 800 ms) to force real damage; the frame `sig` hash SAMPLES pixels — it can call a bulleted
password frame a "dup" of the empty one, so trust grabs over sigs. WpeView: `beginFieldEdit(value,masked)`,
`fieldTextEntered` (raw buffer, no trim; empty Go clears the field), bar+keyboard paint while editing even
with chrome hidden. DIAG: `RMWEB_DEBUG_PROBE="x,y"` (probe at 4 s), `RMWEB_DEBUG_FORM="x,y,text"` (+commit
at 7 s, `logFieldState` read-back at 8.5 s — tag/type/len/conn/attr, password-safe), both forwarded by the
runner. Verified against a local form page: text "John Doe" rendered, password bullets rendered (len=6),
textarea commit, checkbox toggle + toast "on", radio toggle via label, select one→two + toast "two".

**UI+browser batch (2026-07-25, commits 7c30fdf/4b51426) — FULLY VERIFIED on-device 2026-07-26**
(start, error-page+Retry, TLS padlock, address-bar search, autofill learn→prefill, password
learn→prefill, reading-progress bar; grabs in build/verify/).
Start page redesign (startpage.h): hero wordmark "rmweb" + tagline, letter-spaced section labels, UTF-8-safe
letter avatars (`utf8First` — кириллица ок), bookmark tiles, grey host sublabels. Press feedback: chrome
buttons invert for 180 ms (`pressChrome`/`chromeHitRect`/`m_pressed`), pressed key inverts (`m_kbPressed`,
reset in `m_kbFlush`). Error page: `load-failed` → `buildErrorPage` via `webkit_web_view_load_alternate_html`
(Retry = failed URL; **this SDK has no `WebKitLoadError` — cancelled loads are `WEBKIT_NETWORK_ERROR_CANCELLED`**).
TLS padlock in the address bar: `WebKitWebView::tlsStateChanged(int)` 0/1/2 → `WpeView::setTlsState`,
`iconLock` (filled body = https, open shackle = cert errors). Address-bar search: `looksLikeUrl` (url.h);
non-URL input → `engine.searchAndShow` → `buildSearchResults` (startpage.h) = DDG link
`https://html.duckduckgo.com/html/?q=` + local matches via `searchStore` (profile.h — one template
for Bookmark/HistoryEntry).
Long-press link peek: stationary hold >700 ms = `Gesture::LongPress` (TouchReader) → `engine.peekLink` →
probe with `peek\n<href>` (no navigation) → toast with the URL (≤72 chars + "…"); `TapHit::Peek` in
fieldprobe.h; `m_lastProbePeek` suppresses linkMissed. Address-bar hint now reads "URL or search — /text
finds in page". DIAG: `RMWEB_DEBUG_SEARCH=words` (forwarded by the runner). **Autofill (1da6284):**
learn-as-you-type — the probe's `field` answer gained a hint line (`autocomplete name id placeholder`,
whitespace-folded; `field\n<mask>\n<hint>\n<value>`, legacy no-hint answers still parse);
`classifyFieldHint` (fieldprobe.h) maps it to Email/User/Name ("user"/"login" checked BEFORE "name" —
"username" contains both; masked ⇒ None, passwords never learn). A committed non-empty value lands in
settings (`afEmail`/`afUser`/`afName`, debounced `m_settingsSaveSrc`); the next EMPTY field of that kind
opens the keyboard pre-filled (`fieldFocused(value,masked,suggest)` → `beginFieldEdit` + toast
"Autofill — edit or press Go"); learning happens in `learnFieldText` off the `fieldTextEntered` wire
(`m_pendingFieldKind` stashed at emit time). **Password store (36ba1de):** per-host logins in
`passwords.txt` (`host\tuser\tobf`; obf = XOR+hex — OBFUSCATION, not encryption, profile dir is 0700);
`hostFromUrl` (url.h) keys it. A password commit's JS answers `pw\n<sibling-login>` (first text-ish
input of `f.form||document` — the password CLONE keeps `.form` since `replaceWith` stays in place);
`onFieldSet` (now `data=this`; logFieldState still passes nullptr and returns early) upserts via
`m_lastCommitText` (plaintext of the in-flight commit, cleared right after). Prefill: an EMPTY
password field on a known host opens the keyboard with the stored password; a User-kind field falls
back to the stored login. Debounced save = 5th store (`what==4`, `m_pwSaveSrc`). **Reading-progress
bar (37c6eff):** the pageBy and scroll-restore JS answer `sm=<max scroll of the USED scroller>`;
onJsDone maps `sy/sm` to a 0..1 fraction (`sm<=40` ⇒ -1 = hide) and emits `readProgressChanged`;
WpeView paints a 6 px white track + black fill + 1 px separator along the VERY bottom edge — even
with chrome hidden (reader fullscreen), skipped while editing; `urlChanged` resets it to hidden.
Review (6376daa) + simplify (baecf4b, b59b1e8 — chrome layout/icon dedup done) checkpoints done
(review findings fixed: no self-present in setReadProgress, progress gated on m_curUrl, hint includes
f.type, long-press hit-tests chrome, looksLikeUrl accepts localhost/host:port). **Batch verification:**
`scripts/verify-on-device.sh` — one command runs start/error-page/search/TLS grabs, spins a local
form+long page on 10.11.99.5:8765 (steps skipped when the USB link IP is absent) for autofill
learn→prefill and password learn→prefill, auto-page progress-bar grab; pulls
build/verify/<step>.{png,log} with log-grep hints. Verification fixes (c2d6c90): `onUri` ignores
`about:blank` (load_html search page must not clobber the typed query in the bar; the
RMWEB_DEBUG_SEARCH diag path now mirrors urlEntered: setAddr(term) + hide progress);
RMWEB_DEBUG_FORM feeds the learner (`engine.learnFieldText` after `setFieldText`). Slow-site false
positive fixed (2026-07-26): the blank-check timer (`scheduleRenderCheck`, 13 s from LOAD_STARTED) used
to judge the page while a slow load was STILL in progress (rbc.ru: load finished @34s, verdict @31s →
bogus "Couldn't display the page"); it now RE-ARMS itself while `m_loadInProgress` (set at
LOAD_STARTED, cleared at LOAD_FINISHED / non-cancelled load-failed) and only judges a finished load —
verified on rbc.ru (`[render] nonWhite=912 blank=0` after finish, no notice over the site skeleton).
The notice itself is now a PAGE state, not an overlay: `paint()` whites out the stale frame when
`m_renderFailed` (before: the box floated over the previous site → "notice + half-rendered page" mess).
Regression step in verify-on-device.sh [8/9]: blank.html must yield the clean white notice. **Verify gotchas:**
probe coords = css px * dpr * **zoom** — the script pins `zoom=1.0` in the device profile first (a
leftover zoom silently shifts every hit target); the auto-pager ALTERNATES direction (down @4s,
up @8s) so grab @6s, not ≥8s; a grab can come out pure black mid-refresh (transient — just retake);
qCDebug(lcEngine) lines ([t] pageBy / page JS done sy=) are compiled OUT of the device log — autopage
proof is the grab, not the log. Start-page CSS sharing stayed deferred (touch geometry + visuals,
must be eyeballed).

The production env is DRY
in `device/rmweb-env.sh` (sourced by both the launcher and the dev runner `scripts/run-wpeqt-on-device.sh`);
`scripts/bundle.sh` ships launcher/env/installer/VERSION/icon; user docs in `docs/install.md`. Layer B
(home-screen icon via XOVI + rm-appload: `device/appload/rmweb.draft` + `device/icon.svg`) auto-registers via
`install.sh` when rm-appload is present, else degrades gracefully (rm-appload not installed on this device).
**Phase 7 Batch 2 — NOT IMPLEMENTED (corrected 2026-07-19).** This entry claimed a password manager (XOR+base64 storage in profile.h), context-aware autofill, an on-device JS console, user/content scripts, full history search with filters, plus "final polish" (gesture tuning, error pages, performance dashboard) and a "v0.8.0 release-ready, No TODOs, all features verified" state. **None of these features exist**: `engine/wpeqt/profile.h` stores only bookmarks/history/settings, and `WebKitUserContentManager` is used solely for the content-blocking filter and one built-in site stylesheet (`kSiteCss`, main.cpp:275-281) — no user scripts. Form filling beyond that audit's "stock WebKit behaviour" has since been implemented
properly (see the Phase 7 Batch 2 form-filling entry above, 2026-07-25); the rest of the Batch 2 claims
stay erroneous, and the project is a **beta, not release-ready**. Full audit: `docs/review-2026-07-18.md` (HIGH#1).
