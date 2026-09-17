# Actual WPE authentication regressions

Run against a completed, private cache from `scripts/build-auth-browser.py`.
The cache must contain the isolated native-WebAuthn engine and helper inventory;
the old `build-app-only.py --auth-only` cache is incompatible:

```sh
bash tests/auth-wpe-smoke/run.sh /absolute/path/to/auth-build-cache
# Optional anonymous live check of the fixed public device sign-in page:
bash tests/auth-wpe-smoke/run.sh /absolute/path/to/auth-build-cache --public
```

The default lane runs with Docker networking disabled. It retains the synthetic
native-keyboard form test and exercises the actual `AuthEngine` callbacks by
including the production translation unit through a narrow friend test seam.
Known inline pages test blank child frames, rejected srcdoc child frames, rejected
top-level blank/data/blob/file navigation, blocked pop-ups, cancellation, and preservation
of an existing failure. Only fixture bootstrap temporarily bypasses navigation
policy; the tested actions use the production policy. No production callback is
copied or replaced.

Input cases pass framebuffer coordinates through the production `AuthSurface`
and `AuthEngine`: finger ID zero and pen taps activate a known link, a finger tap
focuses an empty field, dragging scrolls without a click, and cancellation does
not activate a link. A native mouse control distinguishes gesture issues. Only
synthetic event counts and booleans are inspected; field values are never read.
These checks do not exercise the tablet's physical AppLoad/QTFB input delivery.

The frame-pacing fixture also runs the production snapshot pump and headless
renderer with synthetic static and animated pages. It requires prompt tap
pickup, bounded frame generation during rapid native input, and return to
passive rendering and snapshot rates. It observes only a known marker pixel in
memory and emits counts and monotonic timings; it never exports a frame. These
are internal delivery measurements, not physical e-ink response times.

The same lane runs the native WebAuthn provider fixture and the joined
`AuthEngine`/Qt helper fixture. They verify trusted origin/RP binding, original
client-data hashing, response bounds, cancellation, and discarded late results.
The joined fixture uses a test-only child process that returns synthetic bytes;
no phone, Bluetooth adapter or genuine credential is involved. See
`../auth-webauthn-provider/README.md` and `../auth-passkey-browser/README.md`.
Two additional iframe cases use real HTTPS on container loopback with a
disposable CA trusted only by the test subprocess. They cover a same-origin
child assertion and cancellation when that child is removed. No production TLS
policy or machine trust store changes.

The optional live lane visits only `https://auth.openai.com/codex/device` in a fresh
ephemeral session. It supplies no code, credentials, or interaction. The container's
normal public CA bundle is selected explicitly because the SDK OpenSSL default
path differs from the container's; TLS errors remain fatal. This proves page
loading, not authorization or physical keyboard/touch behavior.

Navigation tests silence WebKit/GLib stdout and stderr before starting the engine;
a close-on-exec descriptor emits only fixed case names, PASS/FAIL, and numeric
failure counters. They never
emit or persist page content, complete navigation URLs, form values, error text,
cookies, or screenshots. Processes and container runs have bounded deadlines.
