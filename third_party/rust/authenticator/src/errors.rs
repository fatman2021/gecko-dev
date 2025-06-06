/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

pub use crate::ctap2::commands::{client_pin::PinError, CommandError};
pub use crate::transport::errors::HIDError;
use std::fmt;
use std::io;
use std::sync::mpsc;

// This composite error type is patterned from Phil Daniels' blog:
// https://www.philipdaniels.com/blog/2019/defining-rust-error-types/

#[derive(Debug)]
pub enum UnsupportedOption {
    MaxPinLength,
    HmacSecret,
}

#[derive(Debug)]
pub enum AuthenticatorError {
    // Errors from external libraries...
    Io(io::Error),
    // Errors raised by us...
    InvalidRelyingPartyInput,
    NoConfiguredTransports,
    Platform,
    InternalError(String),
    U2FToken(U2FTokenError),
    Custom(String),
    VersionMismatch(&'static str, u32),
    HIDError(HIDError),
    CryptoError,
    PinError(PinError),
    UnsupportedOption(UnsupportedOption),
}

impl AuthenticatorError {
    pub fn as_u2f_errorcode(&self) -> u8 {
        match *self {
            AuthenticatorError::U2FToken(ref err) => *err as u8,
            // TODO: This is somewhat ugly, as we hardcode the error code here, instead of using the
            // const defined in `u2fhid-capi.h`, which we should.
            AuthenticatorError::PinError(PinError::PinRequired) => 6u8,
            AuthenticatorError::PinError(PinError::InvalidPin(_)) => 7u8,
            AuthenticatorError::PinError(PinError::PinAuthBlocked) => 8u8,
            AuthenticatorError::PinError(PinError::PinBlocked) => 9u8,
            _ => U2FTokenError::Unknown as u8,
        }
    }
}

impl std::error::Error for AuthenticatorError {}

impl fmt::Display for AuthenticatorError {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        match *self {
            AuthenticatorError::Io(ref err) => err.fmt(f),
            AuthenticatorError::InvalidRelyingPartyInput => {
                write!(f, "invalid input from relying party")
            }
            AuthenticatorError::NoConfiguredTransports => write!(
                f,
                "no transports were configured in the authenticator service"
            ),
            AuthenticatorError::Platform => write!(f, "unknown platform error"),
            AuthenticatorError::InternalError(ref err) => write!(f, "internal error: {err}"),
            AuthenticatorError::U2FToken(ref err) => {
                write!(f, "A u2f token error occurred {err:?}")
            }
            AuthenticatorError::Custom(ref err) => write!(f, "A custom error occurred {err:?}"),
            AuthenticatorError::VersionMismatch(manager, version) => write!(
                f,
                "{manager} expected arguments of version CTAP{version}"
            ),
            AuthenticatorError::HIDError(ref e) => write!(f, "Device error: {e}"),
            AuthenticatorError::CryptoError => {
                write!(f, "The cryptography implementation encountered an error")
            }
            AuthenticatorError::PinError(ref e) => write!(f, "PIN Error: {e}"),
            AuthenticatorError::UnsupportedOption(ref e) => {
                write!(f, "Unsupported option: {e:?}")
            }
        }
    }
}

impl From<io::Error> for AuthenticatorError {
    fn from(err: io::Error) -> AuthenticatorError {
        AuthenticatorError::Io(err)
    }
}

impl From<HIDError> for AuthenticatorError {
    fn from(err: HIDError) -> AuthenticatorError {
        AuthenticatorError::HIDError(err)
    }
}

impl From<CommandError> for AuthenticatorError {
    fn from(err: CommandError) -> AuthenticatorError {
        AuthenticatorError::HIDError(HIDError::Command(err))
    }
}

impl<T> From<mpsc::SendError<T>> for AuthenticatorError {
    fn from(err: mpsc::SendError<T>) -> AuthenticatorError {
        AuthenticatorError::InternalError(err.to_string())
    }
}

#[derive(Clone, Copy, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub enum U2FTokenError {
    Unknown = 1,
    NotSupported = 2,
    InvalidState = 3,
    ConstraintError = 4,
    NotAllowed = 5,
}

impl U2FTokenError {
    fn as_str(&self) -> &str {
        match *self {
            U2FTokenError::Unknown => "unknown",
            U2FTokenError::NotSupported => "not supported",
            U2FTokenError::InvalidState => "invalid state",
            U2FTokenError::ConstraintError => "constraint error",
            U2FTokenError::NotAllowed => "not allowed",
        }
    }
}

impl std::fmt::Display for U2FTokenError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}", self.as_str())
    }
}

impl std::error::Error for U2FTokenError {}
