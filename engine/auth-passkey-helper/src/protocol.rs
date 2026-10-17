use std::io;
use std::time::Duration;

use libwebauthn::ops::webauthn::{GetAssertionRequest, OriginValidation, PublicSuffixList, RelatedOrigins, RequestOrigin, RequestSettings};
use libwebauthn::proto::ctap2::{Ctap2GetAssertionRequest, Ctap2GetAssertionResponse};
use publicsuffix::Psl;
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use tokio::io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt};

pub const INPUT_LIMIT: usize = 128 * 1024;
pub const RECORD_LIMIT: usize = 64 * 1024;
pub const OUTPUT_LIMIT: usize = 512 * 1024;

#[derive(Clone, Copy, Debug, PartialEq, Serialize)]
pub enum ErrorName { NotAllowedError, NotSupportedError, OperationError }

#[derive(Deserialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
struct Input {
    version: u32,
    origin: String,
    options: Options,
    client_data_hash: String,
}

#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
struct Options {
    challenge: String,
    #[serde(rename = "rpId", skip_serializing_if = "Option::is_none")]
    rp_id: Option<String>,
    #[serde(default)]
    allow_credentials: Vec<Descriptor>,
    #[serde(default = "preferred")]
    user_verification: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    timeout: Option<u32>,
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    hints: Vec<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    extensions: Option<serde_json::Map<String, serde_json::Value>>,
}
fn preferred() -> String { "preferred".into() }

#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct Descriptor {
    r#type: String,
    id: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    transports: Option<Vec<String>>,
}

pub struct Prepared {
    pub ctap: Ctap2GetAssertionRequest,
    pub timeout: Duration,
    pub require_uv: bool,
}

struct EmbeddedPsl(publicsuffix::List);
impl PublicSuffixList for EmbeddedPsl {
    fn public_suffix(&self, host: &str) -> Option<String> {
        let suffix = self.0.suffix(host.as_bytes())?;
        if !suffix.is_known() { return None; }
        std::str::from_utf8(suffix.as_bytes()).ok().map(String::from)
    }
    fn registrable_domain(&self, host: &str) -> Option<String> {
        if !self.0.suffix(host.as_bytes())?.is_known() { return None; }
        std::str::from_utf8(self.0.domain(host.as_bytes())?.as_bytes()).ok().map(String::from)
    }
}

fn decode(value: &str, min: usize, max: usize) -> Result<Vec<u8>, ErrorName> {
    if value.len() > max.div_ceil(3) * 4 || !value.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'-' || b == b'_') {
        return Err(ErrorName::OperationError);
    }
    let bytes = base64_url::decode(value).map_err(|_| ErrorName::OperationError)?;
    if !(min..=max).contains(&bytes.len()) || base64_url::encode(&bytes) != value {
        return Err(ErrorName::OperationError);
    }
    Ok(bytes)
}

pub async fn prepare(bytes: &[u8]) -> Result<Prepared, ErrorName> {
    if bytes.len() > INPUT_LIMIT { return Err(ErrorName::OperationError); }
    // Typed deserialization rejects duplicate fields and unknown request keys.
    let mut input: Input = serde_json::from_slice(bytes).map_err(|_| ErrorName::OperationError)?;
    if input.version != 1 { return Err(ErrorName::NotSupportedError); }
    let url = url::Url::parse(&input.origin).map_err(|_| ErrorName::OperationError)?;
    if input.origin.len() > 2048 || url.scheme() != "https" || url.host_str().is_none()
        || !url.username().is_empty() || url.password().is_some()
        || url.origin().ascii_serialization() != input.origin {
        return Err(ErrorName::OperationError);
    }
    let hash = decode(&input.client_data_hash, 32, 32)?;
    decode(&input.options.challenge, 1, 16 * 1024)?;
    if input.options.rp_id.as_ref().is_some_and(|v| v.is_empty() || v.len() > 253)
        || input.options.allow_credentials.len() > 64 {
        return Err(ErrorName::OperationError);
    }
    if !matches!(input.options.user_verification.as_str(), "required" | "preferred" | "discouraged") {
        return Err(ErrorName::OperationError);
    }
    if input.options.extensions.as_ref().is_some_and(|v| !v.is_empty()) {
        return Err(ErrorName::NotSupportedError);
    }
    if input.options.hints.len() > 3 || input.options.hints.iter().any(|v| !matches!(v.as_str(), "security-key" | "client-device" | "hybrid")) {
        return Err(ErrorName::NotSupportedError);
    }
    for descriptor in &input.options.allow_credentials {
        if descriptor.r#type != "public-key" { return Err(ErrorName::NotSupportedError); }
        decode(&descriptor.id, 1, 1024)?;
        if let Some(transports) = &descriptor.transports {
            if transports.len() > 6 || transports.iter().any(|v| !matches!(v.as_str(), "usb" | "nfc" | "ble" | "internal" | "hybrid" | "smart-card")) {
                return Err(ErrorName::NotSupportedError);
            }
        }
    }
    let timeout = input.options.timeout.unwrap_or(120_000).min(120_000);
    if timeout == 0 { return Err(ErrorName::NotAllowedError); }
    input.options.timeout = Some(timeout);
    let origin: RequestOrigin = input.origin.as_str().try_into().map_err(|_| ErrorName::OperationError)?;
    let psl = EmbeddedPsl(include_str!("../vendor/public_suffix_list.dat").parse().map_err(|_| ErrorName::OperationError)?);
    let options = serde_json::to_string(&input.options).map_err(|_| ErrorName::OperationError)?;
    let request = GetAssertionRequest::prepare(&origin, &options, &RequestSettings {
        origin: OriginValidation::Validate { public_suffix_list: &psl, related_origins: RelatedOrigins::Disabled },
    }).await.map_err(|_| ErrorName::OperationError)?;
    let require_uv = input.options.user_verification == "required";
    let mut ctap = Ctap2GetAssertionRequest::from(request);
    // WebKit owns clientDataJSON. Sign its exact hash; do not regenerate JSON.
    ctap.client_data_hash = hash.into();
    Ok(Prepared { ctap, timeout: Duration::from_millis(timeout.into()), require_uv })
}

#[derive(Serialize)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum Message {
    Qr { size: usize, modules: String },
    Status { state: &'static str },
    Result {
        #[serde(rename = "credentialId")]
        credential_id: String,
        #[serde(rename = "authenticatorData")]
        authenticator_data: String,
        signature: String,
        #[serde(rename = "userHandle", skip_serializing_if = "Option::is_none")]
        user_handle: Option<String>,
    },
    Error { name: ErrorName },
}

pub fn qr_message(payload: &str) -> Result<Message, ErrorName> {
    let code = qrcode::QrCode::with_error_correction_level(payload.as_bytes(), qrcode::EcLevel::M)
        .map_err(|_| ErrorName::OperationError)?;
    let size = code.width();
    if !(21..=177).contains(&size) { return Err(ErrorName::OperationError); }
    let modules: String = code.to_colors().into_iter().map(|c| if c == qrcode::Color::Dark { '1' } else { '0' }).collect();
    if modules.len() != size * size { return Err(ErrorName::OperationError); }
    Ok(Message::Qr { size, modules })
}

pub fn assertion_message(prepared: &Prepared, response: Ctap2GetAssertionResponse) -> Result<Message, ErrorName> {
    // Native CTAP decoder retains the exact signed bytes. Never reconstruct them.
    let auth = response.authenticator_data.raw.as_ref().ok_or(ErrorName::OperationError)?;
    if !(37..=16 * 1024).contains(&auth.len()) || response.signature.is_empty()
        || response.signature.len() > 4096 || response.authenticator_data.attested_credential.is_some() {
        return Err(ErrorName::OperationError);
    }
    let rp_hash = Sha256::digest(prepared.ctap.relying_party_id.as_bytes());
    if auth[..32] != rp_hash[..] || auth[32] & 1 == 0 || (prepared.require_uv && auth[32] & 4 == 0) {
        return Err(ErrorName::OperationError);
    }
    let credential = match response.credential_id {
        Some(value) => value.id.into_vec(),
        None if prepared.ctap.allow.len() == 1 => prepared.ctap.allow[0].id.to_vec(),
        _ => return Err(ErrorName::OperationError),
    };
    if credential.is_empty() || credential.len() > 1024
        || (!prepared.ctap.allow.is_empty() && !prepared.ctap.allow.iter().any(|item| item.id.as_ref() == credential)) {
        return Err(ErrorName::OperationError);
    }
    // A single response cannot silently select the first of several accounts.
    if response.credentials_count.is_some_and(|count| count > 1) {
        return Err(ErrorName::NotSupportedError);
    }
    let user_handle = match response.user {
        Some(user) if !user.id.is_empty() && user.id.len() <= 64 => Some(base64_url::encode(&user.id)),
        Some(_) => return Err(ErrorName::OperationError),
        None if prepared.ctap.allow.is_empty() => return Err(ErrorName::OperationError),
        None => None,
    };
    Ok(Message::Result { credential_id: base64_url::encode(&credential), authenticator_data: base64_url::encode(auth),
        signature: base64_url::encode(&response.signature), user_handle })
}

pub async fn read_input<R: AsyncRead + Unpin>(reader: &mut R) -> Result<Vec<u8>, ErrorName> {
    let mut bytes = Vec::new();
    reader.take(INPUT_LIMIT as u64 + 1).read_to_end(&mut bytes).await.map_err(|_| ErrorName::OperationError)?;
    if bytes.is_empty() || bytes.len() > INPUT_LIMIT { return Err(ErrorName::OperationError); }
    Ok(bytes)
}

pub struct Output<W> { writer: W, used: usize, terminal: bool }
impl<W: AsyncWrite + Unpin> Output<W> {
    pub fn new(writer: W) -> Self { Self { writer, used: 0, terminal: false } }
    pub async fn send(&mut self, message: &Message) -> Result<(), io::Error> {
        if self.terminal { return Err(io::Error::other("output complete")); }
        let mut line = serde_json::to_vec(message).map_err(io::Error::other)?;
        line.push(b'\n');
        if line.len() > RECORD_LIMIT || self.used + line.len() > OUTPUT_LIMIT {
            return Err(io::Error::other("output bound"));
        }
        tokio::time::timeout(Duration::from_secs(1), async {
            self.writer.write_all(&line).await?;
            self.writer.flush().await
        }).await.map_err(|_| io::Error::new(io::ErrorKind::TimedOut, "output deadline"))??;
        self.used += line.len();
        self.terminal = matches!(message, Message::Result { .. } | Message::Error { .. });
        Ok(())
    }
}
