# Phone passkey helper

This Linux/AArch64 helper performs one assertion through the standard caBLE v2 phone transport. It is called by the native WebKit authentication provider, never by page JavaScript. It cannot register credentials, enumerate accounts, reset authenticators, request a host PIN, or retain phone pairings. Native WebKit must first enforce secure context, frame, RP, user-activation and cancellation rules. This helper additionally validates the HTTPS origin/RP with the embedded Public Suffix List, then signs WebKit's exact 32-byte `clientDataHash`; WebKit retains its original `clientDataJSON`.

## Private pipe protocol

Launch without arguments, with anonymous stdin/stdout pipes. Write one UTF-8 JSON object and close stdin within two seconds:

```json
{"version":1,"origin":"https://login.example.com","options":{"challenge":"AA","rpId":"example.com","allowCredentials":[],"userVerification":"required","timeout":120000},"clientDataHash":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"}
```

The example is synthetic. The native provider supplies real values in memory, never through arguments, URLs, logs or files. Input is at most 128 KiB. Unknown fields, duplicate fields and nonempty extensions fail closed. Supported challenges are 1–16384 bytes; allowlists at most 64 entries with 1–1024 byte IDs. Binary fields use canonical unpadded base64url. The caller's timeout is capped at 120 seconds; the process reserves cleanup time within that limit.

The 16 KiB challenge limit is a local compatibility budget, not a WebAuthn maximum: the [WebAuthn specification](https://www.w3.org/TR/webauthn-3/#sctn-cryptographic-challenges) sets no 1 KiB ceiling. A maximum challenge and 64 maximum-size credential IDs fit within the unchanged 128 KiB JSON budget. Tests exercise 1025/16384-byte challenges, preserve the exact browser hash and request binding, and reject both a 16385-byte challenge and input exceeding the total budget. This bound is a local interoperability limit; it does not imply that every relying party is supported.

Stdout contains bounded JSON lines, each at most 64 KiB and total at most 512 KiB:

- `{"type":"qr","size":N,"modules":"0101…"}`: a 21–177 square QR matrix in row order, without quiet zone; the frontend adds four modules around it.
- `{"type":"status","state":"waiting_for_phone"}`: states also include `connecting`, `verifying`, `connected`, `confirm_on_phone`.
- One terminal `result` containing `credentialId`, `authenticatorData`, `signature`, and optional `userHandle`, all canonical unpadded base64url; or `error` with name `NotAllowedError`, `NotSupportedError`, or `OperationError`.

Signed authenticator bytes are returned verbatim, not reconstructed. The helper checks RP hash, presence, required verification, allowlist binding and bounded response fields (authenticator data 37–16384 bytes, signature 1–4096 bytes, user handle 1–64 bytes). It does not have the RP's stored public key; signature verification remains the RP's responsibility. Multiple results requiring a separate account-selection flow are unsupported. A transport close is not reported as a successful assertion or decoded `NoCredentials`.

## Lifetime and hardware

The helper sets parent-death SIGTERM, handles SIGTERM/SIGINT, disables core dumps and all dependency tracing, and suppresses stderr. It opens no credential or QR files. Only a verified Paper Pro (Ferrari) with one built-in `btnxpuart` adapter is supported. A request-scoped platform guard holds a finite wake lock named `rmweb-passkey-<pid>-<sequence>`, prepares the stock Bluetooth module/service only if no adapter exists, powers the adapter when needed, and attempts bounded restoration of its prior power after success, failure or ordinary cancellation. Existing powered adapters are preserved; unrelated adapters are rejected. A root-owned empty lock at `/run/rmweb-passkey.lock` serializes helpers through power restoration; it contains no credential data. No boot files are changed or pre-existing modules removed. SIGKILL/power loss cannot run asynchronous restoration; the kernel wake lock expires independently.

Before replacing the helper, stop new launches and allow active helpers to finish
restoration and exit. Never unlink the lock while a helper may hold it: replacing
its inode breaks serialization. The empty lock file can remain until reboot.

## Rebuild and validation

`prepare.py --cache /absolute/private/cache` verifies the pinned upstream archive and PSL, then prints a fresh source-stage path outside the checkout. It applies `patches/libwebauthn-buffered-response.patch`; generated dependency source is not committed. `Cargo.lock` fixes crate versions. In an ARM64 container with Rust 1.90 and the official Paper Pro 3.28 SDK, run:

```sh
/path/to/stage/build-sdk.sh /path/to/stage /absolute/build-cache
```

The script runs helper/platform tests with the SDK target loader, builds the release executable and runs real-process pipe, EOF, signal and parent-death tests. No test performs a phone ceremony. Output is `build-cache/target/aarch64-unknown-linux-gnu/release/rmweb-auth-passkey-helper`.

To export the executable, license map and complete source archive, run in the same build container (with `--init` when invoking standalone process tests):

```sh
python3 /path/to/stage/package.py --stage /path/to/stage \
    --binary /absolute/build-cache/target/aarch64-unknown-linux-gnu/release/rmweb-auth-passkey-helper \
    --output /absolute/private/helper-artifacts --cargo /usr/local/cargo/bin/cargo \
    --image-id sha256:YOUR_VERIFIED_BUILD_IMAGE --rmweb-revision YOUR_SOURCE_COMMIT
```

Use an absent output directory. The exporter verifies the prepared source hashes, vendors every locked crate source/license, checks the ELF target, and writes `rmweb-auth-passkey`, `licenses/`, and `manifest.json`. It refuses altered staged sources, changed binaries, reused dependency directories and existing output. Capture the actual image ID with `docker image inspect` and repository revision with `git rev-parse HEAD`; the separate helper-source hash covers uncommitted helper changes.

Source pin: libwebauthn v0.9.0 prerelease, commit `a6bdb700918f4c8c06c52d41f4d2d9e79888c5ad`, archive SHA256 `9cbd2d5afe03cc33f7bdbb7a9d5c81922d8008e55520beb2f4e6cfe35016bb54`, from the URL in `prepare.py`. LGPL-2.1-or-later terms are retained in `vendor/libwebauthn-COPYING`. The explicit patch preserves a decoded response queued before peer closure; its actual-source regression was red before the patch and green afterward. The PSL snapshot is a checksum-pinned official download (`2026-09-15_10-18-26_UTC`) under MPL-2.0, with notice in the file.

On 2026-09-17, a user confirmed **PASS** from the disposable relying-party
verifier on a Paper Pro running software 3.28.0.172 / Qt 6.10.3, after creating
a test passkey on a phone and approving the browser's QR assertion. This
establishes that test's signed assertion path; arbitrary account sign-in and
other hardware remain unverified. See [the acceptance harness](../../tools/passkey-acceptance/README.md).

Ship this helper's complete source, Cargo.lock, dependency pin, patch, license
notices and rebuild recipe alongside the binary; preserve the ability to
rebuild/relink the LGPL dependency.
