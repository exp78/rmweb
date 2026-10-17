# Disposable phone-passkey demo

From this directory, with Node 22 or newer, dependencies installed with
`npm ci`, and an installed `cloudflared` on `PATH`:

```sh
npm run demo
# Or select an absent session file outside the repository:
npm run demo -- --session-file /absolute/private/session.json
```

The runner creates a temporary public HTTPS Quick Tunnel for this disposable
test only. It never signs in to a real account. It prints fixed status messages
and the private session file path, without printing the access token, join
links, or raw tunnel diagnostics. The file is created exclusively with mode
0600; an existing file is never replaced. Without `--session-file`, it lives in
a fresh private temporary directory. The file contains `origin`,
`registrationURL`, `verificationURL`, and the runner's `pid`.

Open the private registration link on the phone to create a disposable passkey.
Prepare the tablet app for this run as described below, then open **Passkey
Demo** in AppLoad.
The links include a random 32-byte access code in their `#join=` fragment; treat
the links and session file as private. The page consumes and removes that
fragment before automatically joining. Creating a passkey or requesting an
assertion still requires the page's explicit action button. Registration and
verification must use the same runner session and exact HTTPS origin.

## Prepare the tablet for this run

A Quick Tunnel assigns a new origin each run. Build the native app with that
exact `origin` from the session file using `native/build.sh ORIGIN
ENGINE_ARTIFACTS HELPER_ARTIFACTS FRESH_OUTPUT_DIRECTORY` (see the main README).
A binary built for an earlier tunnel will refuse the new site. Build the engine from the current source with
`scripts/build-auth-engine.py` and export the helper as described in its
README. Both artifact sets must pass the native builder's provenance checks.

Stage the verified application and its separate catalog as documented in the
README. Keep the catalog argument as the bare `ORIGIN/verify`. To avoid typing
an access code on the tablet, extract the base64url code after `#join=` from
this run's private `verificationURL` and install it as the single-line file
`/home/root/rmweb-passkey-acceptance/session-code`, owned by root with mode
`0600`. It must contain only 32–128 letters, digits, `_`, or `-`, with no newline.
The launcher validates this file and appends its contents only to the browser's
launch argument; the catalog never contains the code. A missing file leaves
manual joining available. Do not pass the fragment-bearing link to the shell
launcher or put it in a shared catalog.

Exit any running AppLoad application, open AppLoad, tap **Reload**, then open
**Passkey Demo**. Open or scan this run's private `registrationURL` on the phone
and tap **Create test passkey**. The tablet notices registration and enables
**Test phone passkey**; tap it and scan the native passkey QR. Only the verified
server result produces **PASS**. Remove the test catalog/application and its
`session-code` after the run; no boot configuration change is needed.

## Transport and lifetime

The local HTTP listener binds only to `127.0.0.1` on an OS-assigned port. It
returns a fixed 503 while the tunnel starts, then accepts only the tunnel's
exact Host and Origin. Cloudflare provides public HTTPS; the proxy's local hop
is HTTP over loopback. The server does not trust `Forwarded` or
`X-Forwarded-*` headers, and its cookies remain Secure, HttpOnly, and SameSite
Strict. No certificate bypass or trust-store change is used. Existing direct
LAN HTTPS mode remains unchanged.

Startup is limited to 45 seconds. The demo closes 30 minutes after its links
are ready, or on Ctrl-C/SIGTERM, server failure, tunnel exit, or excessive tunnel
diagnostics. It stops the child, closes HTTP connections, and removes its
session file and temporary directory. The child receives an isolated temporary
home/configuration and no inherited tunnel credentials or logging overrides.
SIGKILL or host power loss cannot run cleanup; remove only that run's owned
session file/directory after confirming its processes have exited.

The URL is parsed only from bounded `cloudflared` stderr and must be a canonical
`https://<single-label>.trycloudflare.com` origin. The runner never follows
arbitrary links from diagnostics. A startup failure reports only a fixed
message; no account login, named tunnel, or persistent Cloudflare resource is
created. See the official [Quick Tunnel documentation](https://developers.cloudflare.com/cloudflare-one/networks/connectors/cloudflare-tunnel/do-more-with-tunnels/trycloudflare/)
for the temporary public tunnel's behavior and limitations.

Source tests use mocked tunnel processes and real local HTTP sockets. They do
not contact Cloudflare or perform a phone ceremony:

```sh
node --test test/demo.test.js test/server.test.js
npm run check
```

The runner does not build, install, or launch tablet software. A ready tunnel,
displayed QR, or authenticated phone channel does not establish a successful
passkey assertion. Require the test verifier's successful signature, challenge,
origin, RP, presence, and user-verification result; then stop the demo and remove
the disposable phone passkey.
