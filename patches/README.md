# Native assertion and AppLoad patches

`wpe-2.48.5-native-assertion-provider.patch` applies to the official WPE WebKit
2.48.5 source archive, SHA-256
`01f36010705adb14404c56baf033147f7927cc7c6badec81bb141266fcdd8d0b`.
`wpe-2.48.5-native-assertion-provider-files.json` records the patched source bytes.
Original license notices remain intact. New GLib API files use LGPL-2.0-or-later;
the WPE coordinator uses BSD-2-Clause. Deliver the upstream source and patch with
the built library.

The native `WebKitWebView::webauthn-request` signal carries immutable trusted
origin, RP ID, request-options JSON, and the exact 32-byte client-data hash.
The application retains the request while its helper runs, observes the borrowed
`GCancellable`, then completes once with copied assertion buffers or cancels.
All calls occur on the view's GLib thread. A missing handler fails explicitly.

The UI process checks its own frame URL/origin, same-origin ancestors, HTTPS,
and the RP suffix against WebKit's public-suffix policy. Original WebCore secure
context, document focus, permissions policy, and abort handling remain active.
Client-data JSON is constructed once in WebKit; the helper signs its exact hash.
Returned RP hash, presence/verification flags, allow-list membership, and bounds
are checked before producing the DOM credential.

This profile supports ordinary phone/hybrid assertions only. Registration,
platform authenticators, conditional/silent mediation, cross-origin/inherited
blank-frame requests, related-origin requests, and unsupported extensions fail
explicitly. Navigation, frame destruction, page closure, timeout, and abort
cancel pending work; stale completion is rejected.

The official SDK build and offline actual-engine tests passed, including the
fixed diagnostic classification cases. A user-confirmed disposable relying-party
**PASS** was recorded on Paper Pro 3.28.0.172 / Qt 6.10.3 on 2026-09-17. This is separate from arbitrary account
sign-in. See `tests/auth-webauthn-provider/README.md` for
the synthetic fixture's precise boundary.

## Fixed diagnostic events

The WPE UI-process provider emits literal `rmweb-webauthn:` labels through
`syslog(LOG_AUTHPRIV | LOG_NOTICE, ...)`: `assertion-received`,
`reject-secure-origin`, `reject-mediation`, `reject-rp`, `unhandled-ui`,
`completed`, and `cancelled`. Unsupported option labels are
`reject-options-appid`, `reject-options-credProps`, `reject-options-largeBlob`,
`reject-options-prf`, `reject-options-platform`, and
`reject-options-credential-type`. Bounds labels distinguish
`reject-challenge-empty`, `reject-challenge-too-large`, `reject-allowlist-too-large`,
`reject-credential-id-empty`, and `reject-credential-id-too-large`; they never
include a numeric length or count. Only the first failed gate is reported.
No request values or formatted arguments are passed to syslog. Diagnostic calls
do not change rejection results, timeouts, or cancellation behavior.

The challenge limit is 16,384 decoded bytes. Empty challenges still fail.
Credential IDs remain limited to 1,024 bytes, allowlists to 64 entries, and the helper request envelope to 128 KiB.
The trusted-origin, RP, verification, response-binding, and lifetime checks are
unchanged. This is a bounded compatibility budget, not a WebAuthn maximum or
a guarantee that every relying party is supported.

`assertion-received` means the request reached the UI-process provider; WebCore
can reject a request before that boundary. `completed` means the provider
accepted assertion buffers, not that the relying party accepted sign-in.
`cancelled` covers every non-success completion, including timeout, an invalid
assertion, and an unhandled UI request. Late calls produce no additional finish
event. System syslog routing and retention apply to these fixed labels.

## AppLoad prerequisites

[AppLoad prerequisites](appload.md) documents the pinned AppLoad source, the QTFB lifetime,
keyboard-log removal and touch-cancellation patches, their apply order and
regression checks. These patches change the separate AppLoad dependency; they
are not applied by rmweb's browser build. Firmware-specific stock-UI hooks must
already be qualified for the target device.
