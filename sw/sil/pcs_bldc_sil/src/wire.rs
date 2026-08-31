//! Wire-level helpers for the board's framed protocol (fw~conn_proto_002).
//! The pure codec lives in the shared [`pcs_wire`] crate (also consumed by
//! the desktop app); this module re-exports it and adds the SIL-only sim USB
//! capture reader.

use voyant::Firmware;

pub use pcs_wire::*;

/// Read the raw sim USB TX capture buffer (txLen + tx[] bytes) by DWARF.
pub fn read_tx_capture(fw: &Firmware) -> Vec<u8> {
    let len = fw.read_cvar("HW_USB_sim_data.txLen").as_u64().unwrap_or(0);
    let mut bytes = Vec::with_capacity(len as usize);
    for i in 0..len {
        let b = fw
            .read_cvar(&format!("HW_USB_sim_data.tx[{i}]"))
            .as_u64()
            .unwrap_or(0) as u8;
        bytes.push(b);
    }
    bytes
}
