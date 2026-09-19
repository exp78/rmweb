# Reusable AppLoad authentication window

`rmweb-auth-browser` is a temporary WebKit window for caller-supplied HTTPS
authentication pages, including phone-passkey assertions. It uses an isolated
patched WPE runtime and the existing AppLoad QTFB transport. It has no article
reader, history, saved passwords, page capture, or persistent browser profile.
The caller owns authorization state, its callback listener, token exchange,
credentials, and the decision that authentication has completed. The browser
does not require a particular provider, catalog ID, or install path.

The supported device profile remains **Paper Pro (Ferrari), firmware
3.28.0.172 / Qt 6.10.3**. The phone helper requires that model's built-in
`btnxpuart` Bluetooth adapter. Generic URLs and movable packaging do not
establish RM2 or other hardware support.

## Launch and callback contract

The launcher accepts an initial URL and an optional display code:

```text
entry HTTPS_URL
entry HTTPS_URL --device-code ABCD-1234
```

The initial URL is limited to 8,192 bytes and must be valid HTTPS with a host,
without credentials, a fragment, or literal whitespace/control bytes. The
provider, path, query, and HTTPS port are supplied by the caller. A display code
may contain up to 64 uppercase letters, digits, or hyphens; it is displayed in
the window, never inserted into page fields.

If the initial query contains `state` or `redirect_uri`, both must occur exactly
once. State must contain 16–256 ASCII letters, digits, hyphens, or underscores.
The decoded redirect must be a canonical literal HTTP loopback URL using
`localhost`, `127.0.0.1`, or `[::1]`, an explicit port from 1024 through 65535,
and a nonempty absolute path. Credentials, query, fragment, residual percent
escapes, and paths requiring dot-segment normalization are rejected. The outer
`redirect_uri` value may be percent encoded. Display codes and callback flows
are separate launch modes.

For example, a caller may issue:

```text
https://login.example.test:8443/authorize?state=abcdefghijklmnop1234567890&redirect_uri=http%3A%2F%2F127.0.0.1%3A49152%2Foauth%2Freturn
```

The browser permits an HTTP callback only at that exact address, port, and path,
with one matching state and either one nonempty `code` or one nonempty `error`.
Without a callback pair, HTTP navigation is blocked. HTTPS redirects remain
available for provider login flows. A callback receipt, including an error
callback, means only that the caller's callback page was reached. It is not proof
of successful authentication, token exchange, or enrollment.

## Build and install layout

Use the pinned SDK recipe in `toolchain/Dockerfile.app-only-sdk-3.28` and separate
external caches. Obtain the official Ferrari OS 5.8.203 SDK installer named
`remarkable-production-image-5.8.203-ferrari-public-aarch64-toolchain.sh` from
[reMarkable's developer downloads](https://developer.remarkable.com/links).
Keep it outside Git; the Dockerfile verifies its pinned size and SHA-256.
Build the ARM64-host SDK image:

```sh
docker buildx build --platform linux/arm64 --load \
  -f toolchain/Dockerfile.app-only-sdk-3.28 \
  --build-context sdk_source=/absolute/path/to/sdk-download-directory \
  -t rmweb-app-sdk:3.28.0.172 .
```

Build/export the helper using
[its rebuild instructions](../engine/auth-passkey-helper/README.md), then build
the engine and browser:

```sh
engine_cache="$HOME/.cache/rmweb/auth-engine-3.28"
auth_cache="$HOME/.cache/rmweb/auth-browser-3.28"
python3 scripts/build-auth-engine.py --cache "$engine_cache" \
  --sdk-image rmweb-app-sdk:3.28.0.172 --jobs 6
python3 scripts/build-auth-browser.py --cache "$auth_cache" \
  --engine-artifacts "$engine_cache/artifacts" \
  --helper-artifacts /absolute/path/to/helper/artifacts
```

The app recipe builds `rmweb-auth-browser` and `rmweb-auth-entry`, then adds the
verified `rmweb-auth-passkey` helper. Manifest version 2 binds source, SDK,
runtime, helper, and executable hashes. The artifact layout remains the three
ELF executables, `runtime/`, `licenses/`, and `manifest.json`; dependency sources,
licenses, and rebuild materials accompany the runtime and helper. Use a dedicated authentication cache; the regular browser runtime does not
contain this native WebAuthn provider.

The caller assembles an application root from those verified artifacts and
`device/auth/entry` from the matching source checkout. The browser manifest's
source inventory records the launcher hash. A generic example layout is:

```text
/home/root/example-auth/
  entry                         # device/auth/entry
  bin/rmweb-auth-entry           # verified native namespace entry
  bin/rmweb-auth-browser         # verified authentication browser
  bin/rmweb-auth-passkey         # verified phone helper
  runtime/rmweb-env.sh
  runtime/lib/...
  runtime/libexec/...
  runtime/licenses/...
  licenses/...
```

The wrapper derives its root from its own location and exports the absolute
`RMWEB_AUTH_RUNTIME` before sourcing that root's runtime environment. It does
not accept a runtime or helper override. The native entry locates the browser
and runtime from its executable location; the browser locates the phone helper
beside its executable. Install real, root-owned files and directories with no
symlinks or group/world-write permission. Preserve private application permissions.

The native entry verifies owned paths and executables, creates a private mount
namespace, and mounts its bundled `runtime/libexec` read-only over
`/usr/libexec`. It preserves the process PID when executing the browser. Stock
interface mounts and other AppLoad applications remain outside that namespace.
Use a matching launcher, native entry, browser, helper, and runtime export;
older environment scripts still contain a fixed consumer path.

The caller owns its catalog directory/ID, display name, install location, and
installation/rollback lifecycle. For example, a caller-owned catalog's
`external.manifest.json` can contain:

```json
{
  "name": "Example sign-in",
  "application": "/home/root/example-auth/entry",
  "workingDirectory": "/home/root/example-auth",
  "args": ["https://login.example.test/sign-in"],
  "environment": {},
  "qtfb": true,
  "supportsVirtualKeyboard": true,
  "supportsRotation": false,
  "disablesWindowedMode": true,
  "aspectRatio": "auto"
}
```

This is a layout example, not an installer. Supply one-time codes and private
callback state through transient launch arguments, not a persistent catalog.

## Dependency and upgrade boundaries

The separate AppLoad installation must include the reviewed QTFB lifetime,
key-log removal and touch-cancellation patches. Follow the pinned source and
apply order in [AppLoad prerequisites](../patches/appload.md), then qualify its
stock-UI hooks for the exact firmware. Rebuilding rmweb alone does not update
AppLoad. No boot configuration or stock-UI hook is installed by these tools.

The helper serializes Bluetooth ownership through `/run/rmweb-passkey.lock`
and uses finite `rmweb-passkey-…` wake locks. Stop new launches and let every
active helper finish restoration and exit before changing versions. Never unlink
a lock while a helper may hold it. The empty lock file may remain until reboot.

Use a fresh, dedicated engine build volume. Its purpose marker is
`rmweb-auth-webauthn-engine-v1`; unrelated or mismatched volumes are rejected
without mutation. Use the matching launcher, runtime environment, native entry,
browser and helper from the same reviewed source/package receipts.

## Keyboard, privacy, and lifetime

Enable `supportsVirtualKeyboard` in the caller's AppLoad manifest. Swipe down
briefly with a finger from the top-center edge to expose AppLoad's toolbar,
then tap its far-left keyboard button within three seconds. Long swipes ending
below 400 framebuffer pixels do not trigger this AppLoad v0.5.3 gesture.
The browser translates the pinned layout's QTFB key packets into native WPE
events, including modifiers and releases. It does not inspect or replace DOM
field values. No custom keyboard is included.

Unmodified AppLoad v0.5.3 logs key labels. Apply the supplied
[key-log removal patch](../patches/appload-v0.5.3-no-key-logging.patch) and load
the rebuilt AppLoad library before entering credentials.
The launcher clears inherited preload, inspector, key-log, and diagnostic
overrides, sets a private umask, and disables core dumps. Page/engine stdout and
stderr are suppressed and the WebKit network session is ephemeral. Native
passkey diagnostics use fixed `AUTHPRIV` syslog labels; HTTP diagnostics contain
only failure status numbers (400–599), capped at 16 per browser. URLs, page text,
request options, QR contents, and assertions are never included. Return closes
this window; the caller retains its own lifecycle.

The patched provider presents a native QR for ordinary same-origin HTTPS phone
assertions, with the verified relying-party ID, progress, and Cancel. The phone
retains its passkeys. Registration, conditional/silent mediation, platform
authenticators, and unsupported extensions fail explicitly. WebKit's trusted
origin/RP checks, exact client-data hash, assertion binding, and cancellation on
navigation, abort, or timeout remain in force. See `patches/README.md` for the
trust boundary. An unpatched runtime without the WebAuthn APIs cannot perform
this flow; provider-offered alternate sign-in methods are a separate option.

## Navigation and input

Exact `about:blank` and `about:srcdoc` child documents are permitted because
ordinary authentication pages use them. Either URL committing in the main frame
is stopped and rejected. Neither replaces the visible origin nor becomes a
valid launch URL. Query/fragment variants and other local schemes remain
blocked. The native provider separately rejects passkey requests from inherited
blank/srcdoc frames. Pop-ups, downloads, unsupported schemes, and TLS exceptions
remain blocked. Ordinary stopped or superseded loads do not create a failure
card; ignoring cancellation never clears an existing failure.

Both AppLoad finger and pen packets use the existing native page-input path.
Mixed contacts cancel the gesture; loading, stale-frame, and navigation guards
apply to both. WPE supplies tap-to-click and drag-to-scroll without an extra
synthetic click. Pen contact IDs map to WPE ID zero, with finger IDs shifted by
one to keep the sources distinct and avoid reserved IDs.

## Validation and remaining gates

```sh
./scripts/run-tests.sh
python3 -m unittest discover -s tests -p auth_launcher_test.py
python3 -m unittest discover -s tests -p auth_build_recipe_test.py
python3 -m unittest discover -s tests -p test_auth_engine_build.py
cmake -S tests/auth-policy -B build/auth-policy -G Ninja
cmake --build build/auth-policy
ctest --test-dir build/auth-policy --output-on-failure
bash tests/auth-wpe-smoke/run.sh "$auth_cache"
# Optional anonymous check; no code, credentials, or sign-in interaction:
bash tests/auth-wpe-smoke/run.sh "$auth_cache" --public
```

The launcher tests execute the real script from a relocated temporary directory
with stub runtime/native-entry fixtures. They check literal arguments, derived
runtime paths, cleared overrides, private umask/core limits, same-PID execution,
and fixed failure messages. They do not exercise the tablet's namespace or
WebKit. `tests/auth_entry_test.py` exercises the actual native namespace entry
inside a disposable root-owned Docker container and refuses to run on the host.

The host suites cover auth policy, surface, helper protocol, and Linux QTFB.
Actual-engine fixtures cover native input/navigation, the WebAuthn provider,
and the Qt/GLib bridge with a fake helper. Synthetic assertions test binding and
cancellation, not phone cryptography. The separate
`tools/passkey-acceptance/README.md` describes a disposable relying-party test;
only a verified assertion from its live verifier establishes that test's phone
passkey result.

On 2026-09-17, host policy, surface, launcher, helper and packaging suites
passed, along with the actual SDK-built entry's 29 namespace/ownership cases
and the acceptance app's four actual-WPE route fixtures. The separate
self-contained acceptance app was built with the official Paper Pro 3.28 SDK,
its package hashes and private helper namespace were checked on-device, and a
user confirmed its **PASS** result after phone QR approval on software
**3.28.0.172 / Qt 6.10.3**. The tested runtime used the already built patched WPE
libraries; this was not a fresh full-engine rebuild.

That physical result establishes the disposable relying party's assertion
verification. It does not establish arbitrary provider compatibility, account
sign-in, caller enrollment, RM2 support, or another firmware profile. Cancellation,
Bluetooth restoration, stock-UI return and input should be rechecked for each
new installation. A source/build check alone is not a device qualification.
