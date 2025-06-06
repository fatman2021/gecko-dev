/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#[macro_use]
extern crate log;

#[macro_use]
extern crate xpcom;

use authenticator::{
    authenticatorservice::{
        AuthenticatorService, CtapVersion, GetAssertionOptions, MakeCredentialsOptions,
        RegisterArgsCtap2, SignArgsCtap2,
    },
    ctap2::attestation::AttestationStatement,
    ctap2::server::{
        PublicKeyCredentialDescriptor, PublicKeyCredentialParameters, RelyingParty, User,
    },
    errors::{AuthenticatorError, PinError, U2FTokenError},
    statecallback::StateCallback,
    Assertion, Pin, RegisterResult, SignResult, StatusUpdate,
};
use moz_task::RunnableBuilder;
use nserror::{
    nsresult, NS_ERROR_DOM_INVALID_STATE_ERR, NS_ERROR_DOM_NOT_ALLOWED_ERR,
    NS_ERROR_DOM_NOT_SUPPORTED_ERR, NS_ERROR_DOM_OPERATION_ERR, NS_ERROR_DOM_UNKNOWN_ERR,
    NS_ERROR_FAILURE, NS_ERROR_NOT_AVAILABLE, NS_ERROR_NOT_IMPLEMENTED, NS_ERROR_NULL_POINTER,
    NS_OK,
};
use nsstring::{nsACString, nsCString, nsString};
use serde_cbor;
use std::cell::RefCell;
use std::sync::mpsc::{channel, Receiver, RecvError, Sender};
use std::sync::{Arc, Mutex};
use thin_vec::ThinVec;
use xpcom::interfaces::{
    nsICtapRegisterArgs, nsICtapRegisterResult, nsICtapSignArgs, nsICtapSignResult,
    nsIWebAuthnController, nsIWebAuthnTransport,
};
use xpcom::{xpcom_method, RefPtr};

fn make_register_prompt(tid: u64, origin: &str, browsing_context_id: u64) -> String {
    format!(
        r#"{{"is_ctap2":true,"action":"register","tid":{tid},"origin":"{origin}","browsingContextId":{browsing_context_id},"device_selected":true}}"#,
    )
}

fn make_sign_prompt(tid: u64, origin: &str, browsing_context_id: u64) -> String {
    format!(
        r#"{{"is_ctap2":true,"action":"sign","tid":{tid},"origin":"{origin}","browsingContextId":{browsing_context_id},"device_selected":true}}"#,
    )
}

fn make_select_device_prompt(tid: u64, origin: &str, browsing_context_id: u64) -> String {
    format!(
        r#"{{"is_ctap2":true,"action":"select-device","tid":{tid},"origin":"{origin}","browsingContextId":{browsing_context_id}}}"#,
    )
}

fn make_pin_error_prompt(action: &str, tid: u64, origin: &str, browsing_context_id: u64) -> String {
    format!(
        r#"{{"is_ctap2":true,"action":"{action}","tid":{tid},"origin":"{origin}","browsingContextId":{browsing_context_id}}}"#,
    )
}

fn make_pin_required_prompt(
    tid: u64,
    origin: &str,
    browsing_context_id: u64,
    was_invalid: bool,
    retries: i64,
) -> String {
    format!(
        r#"{{"is_ctap2":true,"action":"pin-required","tid":{tid},"origin":"{origin}","browsingContextId":{browsing_context_id},"wasInvalid":{was_invalid},"retriesLeft":{retries}}}"#,
    )
}

fn authrs_to_nserror(e: &AuthenticatorError) -> nsresult {
    match e {
        AuthenticatorError::U2FToken(U2FTokenError::Unknown) => NS_ERROR_DOM_UNKNOWN_ERR,
        AuthenticatorError::U2FToken(U2FTokenError::NotSupported) => NS_ERROR_DOM_NOT_SUPPORTED_ERR,
        AuthenticatorError::U2FToken(U2FTokenError::InvalidState) => NS_ERROR_DOM_INVALID_STATE_ERR,
        AuthenticatorError::U2FToken(U2FTokenError::ConstraintError) => NS_ERROR_DOM_UNKNOWN_ERR,
        AuthenticatorError::U2FToken(U2FTokenError::NotAllowed) => NS_ERROR_DOM_NOT_ALLOWED_ERR,
        AuthenticatorError::PinError(PinError::PinRequired) => NS_ERROR_DOM_OPERATION_ERR,
        AuthenticatorError::PinError(PinError::PinIsTooShort) => NS_ERROR_DOM_UNKNOWN_ERR,
        AuthenticatorError::PinError(PinError::PinIsTooLong(_)) => NS_ERROR_DOM_UNKNOWN_ERR,
        AuthenticatorError::PinError(PinError::InvalidKeyLen) => NS_ERROR_DOM_UNKNOWN_ERR,
        AuthenticatorError::PinError(PinError::InvalidPin(_)) => NS_ERROR_DOM_OPERATION_ERR,
        AuthenticatorError::PinError(PinError::PinAuthBlocked) => NS_ERROR_DOM_OPERATION_ERR,
        AuthenticatorError::PinError(PinError::PinBlocked) => NS_ERROR_DOM_OPERATION_ERR,
        AuthenticatorError::PinError(PinError::PinNotSet) => NS_ERROR_DOM_UNKNOWN_ERR,
        AuthenticatorError::PinError(PinError::Backend(_)) => NS_ERROR_DOM_UNKNOWN_ERR,
        _ => NS_ERROR_DOM_UNKNOWN_ERR,
    }
}

#[xpcom(implement(nsICtapRegisterResult), atomic)]
pub struct CtapRegisterResult {
    result: Result<RegisterResult, AuthenticatorError>,
}

impl CtapRegisterResult {
    xpcom_method!(get_client_data_json => GetClientDataJSON() -> nsACString);
    fn get_client_data_json(&self) -> Result<nsCString, nsresult> {
        match &self.result {
            Ok(RegisterResult::CTAP2(_, client_data)) => {
                return Ok(nsCString::from(client_data.serialized_data.clone()))
            }
            _ => return Err(NS_ERROR_FAILURE),
        }
    }

    xpcom_method!(get_attestation_object => GetAttestationObject() -> ThinVec<u8>);
    fn get_attestation_object(&self) -> Result<ThinVec<u8>, nsresult> {
        let mut out = ThinVec::new();
        if let Ok(RegisterResult::CTAP2(attestation, _)) = &self.result {
            if let Ok(encoded_att_obj) = serde_cbor::to_vec(&attestation) {
                out.extend_from_slice(&encoded_att_obj);
                return Ok(out);
            }
        }
        Err(NS_ERROR_FAILURE)
    }

    xpcom_method!(get_credential_id => GetCredentialId() -> ThinVec<u8>);
    fn get_credential_id(&self) -> Result<ThinVec<u8>, nsresult> {
        let mut out = ThinVec::new();
        if let Ok(RegisterResult::CTAP2(attestation, _)) = &self.result {
            if let Some(credential_data) = &attestation.auth_data.credential_data {
                out.extend(credential_data.credential_id.clone());
                return Ok(out);
            }
        }
        Err(NS_ERROR_FAILURE)
    }

    xpcom_method!(get_status => GetStatus() -> nsresult);
    fn get_status(&self) -> Result<nsresult, nsresult> {
        match &self.result {
            Ok(_) => Ok(NS_OK),
            Err(e) => Err(authrs_to_nserror(e)),
        }
    }
}

#[xpcom(implement(nsICtapSignResult), atomic)]
pub struct CtapSignResult {
    result: Result<Assertion, AuthenticatorError>,
}

impl CtapSignResult {
    xpcom_method!(get_credential_id => GetCredentialId() -> ThinVec<u8>);
    fn get_credential_id(&self) -> Result<ThinVec<u8>, nsresult> {
        let mut out = ThinVec::new();
        if let Ok(assertion) = &self.result {
            if let Some(cred) = &assertion.credentials {
                out.extend_from_slice(&cred.id);
                return Ok(out);
            }
        }
        Err(NS_ERROR_FAILURE)
    }

    xpcom_method!(get_signature => GetSignature() -> ThinVec<u8>);
    fn get_signature(&self) -> Result<ThinVec<u8>, nsresult> {
        let mut out = ThinVec::new();
        if let Ok(assertion) = &self.result {
            out.extend_from_slice(&assertion.signature);
            return Ok(out);
        }
        Err(NS_ERROR_FAILURE)
    }

    xpcom_method!(get_authenticator_data => GetAuthenticatorData() -> ThinVec<u8>);
    fn get_authenticator_data(&self) -> Result<ThinVec<u8>, nsresult> {
        let mut out = ThinVec::new();
        if let Ok(assertion) = &self.result {
            if let Ok(encoded_auth_data) = assertion.auth_data.to_vec() {
                out.extend_from_slice(&encoded_auth_data);
                return Ok(out);
            }
        }
        Err(NS_ERROR_FAILURE)
    }

    xpcom_method!(get_user_handle => GetUserHandle() -> ThinVec<u8>);
    fn get_user_handle(&self) -> Result<ThinVec<u8>, nsresult> {
        let mut out = ThinVec::new();
        if let Ok(assertion) = &self.result {
            if let Some(user) = &assertion.user {
                out.extend_from_slice(&user.id);
                return Ok(out);
            }
        }
        Err(NS_ERROR_FAILURE)
    }

    xpcom_method!(get_user_name => GetUserName() -> nsACString);
    fn get_user_name(&self) -> Result<nsCString, nsresult> {
        if let Ok(assertion) = &self.result {
            if let Some(user) = &assertion.user {
                if let Some(name) = &user.name {
                    return Ok(nsCString::from(name));
                }
            }
        }
        Err(NS_ERROR_NOT_AVAILABLE)
    }

    xpcom_method!(get_rp_id_hash => GetRpIdHash() -> ThinVec<u8>);
    fn get_rp_id_hash(&self) -> Result<ThinVec<u8>, nsresult> {
        // assertion.auth_data.rp_id_hash
        let mut out = ThinVec::new();
        if let Ok(assertion) = &self.result {
            out.extend(assertion.auth_data.rp_id_hash.0.clone());
            return Ok(out);
        }
        Err(NS_ERROR_FAILURE)
    }

    xpcom_method!(get_status => GetStatus() -> nsresult);
    fn get_status(&self) -> Result<nsresult, nsresult> {
        match &self.result {
            Ok(_) => Ok(NS_OK),
            Err(e) => Err(authrs_to_nserror(e)),
        }
    }
}

/// Controller wraps a raw pointer to an nsIWebAuthnController. The AuthrsTransport struct holds a
/// Controller which we need to initialize from the SetController XPCOM method.  Since XPCOM
/// methods take &self, Controller must have interior mutability.
#[derive(Clone)]
struct Controller(RefCell<*const nsIWebAuthnController>);

/// Our implementation of nsIWebAuthnController in WebAuthnController.cpp has the property that its
/// XPCOM methods are safe to call from any thread, hence a raw pointer to an nsIWebAuthnController
/// is Send.
unsafe impl Send for Controller {}

impl Controller {
    fn init(&self, ptr: *const nsIWebAuthnController) -> Result<(), nsresult> {
        self.0.replace(ptr);
        Ok(())
    }

    fn send_prompt(&self, tid: u64, msg: &str) {
        if (*self.0.borrow()).is_null() {
            warn!("Controller not initialized");
            return;
        }
        let notification_str = nsCString::from(msg);
        unsafe {
            (**(self.0.borrow())).SendPromptNotificationPreformatted(tid, &*notification_str);
        }
    }

    fn finish_register(
        &self,
        tid: u64,
        result: Result<RegisterResult, AuthenticatorError>,
    ) -> Result<(), nsresult> {
        if (*self.0.borrow()).is_null() {
            return Err(NS_ERROR_FAILURE);
        }
        let wrapped_result = CtapRegisterResult::allocate(InitCtapRegisterResult { result })
            .query_interface::<nsICtapRegisterResult>()
            .ok_or(NS_ERROR_FAILURE)?;
        unsafe {
            (**(self.0.borrow())).FinishRegister(tid, wrapped_result.coerce());
        }
        Ok(())
    }

    fn finish_sign(
        &self,
        tid: u64,
        result: Result<SignResult, AuthenticatorError>,
    ) -> Result<(), nsresult> {
        if (*self.0.borrow()).is_null() {
            return Err(NS_ERROR_FAILURE);
        }

        // If result is an error, we return a single CtapSignResult that has its status field set
        // to an error. Otherwise we convert the entries of SignResult (= Vec<Assertion>) into
        // CtapSignResults with OK statuses.
        let mut assertions: ThinVec<Option<RefPtr<nsICtapSignResult>>> = ThinVec::new();
        let mut client_data = nsCString::new();
        match result {
            Err(e) => assertions.push(
                CtapSignResult::allocate(InitCtapSignResult { result: Err(e) })
                    .query_interface::<nsICtapSignResult>(),
            ),
            Ok(SignResult::CTAP2(assertion_vec, json)) => {
                client_data = nsCString::from(json.serialized_data);
                for assertion in assertion_vec.0 {
                    assertions.push(
                        CtapSignResult::allocate(InitCtapSignResult {
                            result: Ok(assertion),
                        })
                        .query_interface::<nsICtapSignResult>(),
                    );
                }
            }
            _ => return Err(NS_ERROR_NOT_IMPLEMENTED), // SignResult::CTAP1 shouldn't happen.
        }

        unsafe {
            (**(self.0.borrow())).FinishSign(tid, &mut *client_data, &mut assertions);
        }
        Ok(())
    }
}

enum EventType {
    Register,
    Sign,
}

// The state machine creates a Sender<Pin>/Receiver<Pin> channel in ask_user_for_pin. It passes the
// Sender through status_callback, which stores the Sender in the pin_receiver field of an
// AuthrsTransport. The u64 in PinReceiver is a transaction ID, which the AuthrsTransport uses the
// transaction ID as a consistency check.
type PinReceiver = Option<(u64, Sender<Pin>)>;

fn status_callback(
    status_rx: Receiver<StatusUpdate>,
    tid: u64,
    origin: &String,
    browsing_context_id: u64,
    controller: Controller,
    event_type: EventType,
    pin_receiver: Arc<Mutex<PinReceiver>>, /* Shared with an AuthrsTransport */
) {
    loop {
        match status_rx.recv() {
            Ok(StatusUpdate::DeviceAvailable { dev_info }) => {
                debug!("STATUS: device available: {}", dev_info)
            }
            Ok(StatusUpdate::DeviceUnavailable { dev_info }) => {
                debug!("STATUS: device unavailable: {}", dev_info)
            }
            Ok(StatusUpdate::Success { dev_info }) => {
                debug!("STATUS: success using device: {}", dev_info);
            }
            Ok(StatusUpdate::SelectDeviceNotice) => {
                debug!("STATUS: Please select a device by touching one of them.");
                let notification_str = make_select_device_prompt(tid, origin, browsing_context_id);
                controller.send_prompt(tid, &notification_str);
            }
            Ok(StatusUpdate::DeviceSelected(dev_info)) => {
                debug!("STATUS: Continuing with device: {}", dev_info);
                let notification_str = match event_type {
                    EventType::Register => make_register_prompt(tid, origin, browsing_context_id),
                    EventType::Sign => make_sign_prompt(tid, origin, browsing_context_id),
                };
                controller.send_prompt(tid, &notification_str);
            }
            Ok(StatusUpdate::PinError(error, sender)) => match error {
                PinError::PinRequired => {
                    let guard = pin_receiver.lock();
                    if let Ok(mut entry) = guard {
                        entry.replace((tid, sender));
                    } else {
                        return;
                    }
                    let notification_str =
                        make_pin_required_prompt(tid, origin, browsing_context_id, false, -1);
                    controller.send_prompt(tid, &notification_str);
                    continue;
                }
                PinError::InvalidPin(attempts) => {
                    let guard = pin_receiver.lock();
                    if let Ok(mut entry) = guard {
                        entry.replace((tid, sender));
                    } else {
                        return;
                    }
                    let notification_str = make_pin_required_prompt(
                        tid,
                        origin,
                        browsing_context_id,
                        true,
                        attempts.map_or(-1, |x| x as i64),
                    );
                    controller.send_prompt(tid, &notification_str);
                    continue;
                }
                PinError::PinAuthBlocked => {
                    let notification_str =
                        make_pin_error_prompt("pin-auth-blocked", tid, origin, browsing_context_id);
                    controller.send_prompt(tid, &notification_str);
                }
                PinError::PinBlocked => {
                    let notification_str =
                        make_pin_error_prompt("device-blocked", tid, origin, browsing_context_id);
                    controller.send_prompt(tid, &notification_str);
                }
                e => {
                    warn!("Unexpected error: {:?}", e)
                }
            },
            Err(RecvError) => {
                debug!("STATUS: end");
                return;
            }
        }
    }
}

// AuthrsTransport provides an nsIWebAuthnTransport interface to an AuthenticatorService. This
// allows an nsIWebAuthnController to dispatch MakeCredential and GetAssertion requests to USB HID
// tokens. The AuthrsTransport struct also keeps 1) a pointer to the active nsIWebAuthnController,
// which is used to prompt the user for input and to return results to the controller; and
// 2) a channel through which to receive a pin callback.
#[xpcom(implement(nsIWebAuthnTransport), atomic)]
pub struct AuthrsTransport {
    auth_service: RefCell<AuthenticatorService>, // interior mutable for use in XPCOM methods
    controller: Controller,
    pin_receiver: Arc<Mutex<PinReceiver>>,
}

impl AuthrsTransport {
    xpcom_method!(get_controller => GetController() -> *const nsIWebAuthnController);
    fn get_controller(&self) -> Result<RefPtr<nsIWebAuthnController>, nsresult> {
        Err(NS_ERROR_NOT_IMPLEMENTED)
    }

    xpcom_method!(set_controller => SetController(aController: *const nsIWebAuthnController));
    fn set_controller(&self, controller: *const nsIWebAuthnController) -> Result<(), nsresult> {
        self.controller.init(controller)
    }

    xpcom_method!(pin_callback => PinCallback(aTransactionId: u64, aPin: *const nsACString));
    fn pin_callback(&self, transaction_id: u64, pin: &nsACString) -> Result<(), nsresult> {
        let mut guard = self.pin_receiver.lock().or(Err(NS_ERROR_FAILURE))?;
        match guard.take() {
            // The pin_receiver is single-use.
            Some((tid, channel)) if tid == transaction_id => channel
                .send(Pin::new(&pin.to_string()))
                .or(Err(NS_ERROR_FAILURE)),
            // Either we weren't expecting a pin, or the controller is confused
            // about which transaction is active. Neither is recoverable, so it's
            // OK to drop the PinReceiver here.
            _ => Err(NS_ERROR_FAILURE),
        }
    }

    xpcom_method!(make_credential => MakeCredential(aTid: u64, aBrowsingContextId: u64, aArgs: *const nsICtapRegisterArgs));
    fn make_credential(
        &self,
        tid: u64,
        browsing_context_id: u64,
        args: *const nsICtapRegisterArgs,
    ) -> Result<(), nsresult> {
        if args.is_null() {
            return Err(NS_ERROR_NULL_POINTER);
        }
        let args = unsafe { &*args };

        let mut origin = nsString::new();
        unsafe { args.GetOrigin(&mut *origin) }.to_result()?;

        let mut relying_party_id = nsString::new();
        unsafe { args.GetRpId(&mut *relying_party_id) }.to_result()?;

        let mut challenge = ThinVec::new();
        unsafe { args.GetChallenge(&mut challenge) }.to_result()?;

        let mut client_data_json = nsCString::new();
        unsafe { args.GetClientDataJSON(&mut *client_data_json) }.to_result()?;

        let mut timeout_ms = 0u32;
        unsafe { args.GetTimeoutMS(&mut timeout_ms) }.to_result()?;

        let mut exclude_list = ThinVec::new();
        unsafe { args.GetExcludeList(&mut exclude_list) }.to_result()?;
        let exclude_list = exclude_list
            .iter_mut()
            .map(|id| PublicKeyCredentialDescriptor {
                id: id.to_vec(),
                transports: vec![],
            })
            .collect();

        let mut relying_party_name = nsString::new();
        unsafe { args.GetRpName(&mut *relying_party_name) }.to_result()?;

        let mut user_id = ThinVec::new();
        unsafe { args.GetUserId(&mut user_id) }.to_result()?;

        let mut user_name = nsString::new();
        unsafe { args.GetUserName(&mut *user_name) }.to_result()?;

        let mut user_display_name = nsString::new();
        unsafe { args.GetUserDisplayName(&mut *user_display_name) }.to_result()?;

        let mut cose_algs = ThinVec::new();
        unsafe { args.GetCoseAlgs(&mut cose_algs) }.to_result()?;
        let pub_cred_params = cose_algs
            .iter()
            .map(|alg| PublicKeyCredentialParameters::try_from(*alg).unwrap())
            .collect();

        let mut require_resident_key = false;
        unsafe { args.GetRequireResidentKey(&mut require_resident_key) }.to_result()?;

        let mut user_verification = nsString::new();
        unsafe { args.GetUserVerification(&mut *user_verification) }.to_result()?;
        let require_user_verification = user_verification.eq("required");

        let mut attestation_conveyance_preference = nsString::new();
        unsafe { args.GetAttestationConveyancePreference(&mut *attestation_conveyance_preference) }
            .to_result()?;
        let none_attestation = attestation_conveyance_preference.eq("none");

        // TODO(Bug 1593571) - Add this to the extensions
        // let mut hmac_create_secret = None;
        // let mut maybe_hmac_create_secret = false;
        // match unsafe { args.GetHmacCreateSecret(&mut maybe_hmac_create_secret) }.to_result() {
        //     Ok(_) => hmac_create_secret = Some(maybe_hmac_create_secret),
        //     _ => (),
        // }

        let info = RegisterArgsCtap2 {
            challenge: challenge.to_vec(),
            relying_party: RelyingParty {
                id: relying_party_id.to_string(),
                name: None,
                icon: None,
            },
            origin: origin.to_string(),
            user: User {
                id: user_id.to_vec(),
                icon: None,
                name: Some(user_name.to_string()),
                display_name: None,
            },
            pub_cred_params,
            exclude_list,
            options: MakeCredentialsOptions {
                resident_key: require_resident_key.then_some(true),
                user_verification: require_user_verification.then_some(true),
            },
            extensions: Default::default(),
            pin: None,
        };

        let (status_tx, status_rx) = channel::<StatusUpdate>();
        let pin_receiver = self.pin_receiver.clone();
        let controller = self.controller.clone();
        let status_origin = origin.to_string();
        RunnableBuilder::new(
            "AuthrsTransport::MakeCredential::StatusReceiver",
            move || {
                status_callback(
                    status_rx,
                    tid,
                    &status_origin,
                    browsing_context_id,
                    controller,
                    EventType::Register,
                    pin_receiver,
                )
            },
        )
        .may_block(true)
        .dispatch_background_task()?;

        let controller = self.controller.clone();
        let state_callback = StateCallback::<Result<RegisterResult, AuthenticatorError>>::new(
            Box::new(move |result| {
                let result = match result {
                    Ok(RegisterResult::CTAP1(..)) => Err(AuthenticatorError::VersionMismatch(
                        "AuthrsTransport::MakeCredential",
                        2,
                    )),
                    Ok(RegisterResult::CTAP2(mut attestation_object, client_data)) => {
                        // Tokens always provide attestation, but the user may have asked we not
                        // include the attestation statement in the response.
                        if none_attestation {
                            attestation_object.att_statement = AttestationStatement::None;
                        }
                        Ok(RegisterResult::CTAP2(attestation_object, client_data))
                    }
                    Err(e) => Err(e),
                };
                let _ = controller.finish_register(tid, result);
            }),
        );

        self.auth_service
            .borrow_mut()
            .register(timeout_ms as u64, info.into(), status_tx, state_callback)
            .or(Err(NS_ERROR_FAILURE))
    }

    xpcom_method!(get_assertion => GetAssertion(aTid: u64, aBrowsingContextId: u64, aArgs: *const nsICtapSignArgs));
    fn get_assertion(
        &self,
        tid: u64,
        browsing_context_id: u64,
        args: *const nsICtapSignArgs,
    ) -> Result<(), nsresult> {
        if args.is_null() {
            return Err(NS_ERROR_NULL_POINTER);
        }
        let args = unsafe { &*args };

        let mut origin = nsString::new();
        unsafe { args.GetOrigin(&mut *origin) }.to_result()?;

        let mut relying_party_id = nsString::new();
        unsafe { args.GetRpId(&mut *relying_party_id) }.to_result()?;

        let mut challenge = ThinVec::new();
        unsafe { args.GetChallenge(&mut challenge) }.to_result()?;

        let mut client_data_json = nsCString::new();
        unsafe { args.GetClientDataJSON(&mut *client_data_json) }.to_result()?;

        let mut timeout_ms = 0u32;
        unsafe { args.GetTimeoutMS(&mut timeout_ms) }.to_result()?;

        let mut allow_list = ThinVec::new();
        unsafe { args.GetAllowList(&mut allow_list) }.to_result()?;
        let allow_list: Vec<_> = allow_list
            .iter_mut()
            .map(|id| PublicKeyCredentialDescriptor {
                id: id.to_vec(),
                transports: vec![],
            })
            .collect();

        let mut user_verification = nsString::new();
        unsafe { args.GetUserVerification(&mut *user_verification) }.to_result()?;
        let require_user_verification = user_verification.eq("required");

        let mut alternate_rp_id = None;
        let mut maybe_alternate_rp_id = nsString::new();
        match unsafe { args.GetAppId(&mut *maybe_alternate_rp_id) }.to_result() {
            Ok(_) => alternate_rp_id = Some(maybe_alternate_rp_id.to_string()),
            _ => (),
        }

        let (status_tx, status_rx) = channel::<StatusUpdate>();
        let pin_receiver = self.pin_receiver.clone();
        let controller = self.controller.clone();
        let status_origin = origin.to_string();
        RunnableBuilder::new("AuthrsTransport::GetAssertion::StatusReceiver", move || {
            status_callback(
                status_rx,
                tid,
                &status_origin,
                browsing_context_id,
                controller,
                EventType::Sign,
                pin_receiver,
            )
        })
        .may_block(true)
        .dispatch_background_task()?;

        let uniq_allowed_cred = if allow_list.len() == 1 {
            allow_list.first().cloned()
        } else {
            None
        };

        let controller = self.controller.clone();
        let state_callback =
            StateCallback::<Result<SignResult, AuthenticatorError>>::new(Box::new(move |result| {
                let result = match result {
                    Ok(SignResult::CTAP1(..)) => Err(AuthenticatorError::VersionMismatch(
                        "AuthrsTransport::GetAssertion",
                        2,
                    )),
                    Ok(SignResult::CTAP2(mut assertion_object, client_data)) => {
                        // In CTAP 2.0, but not CTAP 2.1, the assertion object's credential field
                        // "May be omitted if the allowList has exactly one Credential." If we had
                        // a unique allowed credential, then copy its descriptor to the output.
                        if uniq_allowed_cred.is_some() {
                            if let Some(assertion) = assertion_object.0.first_mut() {
                                if assertion.credentials.is_none() {
                                    assertion.credentials = uniq_allowed_cred.clone();
                                }
                            }
                        }
                        Ok(SignResult::CTAP2(assertion_object, client_data))
                    }
                    Err(e) => Err(e),
                };
                let _ = controller.finish_sign(tid, result);
            }));

        let info = SignArgsCtap2 {
            challenge: challenge.to_vec(),
            relying_party_id: relying_party_id.to_string(),
            origin: origin.to_string(),
            allow_list,
            options: GetAssertionOptions {
                user_presence: Some(true),
                user_verification: require_user_verification.then_some(true),
            },
            extensions: Default::default(),
            pin: None,
            alternate_rp_id,
        };

        self.auth_service
            .borrow_mut()
            .sign(timeout_ms as u64, info.into(), status_tx, state_callback)
            .or(Err(NS_ERROR_FAILURE))
    }

    xpcom_method!(cancel => Cancel());
    fn cancel(&self) -> Result<nsresult, nsresult> {
        // We may be waiting for a pin. Drop the channel to release the
        // state machine from `ask_user_for_pin`.
        drop(self.pin_receiver.lock().or(Err(NS_ERROR_FAILURE))?.take());

        match &self.auth_service.borrow_mut().cancel() {
            Ok(_) => Ok(NS_OK),
            Err(e) => Err(authrs_to_nserror(e)),
        }
    }
}

#[no_mangle]
pub extern "C" fn authrs_transport_constructor(
    result: *mut *const nsIWebAuthnTransport,
) -> nsresult {
    let mut auth_service = match AuthenticatorService::new(CtapVersion::CTAP2) {
        Ok(auth_service) => auth_service,
        _ => return NS_ERROR_FAILURE,
    };
    auth_service.add_detected_transports();
    let wrapper = AuthrsTransport::allocate(InitAuthrsTransport {
        auth_service: RefCell::new(auth_service),
        controller: Controller(RefCell::new(std::ptr::null())),
        pin_receiver: Arc::new(Mutex::new(None)),
    });
    unsafe {
        RefPtr::new(wrapper.coerce::<nsIWebAuthnTransport>()).forget(&mut *result);
    }
    NS_OK
}
