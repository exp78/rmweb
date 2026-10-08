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
fixed diagnostic classification cases. The contributor also reported a
disposable relying-party **PASS** on Paper Pro 3.28.0.172 / Qt 6.10.3 on
2026-09-17; maintainer verification of that on-device result is pending. This
is separate from arbitrary account sign-in. See
`tests/auth-webauthn-provider/README.md` for the synthetic fixture's precise
boundary.

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

## Bundled headless frame pacing

Beyond the assertion provider, this patch intentionally bundles a second,
separable component in `Source/WebKit/WPEPlatform/wpe/headless/WPEViewHeadless.cpp`:
it reworks frame pacing on the headless view. Upstream schedules frames at a
fixed 60 fps from buffer arrival; the bundled change

- caps passive compositing at 8 frames per second,
- raises pacing to 30 Hz for about one second after observed native input
  (pointer, scroll, keyboard, touch) so queued animated frames drain promptly
  and a tap or keystroke is not displayed late, and
- stamps the pacing reference at completed presentation instead of buffer
  arrival, so fast producers no longer alternate delayed with immediate frames.

First and overdue frames stay immediate and buffer backpressure is unchanged.

`WPEViewHeadless` is rmweb's production render path: both the regular browser
and the authentication browser create their display with
`wpe_display_headless_new`, so every presented frame flows through this view.
On e-ink the panel cannot usefully show 60 fps, while each composited frame
costs CPU time and power before its snapshot reaches the display controller.
Bounding passive compositing to 8 fps saves both; the short 30 Hz window after
input preserves interaction latency where it is perceptible.

The component rides in this patch rather than a second patch because both touch
the same staged engine build, pin and qualification cycle; it is documented
here so the bundle carries no undocumented behavior. The offline actual-engine
timing fixture `tests/auth_frame_pacing_smoke.cpp` covers static, animated and
continuous frame production, including the post-input burst and the return to
passive pacing.

## AppLoad prerequisites

[AppLoad prerequisites](appload.md) documents the pinned AppLoad source, the QTFB lifetime,
keyboard-log removal and touch-cancellation patches, their apply order and
regression checks. These patches change the separate AppLoad dependency; they
are not applied by rmweb's browser build. Firmware-specific stock-UI hooks must
already be qualified for the target device.
