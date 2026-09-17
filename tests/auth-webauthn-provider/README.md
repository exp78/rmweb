# Native WebAuthn provider tests

This fixture uses the actual WPE WebKit DOM, IPC, public GLib request API, and
headless view. It loads synthetic HTML with `https://login.example.com` as its
base URL. Run in an isolated container with networking disabled and an ephemeral
WebKit session; no authorization pages, accounts, Bluetooth, or phone are used.

Configure this directory with the official 3.28 SDK and the WPE development
headers. Run `run-cases.sh /path/to/native-webauthn-provider-test` against the
patched runtime. Each process has a ten-second deadline. The runner stops on the
first failure.

The core cases cover immutable origin/RP/options, the exact client-data hash, buffer
validation, single completion, reentrant and late completion after cancellation,
abort, navigation, timeout, RP/public-suffix rejection, unsupported modes, and
truthful capabilities, unfocused-document rejection, and real inherited
about:blank/srcdoc child rejection. The success fixture supplies **synthetic bytes, not a
cryptographically valid assertion**. It verifies DOM/native byte identity and
hash ownership; it cannot prove a phone signature or relying-party acceptance.

The added API is resolved dynamically, allowing the same test executable to
report an explicit missing-provider failure against the unmodified engine.
The official SDK compile with `-Wall -Wextra -Werror`, that baseline feature
failure, and all 29 cases against the linked native-provider engine passed.

## Real HTTPS child-frame cases

`run-https-cases.py` separately runs `child-https-success` and
`child-https-detach`. It generates an ephemeral CA and a leaf certificate for
`login.example.com`, then serves fixed `/parent` and `/child` pages on container
loopback. The provider loads real HTTPS documents, focuses the child, and calls
the native WebAuthn API. Removal happens after native dispatch and must cancel
once while rejecting both reentrant and late completion.

The launcher requires a root Linux `--network none` container with only the
loopback interface initially, `--cap-add NET_ADMIN`, and
`--add-host login.example.com:127.0.0.1`. It temporarily creates a dummy
`wpe-test0` interface with the TEST-NET address `192.0.2.1/32`: glibc rejects
family-specific `AI_ADDRCONFIG` lookups in a loopback-only namespace, which
otherwise prevents GIO from reaching the TLS server. The launcher verifies
the exact interface set, absence of IPv4 external routes, and resolution to
loopback, then removes the owned interface during cleanup. No external network
is attached. It passes the CA through
`SSL_CERT_FILE` to test processes only, removes proxy overrides, and makes no
certificate-policy exceptions or system trust changes. Server and fixture
process groups have bounded cleanup; temporary certificate material is removed
on success, failure, or SIGTERM. Container teardown is the final cleanup boundary
for SIGKILL or host failure.

After compiling the fixture, use the runtime setup in
`tests/auth-wpe-smoke/run.sh`; its isolated container runs both this launcher and
the 31-case core runner. To run only these two cases in that configured
container:

```sh
python3 /provider-fixtures/run-https-cases.py /provider/native-webauthn-provider-test
```

The harness itself can be checked without a WebKit build:

```sh
python3 tests/auth-webauthn-provider/https_fixture_test.py -v
docker run --rm --network none --cap-add NET_ADMIN --add-host login.example.com:127.0.0.1 \
  --platform linux/arm64 \
  -v "$PWD/tests/auth-webauthn-provider:/fixture:ro" \
  rmweb-app-sdk:3.28.0.172 python3 /fixture/https_fixture_test.py -v
```

All eleven harness cases passed in the official SDK container. They check the
resolver fix, interrupted interface creation and cleanup, trusted TLS, rejection
of an unknown CA or wrong hostname, fixed routes, exact case dispatch, failure
cleanup, and cancellation cleanup. On macOS five pass and the six isolated
process cases are skipped. Separately, both actual WebKit HTTPS cases passed
against the initial linked engine (29 core plus 2 HTTPS cases). These
results do not establish a valid phone signature or relying-party acceptance.

## Diagnostic classification and privacy

The same executable exports a test-only `syslog` sink. It captures at most 128
records of 511 bytes in memory and never forwards them to the system logger.
`run-diagnostic-cases.sh /path/to/native-webauthn-provider-test` runs 23 real
DOM/native cases, checking exact ordered labels and `LOG_AUTHPRIV | LOG_NOTICE`.
Any additional values, messages, incorrect provider priority, repeated terminal
event, or overflow fails the check. The exact pinned-library warning
`Libgcrypt warning: missing initialization - please fix the application` is
allowed at most once with `LOG_USER | LOG_WARNING`; no other unrelated record is ignored. Extension requests contain synthetic sentinel
values in their challenge, AppID, or PRF input; none may enter the captured
records. No real account, credential, phone, or Bluetooth adapter is involved.

The cases cover success, cancellation/abort/navigation/timeout, an unhandled UI,
secure-origin and RP rejection, unsupported mediation, AppID/credProps/largeBlob/
PRF classification, and distinct empty/oversized challenge, excessive allowlist,
and empty/oversized credential-ID labels. The platform and
credential-type defensive labels cannot be exercised through this WebKit
version's ordinary JavaScript request dictionary: platform attachment is not
serialized in request IPC, and the descriptor IDL accepts only `public-key`.
Their guards remain in the provider without adding a test policy exception.

The diagnostic test compiled with the official 3.28 SDK and failed against the
previous engine specifically because the fixed events were absent. The original
29-case runner and seven new option/bounds behavior cases passed against that
engine. After rebuilding, all 21 diagnostic cases, the original 29 provider
cases, both unchanged real-HTTPS cases, and the joined Qt/helper cases passed
through `tests/auth-wpe-smoke/run.sh` with networking disabled. The combined
runner also passed its synthetic keyboard and navigation checks. These results
qualify offline diagnostics and preserve the authentication behavior checks;
they do not establish successful phone authentication or device deployment.

The later five-way bounds-label refinement failed against the preceding engine
with the expected diagnostic label mismatch. The subsequent challenge-cap
candidate adds `challenge-1025` and `challenge-16384` acceptance cases and moves
`challenge-large` to 16,385 bytes. The test independently encodes known challenge
bytes with GLib and compares them with native options and returned
`clientDataJSON`; WebCrypto SHA-256 of the returned JSON must match the native
32-byte hash. The 1,025-byte acceptance case failed against the preceding
1 KiB-limit engine before the cap was changed. Empty challenge, allowlist count,
and credential-ID rejection cases are retained. Against the rebuilt 16 KiB-limit
engine, both acceptance cases and all five bounds-classification cases passed
with exact diagnostic labels (seven focused cases in an isolated SDK container).
The final combined engine/helper/browser suite passed through
`tests/auth-wpe-smoke/run.sh`: 31 core provider cases, 23 diagnostic cases,
2 real HTTPS child-frame cases, and 4 Qt bridge cases, plus synthetic keyboard
and navigation checks. Installation and physical retry remain separate gates;
these results do not establish the cause of the reported live failure.
