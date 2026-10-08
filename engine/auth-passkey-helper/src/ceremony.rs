use std::time::Duration;
use libwebauthn::transport::cable::channel::{CableUpdate, CableUxUpdate};
use libwebauthn::transport::cable::qr_code_device::{CableQrCodeDevice, CableTransports, QrCodeOperationHint};
use libwebauthn::transport::{Channel, ChannelSettings, Device};
use libwebauthn::proto::ctap2::Ctap2;
use libwebauthn::webauthn::error::{CtapError, WebAuthnError};
use libwebauthn::UvUpdate;
use tokio::io::AsyncWrite;
use crate::protocol::{Prepared, Output, Message, ErrorName, qr_message, assertion_message};

pub async fn assertion<W: AsyncWrite + Unpin>(prepared: &Prepared, output: &mut Output<W>) -> Result<Message, ErrorName> {
    let mut device = CableQrCodeDevice::new_transient(QrCodeOperationHint::GetAssertionRequest, CableTransports::CloudAssistedOnly)
        .map_err(|_| ErrorName::OperationError)?;
    // The payload stays in memory and is emitted only as a QR matrix to the
    // owning browser process. No pairing record or credential file is created.
    let qr = qr_message(&device.qr_code.to_string())?;
    output.send(&qr).await.map_err(|_| ErrorName::OperationError)?;
    let mut channel = tokio::time::timeout(Duration::from_secs(2), device.channel(ChannelSettings::default()))
        .await.map_err(|_| ErrorName::NotAllowedError)?.map_err(|_| ErrorName::NotAllowedError)?;
    let mut updates = channel.get_ux_update_receiver();
    let result;
    {
        let ceremony = channel.ctap2_get_assertion(&prepared.ctap, prepared.timeout);
        tokio::pin!(ceremony);
        let deadline = tokio::time::sleep(prepared.timeout);
        tokio::pin!(deadline);
        let mut last_status = None;
        result = loop {
            tokio::select! {
                biased;
                response = &mut ceremony => {
                    break match response {
                        Ok(response) => assertion_message(prepared, response),
                        Err(WebAuthnError::Transport(_)) => Err(ErrorName::NotAllowedError),
                        Err(WebAuthnError::Ctap(CtapError::UnsupportedOption | CtapError::UnsupportedExtension | CtapError::UnsupportedAlgorithm)) => Err(ErrorName::NotSupportedError),
                        Err(WebAuthnError::Ctap(_)) => Err(ErrorName::NotAllowedError),
                        Err(_) => Err(ErrorName::OperationError),
                    };
                }
                _ = &mut deadline => { break Err(ErrorName::NotAllowedError); }
                update = updates.recv() => {
                    let state = match update {
                        Ok(CableUxUpdate::CableUpdate(CableUpdate::ProximityCheck)) => "waiting_for_phone",
                        Ok(CableUxUpdate::CableUpdate(CableUpdate::Connecting)) => "connecting",
                        Ok(CableUxUpdate::CableUpdate(CableUpdate::Authenticating)) => "verifying",
                        Ok(CableUxUpdate::CableUpdate(CableUpdate::Connected)) => "connected",
                        Ok(CableUxUpdate::CableUpdate(CableUpdate::Error(_))) => { break Err(ErrorName::NotAllowedError); }
                        Ok(CableUxUpdate::UvUpdate(UvUpdate::PresenceRequired)) => "confirm_on_phone",
                        Ok(CableUxUpdate::UvUpdate(UvUpdate::PinRequired(pin))) => { pin.cancel(); break Err(ErrorName::NotSupportedError); }
                        Ok(CableUxUpdate::UvUpdate(UvUpdate::PinNotSet(pin))) => { pin.cancel(); break Err(ErrorName::NotSupportedError); }
                        Ok(CableUxUpdate::UvUpdate(UvUpdate::UvRetry { .. })) => { break Err(ErrorName::NotAllowedError); }
                        Err(_) => { break Err(ErrorName::NotAllowedError); }
                    };
                    if last_status != Some(state) {
                        if output.send(&Message::Status { state }).await.is_err() { break Err(ErrorName::OperationError); }
                        last_status = Some(state);
                    }
                }
            }
        };
    }
    // Upstream close currently does no work; Drop aborts the owned connection
    // task. Process exit subsequently closes its D-Bus discovery ownership.
    let _ = tokio::time::timeout(Duration::from_secs(1), channel.close()).await;
    drop(channel);
    result
}
