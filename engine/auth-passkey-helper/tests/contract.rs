use std::collections::BTreeMap;
use std::time::Duration;
use rmweb_auth_passkey_helper::protocol::*;
use libwebauthn::proto::ctap2::Ctap2GetAssertionResponse;
use serde_json::{json, Value};
use serde_cbor_2::Value as Cbor;
use sha2::{Digest, Sha256};

fn input() -> Value {
    json!({"version":1,"origin":"https://login.example.com","clientDataHash":base64_url::encode(&[0x92;32]),
        "options":{"challenge":base64_url::encode(&[0x17;32]),"rpId":"example.com","userVerification":"required",
        "allowCredentials":[{"type":"public-key","id":base64_url::encode(&[0x48;32]),"transports":["hybrid"]}]}})
}
async fn prepare_value(value: &Value) -> Result<Prepared, ErrorName> { prepare(&serde_json::to_vec(value).unwrap()).await }
fn assertion(raw: Vec<u8>, credential: Option<Vec<u8>>, user: Option<Vec<u8>>) -> Ctap2GetAssertionResponse {
    let mut map = BTreeMap::from([
        (Cbor::Integer(2), Cbor::Bytes(raw)),
        (Cbor::Integer(3), Cbor::Bytes(vec![0xaa;72])),
    ]);
    if let Some(id) = credential {
        map.insert(Cbor::Integer(1), Cbor::Map(BTreeMap::from([
            (Cbor::Text("type".into()), Cbor::Text("public-key".into())),
            (Cbor::Text("id".into()), Cbor::Bytes(id)),
        ])));
    }
    if let Some(id) = user { map.insert(Cbor::Integer(4), Cbor::Map(BTreeMap::from([(Cbor::Text("id".into()), Cbor::Bytes(id))]))); }
    serde_cbor_2::from_slice(&serde_cbor_2::to_vec(&Cbor::Map(map)).unwrap()).unwrap()
}
fn raw_auth() -> Vec<u8> {
    let mut raw = Sha256::digest(b"example.com").to_vec();
    raw.extend([5, 0, 0, 0, 17]);
    raw
}

#[tokio::test] async fn exact_browser_hash_and_request_binding() {
    let p = prepare_value(&input()).await.unwrap();
    assert_eq!(p.ctap.client_data_hash.as_ref(), &[0x92;32]);
    assert_eq!(p.ctap.relying_party_id,"example.com");
    assert_eq!(p.ctap.allow[0].id.as_ref(), &[0x48;32]);
    let options = p.ctap.options.unwrap();
    assert!(options.require_user_presence && options.require_user_verification);
    assert!(p.ctap.pin_auth_param.is_none() && p.ctap.pin_auth_proto.is_none());
    assert_eq!(p.timeout,Duration::from_secs(120));
}

#[tokio::test] async fn larger_challenges_preserve_browser_hash_and_request_binding() {
    let mut accepted = Vec::new();
    for length in [1025, 16384] {
        let mut value = input();
        value["options"]["challenge"] = base64_url::encode(&vec![0x17; length]).into();
        let result = prepare_value(&value).await;
        accepted.push(result.is_ok());
        if let Ok(prepared) = result {
            assert_eq!(prepared.ctap.client_data_hash.as_ref(), &[0x92; 32]);
            assert_eq!(prepared.ctap.relying_party_id, "example.com");
            assert_eq!(prepared.ctap.allow[0].id.as_ref(), &[0x48; 32]);
            let options = prepared.ctap.options.unwrap();
            assert!(options.require_user_presence && options.require_user_verification);
            assert!(prepared.ctap.pin_auth_param.is_none() && prepared.ctap.pin_auth_proto.is_none());
        }
    }
    assert_eq!(accepted, [true, true]);
}

#[tokio::test] async fn largest_challenge_and_allowlist_still_obey_total_input_budget() {
    let mut value = input();
    value["options"]["challenge"] = base64_url::encode(&vec![0x17; 16384]).into();
    value["options"]["allowCredentials"] = json!((0..64).map(|index| json!({
        "type": "public-key", "id": base64_url::encode(&vec![index as u8; 1024]),
        "transports": ["usb", "nfc", "ble", "internal", "hybrid", "smart-card"]
    })).collect::<Vec<_>>());
    value["options"]["hints"] = json!(["security-key", "client-device", "hybrid"]);
    let mut bytes = serde_json::to_vec(&value).unwrap();
    let total_limit = 128 * 1024;
    assert!(bytes.len() < total_limit);
    let prepared = prepare(&bytes).await.unwrap();
    assert_eq!(prepared.ctap.client_data_hash.as_ref(), &[0x92; 32]);
    assert_eq!(prepared.ctap.allow.len(), 64);
    assert_eq!(prepared.ctap.allow[63].id.as_ref(), &[63; 1024]);
    // JSON whitespace reaches the aggregate boundary without violating field limits.
    bytes.resize(total_limit, b' ');
    assert!(prepare(&bytes).await.is_ok());
    bytes.push(b' ');
    assert!(matches!(prepare(&bytes).await, Err(ErrorName::OperationError)));
}

#[tokio::test] async fn origin_and_public_suffix_validation_precedes_transport() {
    for origin in ["http://login.example.com", "https://other.example.org", "https://login.example.com/path", "https://u:p@login.example.com", "https://login.example.com#private", "https://login.example.com/"] {
        let mut v=input();v["origin"]=origin.into();assert!(prepare_value(&v).await.is_err());
    }
    for rp in ["com", "org", "other.example.com", "https://example.com", "", "example.com:443"] {
        let mut v=input();v["options"]["rpId"]=rp.into();assert!(prepare_value(&v).await.is_err());
    }
}

#[tokio::test] async fn typed_schema_rejects_duplicates_multiple_inputs_and_unknown_fields() {
    let bytes=serde_json::to_vec(&input()).unwrap();
    let mut extra=bytes.clone();extra.extend_from_slice(&bytes);assert!(prepare(&extra).await.is_err());
    let duplicate=String::from_utf8(bytes).unwrap().replacen("\"version\":1", "\"version\":1,\"version\":1",1);
    assert!(prepare(duplicate.as_bytes()).await.is_err());
    for path in ["root","options","descriptor"] {
        let mut v=input();
        match path {"root"=>v["untrusted"]=true.into(),"options"=>v["options"]["mediation"]="conditional".into(),_=>v["options"]["allowCredentials"][0]["secret"]=true.into()};
        assert!(prepare_value(&v).await.is_err());
    }
    assert!(prepare(&vec![b' ';INPUT_LIMIT+1]).await.is_err());
}

#[tokio::test] async fn unknown_extensions_are_not_silently_ignored() {
    for extensions in [json!({"appid":"https://other.test"}),json!({"prf":{}}),json!({"future":false})] {
        let mut v=input();v["options"]["extensions"]=extensions;
        assert!(matches!(prepare_value(&v).await,Err(ErrorName::NotSupportedError)));
    }
    let mut v=input();v["options"]["extensions"]=json!({});assert!(prepare_value(&v).await.is_ok());
}

#[tokio::test] async fn binary_inputs_are_canonical_and_bounded() {
    for value in ["", "AA==", "AB", "private payload", &base64_url::encode(&[0;31]), &base64_url::encode(&[0;33])] {
        let mut v=input();v["clientDataHash"]=value.into();assert!(prepare_value(&v).await.is_err());
    }
    for length in [0, 16385] {
        let mut v=input();v["options"]["challenge"]=base64_url::encode(&vec![0;length]).into();assert!(matches!(prepare_value(&v).await,Err(ErrorName::OperationError)));
    }
    let mut v=input();v["options"]["allowCredentials"][0]["id"]=base64_url::encode(&vec![0;1025]).into();assert!(prepare_value(&v).await.is_err());
    let mut v=input();let descriptor=v["options"]["allowCredentials"][0].clone();v["options"]["allowCredentials"]=json!(vec![descriptor;65]);assert!(prepare_value(&v).await.is_err());
}

#[tokio::test] async fn timeout_and_verification_are_explicit() {
    let mut v=input();v["options"]["timeout"]=300000.into();assert_eq!(prepare_value(&v).await.unwrap().timeout,Duration::from_secs(120));
    v["options"]["timeout"]=0.into();assert!(matches!(prepare_value(&v).await,Err(ErrorName::NotAllowedError)));
    v["options"]["timeout"]=42.into();v["options"]["userVerification"]="discouraged".into();let p=prepare_value(&v).await.unwrap();assert!(!p.require_uv);assert!(p.ctap.options.unwrap().require_user_presence);
    v["options"]["userVerification"]="silent".into();assert!(prepare_value(&v).await.is_err());
}

#[tokio::test] async fn exact_signed_authenticator_bytes_survive_result() {
    let p=prepare_value(&input()).await.unwrap();
    let mut raw=raw_auth();raw[32]|=0x80;
    // Deliberately noncanonical CBOR ordering within an unknown signed extension.
    raw.extend([0xa2,0x61,b'z',0x01,0x61,b'a',0x02]);
    let response=assertion(raw.clone(),Some(vec![0x48;32]),Some(vec![0x88;16]));
    let json=serde_json::to_value(assertion_message(&p,response).unwrap()).unwrap();
    assert_eq!(json["authenticatorData"],base64_url::encode(&raw));
    assert_eq!(json["credentialId"],base64_url::encode(&[0x48;32]));
    assert_eq!(json["signature"],base64_url::encode(&[0xaa;72]));
    assert_eq!(json["userHandle"],base64_url::encode(&[0x88;16]));
    assert!(json.get("userName").is_none());
}

#[tokio::test] async fn missing_or_mismatched_result_binding_fails() {
    let p=prepare_value(&input()).await.unwrap();
    for change in [0,1,2] {
        let mut raw=raw_auth();match change {0=>raw[0]^=1,1=>raw[32]&=!1,_=>raw[32]&=!4};
        assert!(assertion_message(&p,assertion(raw,Some(vec![0x48;32]),None)).is_err());
    }
    assert!(assertion_message(&p,assertion(raw_auth(),Some(vec![0x49;32]),None)).is_err());
    let mut response=assertion(raw_auth(),Some(vec![0x48;32]),None);response.authenticator_data.raw=None;
    assert!(assertion_message(&p,response).is_err());
}

#[tokio::test] async fn exactly_one_allowlisted_id_can_fill_omitted_descriptor() {
    let p=prepare_value(&input()).await.unwrap();
    assert!(assertion_message(&p,assertion(raw_auth(),None,None)).is_ok());
    let mut v=input();v["options"]["allowCredentials"]=json!([]);let p=prepare_value(&v).await.unwrap();
    assert!(assertion_message(&p,assertion(raw_auth(),None,None)).is_err());
    assert!(assertion_message(&p,assertion(raw_auth(),Some(vec![0x48;32]),None)).is_err());
    assert!(assertion_message(&p,assertion(raw_auth(),Some(vec![0x48;32]),Some(vec![1;16]))).is_ok());
}

#[tokio::test] async fn multiple_accounts_require_supported_selection() {
    let p=prepare_value(&input()).await.unwrap();let mut response=assertion(raw_auth(),Some(vec![0x48;32]),None);response.credentials_count=Some(2);
    assert!(matches!(assertion_message(&p,response),Err(ErrorName::NotSupportedError)));
}

#[test] fn qr_is_only_a_bounded_binary_matrix() {
    let value=serde_json::to_value(qr_message("FIDO:/1234567890").unwrap()).unwrap();
    let size=value["size"].as_u64().unwrap() as usize;let modules=value["modules"].as_str().unwrap();
    assert!((21..=177).contains(&size));assert_eq!(modules.len(),size*size);assert!(modules.bytes().all(|c|matches!(c,b'0'|b'1')));
    assert_eq!(value.as_object().unwrap().len(),3);
}

#[tokio::test] async fn input_is_bounded_and_requires_eof() {
    assert!(read_input(&mut &b""[..]).await.is_err());
    assert!(read_input(&mut vec![b' ';INPUT_LIMIT+1].as_slice()).await.is_err());
    assert_eq!(read_input(&mut &b"{}"[..]).await.unwrap(),b"{}");
    let (mut reader,_writer)=tokio::io::duplex(32);
    assert!(tokio::time::timeout(Duration::from_millis(20),read_input(&mut reader)).await.is_err());
}

#[tokio::test] async fn output_has_one_terminal_record_and_no_secret_diagnostics() {
    let mut bytes=Vec::new();
    let mut output=Output::new(&mut bytes);
    output.send(&Message::Error{name:ErrorName::NotAllowedError}).await.unwrap();
    assert!(output.send(&Message::Status{state:"connected"}).await.is_err());
    assert_eq!(bytes,b"{\"type\":\"error\",\"name\":\"NotAllowedError\"}\n");
}

#[tokio::test] async fn output_limits_record_total_and_blocked_reader() {
    let mut bytes=Vec::new();let mut out=Output::new(&mut bytes);
    assert!(out.send(&Message::Qr{size:177,modules:"1".repeat(RECORD_LIMIT)}).await.is_err());
    let record=Message::Qr{size:177,modules:"1".repeat(177*177)};
    let mut count=0;while out.send(&record).await.is_ok(){count+=1;assert!(count<20);}
    assert!(bytes.len()<=OUTPUT_LIMIT);assert!(count>0);
    let (writer,_reader)=tokio::io::duplex(1);let mut blocked=Output::new(writer);
    assert!(tokio::time::timeout(Duration::from_secs(2),blocked.send(&Message::Status{state:"connected"})).await.unwrap().is_err());
}
