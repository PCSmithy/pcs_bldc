//! The State Table: a path-addressed registry of trait-backed signals.
//!
//! One namespace over every piece of state in the sim (state-route-tables.md).
//! Entries are `Box<dyn Signal>` keyed by a canonical name
//! `<sig_type>:<source>:<local>[:<modifier>]`; values flow through the common
//! [`Value`]. Backings so far: `cvar` (a firmware static via DWARF). `vsig`
//! (model fields) and comms land with the model + comms layers.

use crate::backend::Firmware;
use crate::value::{read_scalar, write_scalar, Scalar, Value};
use std::collections::HashMap;

/// A parsed canonical signal key: `<sig_type>:<source>:<local>[:<modifier>]`.
///
/// `:` is the delimiter; `local` (a C/DWARF path for `cvar`) never contains it.
#[derive(Debug, Clone, Copy)]
pub struct SignalKey<'a> {
    pub sig_type: &'a str,
    pub source: &'a str,
    pub local: &'a str,
    pub modifier: Option<&'a str>,
}

impl<'a> SignalKey<'a> {
    pub fn parse(s: &'a str) -> Option<SignalKey<'a>> {
        let mut p = s.splitn(4, ':');
        Some(SignalKey {
            sig_type: p.next()?,
            source: p.next()?,
            local: p.next()?,
            modifier: p.next(),
        })
    }
}

/// A State Table entry backing. Read/write go through `&self` (interior
/// mutability), so the whole table is reachable through `&StateTable` during
/// route propagation (read sources, write destinations).
pub trait Signal {
    fn read(&self) -> Value;
    fn write(&self, v: Value);
}

/// A firmware `static` (or member / array element), reached by a live
/// in-process address resolved from DWARF at registration. Valid while the
/// firmware DLL stays loaded — the [`StateTable`] borrows the [`Firmware`] to
/// enforce that.
struct CvarSignal {
    ptr: *mut u8,
    kind: Scalar,
}

impl Signal for CvarSignal {
    fn read(&self) -> Value {
        // SAFETY: `ptr` is a valid firmware address resolved at registration;
        // firmware is quiescent during framework access (ffi-boundary.md §5).
        unsafe { read_scalar(self.ptr, self.kind) }
    }
    fn write(&self, v: Value) {
        // SAFETY: as above; `write_scalar` type-checks `v` against `kind`.
        unsafe { write_scalar(self.ptr, self.kind, v) }
    }
}

/// The State Table: canonical name → signal. Borrows the firmware backend so
/// `cvar` pointers stay valid for the table's lifetime.
pub struct StateTable<'fw> {
    fw: &'fw Firmware,
    signals: HashMap<String, Box<dyn Signal>>,
}

impl<'fw> StateTable<'fw> {
    pub fn new(fw: &'fw Firmware) -> Self {
        Self {
            fw,
            signals: HashMap::new(),
        }
    }

    /// Register a signal by canonical name. `cvar` resolves its `local` path via
    /// DWARF; `vsig`/comms land with their layers. Idempotent (re-registering
    /// replaces).
    pub fn register(&mut self, name: &str) -> Result<(), String> {
        let key = SignalKey::parse(name).ok_or_else(|| format!("bad signal key: {name:?}"))?;
        let sig: Box<dyn Signal> = match key.sig_type {
            "cvar" => {
                let (ptr, kind) = self
                    .fw
                    .resolve_addr(key.local)
                    .ok_or_else(|| format!("cvar not found in DWARF: {}", key.local))?;
                Box::new(CvarSignal { ptr, kind })
            }
            other => {
                return Err(format!(
                    "unsupported sig_type {other:?} (only `cvar` so far)"
                ))
            }
        };
        self.signals.insert(name.to_string(), sig);
        Ok(())
    }

    /// Read a registered signal. Panics if not registered.
    pub fn read(&self, name: &str) -> Value {
        self.get(name).read()
    }

    /// Write a registered signal (type-checked). Panics if not registered.
    pub fn write(&self, name: &str, v: Value) {
        self.get(name).write(v);
    }

    fn get(&self, name: &str) -> &dyn Signal {
        self.signals
            .get(name)
            .unwrap_or_else(|| panic!("signal not registered: {name:?}"))
            .as_ref()
    }

    pub fn contains(&self, name: &str) -> bool {
        self.signals.contains_key(name)
    }

    pub fn len(&self) -> usize {
        self.signals.len()
    }

    pub fn is_empty(&self) -> bool {
        self.signals.is_empty()
    }
}

#[cfg(test)]
mod tests {
    use super::SignalKey;

    #[test]
    fn parses_cvar_without_modifier() {
        let k = SignalKey::parse("cvar:pcs_bldc:HW_ADC_data.channelData[0].counts[6]").unwrap();
        assert_eq!(k.sig_type, "cvar");
        assert_eq!(k.source, "pcs_bldc");
        assert_eq!(k.local, "HW_ADC_data.channelData[0].counts[6]");
        assert_eq!(k.modifier, None);
    }

    #[test]
    fn parses_comms_with_modifier() {
        let k = SignalKey::parse("spi:encoder:rx:decoded").unwrap();
        assert_eq!(k.sig_type, "spi");
        assert_eq!(k.source, "encoder");
        assert_eq!(k.local, "rx");
        assert_eq!(k.modifier, Some("decoded"));
    }

    #[test]
    fn rejects_too_few_fields() {
        assert!(SignalKey::parse("cvar:onlytwo").is_none());
    }
}

