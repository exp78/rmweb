# Browser–helper integration fixture

This standalone SDK/WPE executable includes the production `AuthEngine`, links
the real Qt `AuthPasskey`, and invokes the real native WebKit provider from an
offline synthetic HTTPS document. Its child fixture is selected only by this
test executable's `--synthetic-helper` argument; the production helper path and
browser launch protocol are unchanged.

The child checks version, origin, RP, challenge, verification requirement,
allowlist, timeout and canonical 32-byte browser hash. It returns fixed synthetic
credential/authenticator bytes and echoes that hash as a synthetic signature.
The DOM independently hashes the returned original `clientDataJSON` and compares
all returned bytes. This tests the cross-thread and process glue, not signature
verification or phone authentication.

Abort and navigation cases wait until the helper's QR message, cancel the native
request, and require a normal child exit after actual SIGTERM. The child writes a
deliberately late result before exiting; it must be discarded. A further stale
native completion must leave the aborted/replacement document unchanged. Prompt
clearing, single completion, and native request release are also checked.
The fixture waits for the initial blank load to finish before inserting its
synthetic page. Navigation checks wait for the replacement's actual finished
load and keep promise outcomes in document-local objects, so a retiring page's
rejection handler cannot overwrite the replacement sentinel through `window`.

Build this directory with the same verified SDK, staged headers and isolated
runtime as `tests/auth-wpe-smoke`, then run:

```sh
sh tests/auth-passkey-browser/run-cases.sh /path/to/auth-passkey-browser-smoke
```

Run in a disposable ARM64 container with networking disabled, runtime WebKit
helpers available at their compiled path, and the SDK loader used by child
processes. No Bluetooth, device, credentials, user account, or external server is
used. Each scenario has a 15-second deadline; the fixture helper has its own
12-second alarm. WebKit diagnostics are suppressed; output contains fixed case
names and PASS/FAIL only.

The official 3.28 SDK build and all three cases passed against the linked
patched WPE runtime. Three additional navigation runs also passed after fixing
the fixture's document-lifetime races. These are synthetic integration results;
they do not establish a real phone assertion or account sign-in.
