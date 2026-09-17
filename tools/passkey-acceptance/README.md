# Disposable passkey acceptance test

This local test lets a normal phone browser **create a disposable passkey**, then
lets the tablet's real WPE → native provider → Rust helper → phone path request
an assertion. The server verifies its signature, challenge, exact origin, RP ID,
credential ID, user presence and user verification. It does not sign in to
any account. The production browser supports `get` only;
registration happens exclusively in the phone's ordinary browser.

## Quick guided demo

For the temporary HTTPS runner and guided phone/tablet screens, see
[DEMO.md](DEMO.md). This explicit demo mode uses a code-gated Cloudflare Quick
Tunnel; the direct-LAN setup below remains available without a proxy. The demo
requires phone registration followed by an explicit tablet verification tap.

## Local checks

Requires Node 22 or later and npm. The sole direct test dependency is pinned to
`@simplewebauthn/server` **14.0.2**, with its transitive integrity lockfile.
The package metadata requires Node >=20; we use the official documentation's
Node LTS22+ recommendation. No browser polyfill or virtual authenticator is used.

```sh
cd tools/passkey-acceptance
npm ci --ignore-scripts --no-audit --no-fund
npm test
npm run check
python3 native/test-preparation.py -v
shellcheck native/build.sh native/test-engine.sh native/launch
npm start
```

The default server binds **127.0.0.1:8089**, with origin
`http://localhost:8089`. Open `/register` or `/verify` locally and leave the test
access code blank. This default is for local development, not phone/tablet
acceptance. Changing the bind address requires HTTPS and an explicit access code.

The tests generate ephemeral P-256 private keys in memory and use real signed
assertions with the standard verifier. They cover successful registration and
assertion, wrong signature/origin/challenge/RP/credential/UP/UV/userHandle/type,
cross-origin contexts, replay, counter reuse, expiry, malformed and oversized
requests, HTTP session isolation and the early-tablet retry UI. These tests prove
server checks, **not phone or hardware authentication**.

## Prepare a trusted physical test origin

Use a dedicated DNS hostname you control, on a private test LAN, with a valid
HTTPS certificate trusted by both the phone and the tablet's existing runtime.
The phone and tablet must resolve the same hostname to this server. Do not use a
real account's RP. Do not use `login.example.com` from the automated fixture.
This direct-LAN mode provisions no domain, certificate, tunnel or deployment.
The optional guided demo runner provisions only a temporary Quick Tunnel.

Set `TEST_ORIGIN` to the exact HTTPS origin, with no path, trailing slash, query,
fragment, credentials, IP literal, or explicit default `:443`.
Non-default ports are supported. `TEST_TLS_CERT` must contain the leaf and any
required intermediate chain; `TEST_TLS_KEY` is its private key. Keep certificate
keys outside the repository. `TEST_ACCESS_CODE` must be 32–128 characters from
`A-Z a-z 0-9 _ -`; use a random disposable value, entered without shell-history
or log capture. For example, in Bash:

```sh
export TEST_ORIGIN=https://passkey-test.your-domain.example
export TEST_BIND=192.168.1.50
export TEST_TLS_CERT=/private/path/fullchain.pem
export TEST_TLS_KEY=/private/path/server.key
IFS= read -r -s -p 'Disposable access code: ' TEST_ACCESS_CODE
export TEST_ACCESS_CODE
printf '\n'
npm start
```

Substitute a real owned hostname and local address. There is no TLS-error bypass,
no trusted-forwarded-header mode, and no reverse-proxy setup in this direct-LAN
mode. For temporary public HTTPS, use only the bounded guided demo runner.
Changing device trust stores or network configuration is a separate operator action, not part of this harness.

The server retains one public credential, at most four session cookies and one
single-use 120-second challenge per session. A run lasts 30 minutes. Registration
cannot replace the credential; restart the server for a new run. Bodies are
limited to 64 KiB, with eight connections/in-flight requests, bounded HTTP
headers/timeouts, and 64 join attempts. Cookies are HttpOnly, SameSite=Strict,
and Secure with HTTPS. API requests require the exact Host and Origin; no CORS.
No private passkey exists on the server. No ceremony, credential, QR, assertion,
access-code or raw verifier-error values are logged or written to disk. Browser
requests necessarily transmit the proof to the verifier over HTTPS; the page
displays fixed status text only. The tablet displays PASS only after the verifier
accepts that tablet's own assertion response; another session's success cannot produce PASS.

## Build the separate tablet test application

This target includes the existing production `AuthEngine` through its test friend
seam and links the real `AuthSurface`, `QtfbClient`, `AuthPasskey` and WPE runtime.
Its own package supplies the runtime and the default sibling
`bin/rmweb-auth-passkey` helper.
There is no fake helper, alternate HTML, form inspection, TLS exception, callback
interception or production-policy change. After an inert blank startup finishes,
its stricter policy allows navigation only to the baked test origin's `/verify`.
Other origins, routes, local schemes, downloads and pop-ups are refused. The test
page's CSP limits its scripts, styles and API calls to itself. The normal AppLoad
keyboard, touch/pen input, passkey QR/cancel panel and Return control are retained.
The process has no history/persistent profile, suppresses browser diagnostics,
and exits after 30 minutes.

Use verified engine artifacts from `scripts/build-auth-engine.py` and helper
artifacts exported by `engine/auth-passkey-helper/package.py`. The engine's
`rmweb-env.sh` must match the current source and accept `RMWEB_AUTH_RUNTIME`;
an older runtime with a fixed installation path must be rebuilt. The helper
manifest, executable, pinned dependency, current helper sources and license
inventory must also match the current checkout.

```sh
bash tools/passkey-acceptance/native/build.sh \
  https://passkey-test.your-domain.example \
  /absolute/private/engine-artifacts \
  /absolute/private/helper-artifacts \
  /absolute/private/FRESH-acceptance-build
```

The output directory must not exist. The builder uses the production artifact
validator to check the engine and helper, builds this test application and its
fixtures in the engine's pinned official 3.28 SDK image without network access,
and copies the verified runtime, helper and complete license/source materials
into the test package. It checks input identity again after the build, checks
the copied files, and records source and recursive artifact hashes in
`receipt.json`. It does not alter the production cache, runtime, browser or device.
Rebuild for the actual trusted hostname; a fixture build is not a live catalog.

The local output is prepared for this **separate** layout:

```text
application/bin/rmweb-auth-browser  → /home/root/rmweb-passkey-acceptance/bin/rmweb-auth-browser
application/bin/rmweb-auth-entry    → /home/root/rmweb-passkey-acceptance/bin/rmweb-auth-entry
application/bin/rmweb-auth-passkey  → /home/root/rmweb-passkey-acceptance/bin/rmweb-auth-passkey
application/runtime/              → /home/root/rmweb-passkey-acceptance/runtime/
application/licenses/             → /home/root/rmweb-passkey-acceptance/licenses/
application/launch                → /home/root/rmweb-passkey-acceptance/launch
catalog/                          → a new, uniquely named temporary AppLoad catalog directory
```

The entry compiles `engine/wpeqt/auth-entry.cpp` unchanged. It resolves its
application root from its executable and mounts that package's
`runtime/libexec` read-only in its private mount namespace while preserving
AppLoad's process PID. The launcher sets `RMWEB_AUTH_RUNTIME` to its own runtime
before loading the runtime environment. It does not create a network namespace,
so normal tablet connectivity remains.
The catalog is distinct from other installed application entries and enables the
existing AppLoad virtual keyboard. The installed AppLoad must satisfy the
[QTFB lifetime, touch and keyboard privacy prerequisites](../../patches/appload.md).
No installer is included; the operator must stage this dedicated directory/catalog with root ownership and
private executable permissions, verify the build receipt, and remove only that
owned test directory/catalog afterward. Never swap the production executable,
edit its manifest, or change existing boot behavior for this test.

## Automated native gates

```sh
bash tools/passkey-acceptance/native/test-engine.sh \
  /absolute/private/acceptance-build
```

This checks the packaged application's receipt and uses its own runtime for
four actual-engine cases in a disposable, network-disabled ARM64
container: trusted loopback HTTPS load/reload, rejected same-origin wrong route,
rejected other origin, and rejected blank navigation. It uses a disposable CA
trusted only by that child fixture, not a production TLS exception. An owned
TEST-NET dummy address satisfies libc's resolver check; it adds no external
route. The fixture does not run a phone ceremony, view a real account, save page
captures or output URLs. Status output contains only fixed labels and booleans.

`native/test-entry.py BINARY` runs the existing namespace/PID/argv/ownership
fixture directly with `--application-root /opt/rmweb-passkey-acceptance-fixture`
against the unchanged production entry binary. Run it only inside a disposable
root-owned Docker container, with the same SDK loader and `--cap-add SYS_ADMIN
--security-opt seccomp=unconfined`; run again without those capabilities using
`--without-mount-capability` to check refusal. These privileges belong only to
the disposable container. See the existing `tests/auth_entry_test.py` fixture.

## Validation evidence and boundaries

On 2026-09-17:

- The acceptance server, guided UI and runner passed 54 Node tests and JavaScript
  syntax checks. Six preparation tests passed in Linux, including private
  session-code ownership, mode and content checks.
- The self-contained app built with the official Paper Pro 3.28 SDK / Qt 6.10.3.
  The native policy fixture, four actual-WPE route fixtures and 29 production
  namespace-entry cases passed. The runtime used existing verified patched WPE
  libraries; these checks did not rebuild the complete engine.
- The complete package's hashes and read-only private helper namespace were
  verified on a Paper Pro running **3.28.0.172 / Qt 6.10.3**. After creating the
  disposable phone passkey and approving its QR assertion, the user confirmed
  that the tablet displayed **PASS**.

That is physical evidence for this disposable relying party, not arbitrary
account sign-in, another device/firmware, or every cancellation/recovery path.
Build again for the actual trusted test hostname. A synthetic fixture build
using `https://login.example.com` is not a live catalog.

## Physical acceptance checklist

1. Start a fresh HTTPS server. On the phone, open `/register`, join with the
   disposable access code and tap **Create test passkey**. The guided demo's
   private link joins automatically.
2. Open **Passkey Demo** from its separate tablet AppLoad entry. Join with the
   same code if it was not provisioned through `session-code`. Wait for phone
   registration, then tap **Test phone passkey**.
3. Scan the native QR on the phone, select the disposable passkey and approve
   user verification. Require **PASS** on the tablet. A QR, authenticated phone
   channel or “no matching passkey” alone is insufficient.
4. Test Cancel/Return and confirm the app/helper close, the stock interface
   remains responsive, and Bluetooth/wake state is restored.
5. Stop the server, remove the disposable phone passkey for this exact RP,
   remove only the dedicated catalog/files, and unset the environment code.

Sources: [SimpleWebAuthn server documentation](https://simplewebauthn.dev/docs/packages/server),
[maintainer releases](https://github.com/MasterKale/SimpleWebAuthn/releases),
[npm package metadata](https://registry.npmjs.org/@simplewebauthn/server/14.0.2).
