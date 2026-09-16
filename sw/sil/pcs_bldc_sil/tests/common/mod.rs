//! Shared helpers for the `tests/*.rs` scenarios: world setup, the DWARF
//! read/write accessors every white-box suite needs, and the driver status
//! tables. Each integration test is its own crate, so this module is compiled
//! once per binary and most of it is unused in any single one.
#![allow(dead_code)]

use pcs_bldc_sil::board::{
    fault_latched, ALIGN_DWELL_MS, DIAL, GATE_BRINGUP_MS, GPIO_LEVEL_HIGH, GPIO_LEVEL_LOW,
    INPUT_LEVEL_PB10, MOTOR,
};
use pcs_bldc_sil::wire::frame;
use pcs_bldc_sil::{cid, vid, Sil, SilOptions, SOURCE};
use voyant::Value;

/// The PWM period at 20 kHz — the control-rate grid a board world steps on.
pub const GRID_US: u64 = 50;

/// The kernel-tick handler the fiber port registers with the interrupt table at
/// scheduler start. Shared so the literal stays in sync across the suites.
pub const SYSTICK_ISR: &str = "vSilSysTickHandler";

/// A booted world: firmware loaded, added as a member, `ms` of run behind it.
pub fn booted(ms: u64) -> Sil {
    let mut sim = Sil::new();
    let fwm = sim.load_firmware(SOURCE);
    sim.add_member(fwm);
    sim.run_for_ms(ms);
    sim
}

/// An unsigned firmware field — a count, a register byte, a bit mask.
pub fn u64_at(sim: &Sil, path: &str) -> u64 {
    sim.fw()
        .read_cvar(path)
        .as_u64()
        .unwrap_or_else(|| panic!("{path} reads as an unsigned value"))
}

/// A floating-point firmware field — a decoded engineering value.
pub fn f64_at(sim: &Sil, path: &str) -> f64 {
    sim.fw()
        .read_cvar(path)
        .as_f64()
        .unwrap_or_else(|| panic!("{path} reads as a floating-point value"))
}

/// One of the plant's own signals, the physical truth a firmware reading is
/// measured against.
pub fn plant(sim: &Sil, local: &str) -> f64 {
    sim.read_f64(&vid(MOTOR, local))
}

/// Press and release the mode button, then wait out the double-tap window.
pub fn tap_button(sim: &mut Sil) {
    sim.write(&cid(INPUT_LEVEL_PB10), GPIO_LEVEL_LOW).expect("button press");
    sim.run_for_ms(30);
    sim.write(&cid(INPUT_LEVEL_PB10), GPIO_LEVEL_HIGH).expect("button release");
    sim.run_for_ms(30);
    sim.run_for_ms(320); // APP_USERCONTROLS_DOUBLE_TAP_WINDOW_MS plus slack
}

/// Bring the gate driver up, align, and dial the shaft to a steady spin — the
/// operating point the current-sense suites measure against.
pub fn spin_up(sim: &mut Sil) {
    sim.run_for_ms(GATE_BRINGUP_MS);
    tap_button(sim);
    sim.run_for_ms(ALIGN_DWELL_MS + 100);
    assert!(
        sim.read_bool(&cid("app_motorControl_data.channels[0].isAligned")),
        "alignment latched before the spin"
    );
    let mut deg = 0.0;
    while deg < 90.0 {
        deg += 10.0;
        sim.write(&vid(DIAL, "angle[deg]"), deg).expect("turn the dial");
        sim.run_for_ms(20);
    }
    sim.run_for_ms(300);
    let velocity = plant(sim, "velocity");
    assert!(velocity.abs() > 2.0, "the shaft is spinning, got {velocity:.2} rad/s");
    assert!(!fault_latched(sim), "no fault through the spin-up");
}

/// A `bool` firmware field.
pub fn bool_at(sim: &Sil, path: &str) -> bool {
    match sim.fw().read_cvar(path) {
        Value::Bool(b) => b,
        other => panic!("{path} reads as a bool, got {other:?}"),
    }
}

/// Write a `bool` firmware field — the drivers' fault knobs and world state.
pub fn set_bool(sim: &Sil, path: &str, value: bool) {
    sim.fw().write_cvar(path, &Value::Bool(value));
}

/// Write a `uint32_t` firmware field.
pub fn set_u32(sim: &Sil, path: &str, value: u32) {
    sim.fw().write_cvar(path, &Value::U32(value));
}

/// One value of a driver's status enum. Enumerator names do not resolve in this
/// build (`docs/sil/backlog.md`), so a status matches by name OR by the DWARF
/// reader's `<ordinal>` placeholder. The ordinals below mirror the C enums
/// positionally: an appended enumerator is safe, a reorder is silent — delete the
/// tables and the ordinal arm once the backend resolves enumerator names.
pub struct Status {
    pub name: &'static str,
    pub ordinal: i64,
}

/// `HW_ADC_conversionStatus_E` — a pass that stored every enabled input.
pub const ADC_OK: Status = Status {
    name: "HW_ADC_CONVERSION_STATUS_OK",
    ordinal: 2,
};
/// `HW_ADC_conversionStatus_E` — a DMA regular pass in flight, its completion
/// not yet dispatched.
pub const ADC_BUSY: Status = Status {
    name: "HW_ADC_CONVERSION_STATUS_BUSY",
    ordinal: 1,
};
/// `HW_ADC_conversionStatus_E` — a stalled channel's pass, the sim stand-in for a
/// poll timeout or a lost DMA completion.
pub const ADC_FAULT: Status = Status {
    name: "HW_ADC_CONVERSION_STATUS_FAULT",
    ordinal: 3,
};

/// `HW_SPI_status_E` — a successful transfer.
pub const SPI_COMPLETE: Status = Status {
    name: "HW_SPI_STATUS_COMPLETE",
    ordinal: 2,
};
/// `HW_SPI_status_E` — what a stalled or force-errored transfer leaves behind.
pub const SPI_ERROR: Status = Status {
    name: "HW_SPI_STATUS_ERROR",
    ordinal: 3,
};

/// `HW_DMA_status_E` — a channel with no transfer since init.
pub const DMA_IDLE: Status = Status {
    name: "HW_DMA_STATUS_IDLE",
    ordinal: 0,
};
/// `HW_DMA_status_E` — the status a successful completion lands.
pub const DMA_COMPLETE: Status = Status {
    name: "HW_DMA_STATUS_COMPLETE",
    ordinal: 2,
};
/// `HW_DMA_status_E` — the status a force-errored completion lands.
pub const DMA_ERROR: Status = Status {
    name: "HW_DMA_STATUS_ERROR",
    ordinal: 3,
};

/// Assert the status enum at `path`, read out of firmware memory past the historian.
pub fn assert_status(sim: &Sil, path: &str, want: &Status, why: &str) {
    let got = match sim.fw().read_cvar(path) {
        Value::Enum(name) => name,
        other => panic!("{path} reads as an enum, got {other:?}"),
    };
    assert!(
        (got == want.name) || (got == format!("<{}>", want.ordinal)),
        "{why}: {path} is {got}, expected {}",
        want.name
    );
}

// --- protocol scenarios ----------------------------------------------------

/// How many `rx[]` elements the protocol suites register for command writes;
/// must cover the longest injected frame.
pub const RX_REG: usize = 128;

/// A world whose firmware is booted with the host connected and the sim USB
/// capture drained: the starting point of every protocol scenario.
pub fn connected_world(options: SilOptions) -> Sil {
    let mut sim = options.build();
    let mut fwm = sim.load_firmware(SOURCE);
    for i in 0..RX_REG {
        fwm.register_cvar_in_state_table(&format!("HW_USB_sim_data.rx[{i}]"));
    }
    sim.add_member(fwm);

    sim.write(&cid("HW_USB_sim_data.connected"), true)
        .expect("write connected");
    for _ in 0..3 {
        sim.step().expect("engine step");
    }
    let _ = drain_tx(&mut sim);
    sim
}

/// Frame one envelope and place it in the sim USB RX ring.
pub fn inject(sim: &mut Sil, envelope_bytes: &[u8]) {
    let wire = frame(envelope_bytes);
    assert!(
        wire.len() <= RX_REG,
        "injected frame ({} B) exceeds the {} registered rx elements",
        wire.len(),
        RX_REG
    );
    for (i, &byte) in wire.iter().enumerate() {
        sim.write(&cid(&format!("HW_USB_sim_data.rx[{i}]")), u32::from(byte))
            .expect("write rx byte");
    }
    sim.write(&cid("HW_USB_sim_data.rxHead"), 0u32)
        .expect("write rxHead");
    sim.write(&cid("HW_USB_sim_data.rxTail"), wire.len() as u32)
        .expect("write rxTail");
}

/// Read the sim USB TX capture and zero it, so the next step sees a drained
/// transport — the host end of the link the board writes into.
pub fn drain_tx(sim: &mut Sil) -> Vec<u8> {
    let bytes = pcs_bldc_sil::wire::read_tx_capture(&sim.fw());
    sim.write(&cid("HW_USB_sim_data.txLen"), 0u32)
        .expect("drain tx capture");
    bytes
}

/// Hand-rolled proto3 codec for the board protocol: the SIL suites build and
/// read envelopes without a schema binding in the framework crate.
pub mod proto {
    /// `shared.Envelope` oneof field numbers.
    pub const F_RESPONSE: u32 = 3;
    pub const F_WATCH_REQUEST: u32 = 30;
    pub const F_TRACE_STATUS: u32 = 31;
    pub const F_SAMPLES: u32 = 33;
    pub const F_READ_REQUEST: u32 = 34;
    pub const F_READ_REPLY: u32 = 35;
    pub const F_WRITE_REQUEST: u32 = 36;

    pub fn put_varint(mut v: u64, out: &mut Vec<u8>) {
        loop {
            let byte = (v & 0x7F) as u8;
            v >>= 7;
            if v != 0 {
                out.push(byte | 0x80);
            } else {
                out.push(byte);
                break;
            }
        }
    }

    pub fn get_varint(bytes: &[u8], i: &mut usize) -> Option<u64> {
        let mut v = 0u64;
        let mut shift = 0u32;
        loop {
            let byte = *bytes.get(*i)?;
            *i += 1;
            v |= u64::from(byte & 0x7F) << shift;
            if byte & 0x80 == 0 {
                return Some(v);
            }
            shift += 7;
        }
    }

    /// Field key for a length-delimited (wire type 2) field.
    fn put_len_key(field: u32, out: &mut Vec<u8>) {
        put_varint(u64::from((field << 3) | 2), out);
    }

    pub fn envelope(request_id: u64, payload_field: u32, payload: &[u8]) -> Vec<u8> {
        let mut env = Vec::new();
        if request_id != 0 {
            env.push(0x08);
            put_varint(request_id, &mut env);
        }
        put_len_key(payload_field, &mut env);
        put_varint(payload.len() as u64, &mut env);
        env.extend_from_slice(payload);
        env
    }

    /// Parse an Envelope payload into (request_id, payload field number, bytes).
    pub fn parse_envelope(payload: &[u8]) -> Option<(u64, u32, Vec<u8>)> {
        let mut i = 0usize;
        let mut request_id = 0u64;
        let mut key = get_varint(payload, &mut i)?;
        if key == 0x08 {
            request_id = get_varint(payload, &mut i)?;
            key = get_varint(payload, &mut i)?;
        }
        if key & 7 != 2 {
            return None;
        }
        let field = (key >> 3) as u32;
        let len = get_varint(payload, &mut i)? as usize;
        let bytes = payload.get(i..i + len)?.to_vec();
        Some((request_id, field, bytes))
    }

    /// Collect a submessage's scalar varint and bytes fields by field number.
    pub fn parse_fields(msg: &[u8]) -> Vec<(u32, u64, Vec<u8>)> {
        let mut out = Vec::new();
        let mut i = 0usize;
        while i < msg.len() {
            let Some(key) = get_varint(msg, &mut i) else { break };
            let field = (key >> 3) as u32;
            match key & 7 {
                0 => {
                    let Some(v) = get_varint(msg, &mut i) else { break };
                    out.push((field, v, Vec::new()));
                }
                2 => {
                    let Some(len) = get_varint(msg, &mut i) else { break };
                    let Some(bytes) = msg.get(i..i + len as usize) else { break };
                    i += len as usize;
                    out.push((field, 0, bytes.to_vec()));
                }
                _ => break,
            }
        }
        out
    }

    pub fn field_varint(fields: &[(u32, u64, Vec<u8>)], field: u32) -> u64 {
        fields
            .iter()
            .find(|(f, _, _)| *f == field)
            .map(|(_, v, _)| *v)
            .unwrap_or(0)
    }

    pub fn field_bytes(fields: &[(u32, u64, Vec<u8>)], field: u32) -> &[u8] {
        fields
            .iter()
            .find(|(f, _, _)| *f == field)
            .map(|(_, _, b)| b.as_slice())
            .unwrap_or(&[])
    }

    /// A WatchRequest over (address, size, period_cycles) triples.
    pub fn watch_request(request_id: u64, watches: &[(u32, u32, u32)]) -> Vec<u8> {
        let mut wr = Vec::new();
        for &(address, size, period_cycles) in watches {
            let mut w = Vec::new();
            w.push(0x08);
            put_varint(u64::from(address), &mut w);
            w.push(0x10);
            put_varint(u64::from(size), &mut w);
            w.push(0x18);
            put_varint(u64::from(period_cycles), &mut w);
            wr.push(0x0A);
            put_varint(w.len() as u64, &mut wr);
            wr.extend_from_slice(&w);
        }
        envelope(request_id, F_WATCH_REQUEST, &wr)
    }

    pub fn read_request(request_id: u64, address: u32, size: u32) -> Vec<u8> {
        let mut msg = Vec::new();
        msg.push(0x08);
        put_varint(u64::from(address), &mut msg);
        msg.push(0x10);
        put_varint(u64::from(size), &mut msg);
        envelope(request_id, F_READ_REQUEST, &msg)
    }

    pub fn write_request(request_id: u64, address: u32, data: &[u8]) -> Vec<u8> {
        let mut msg = Vec::new();
        msg.push(0x08);
        put_varint(u64::from(address), &mut msg);
        msg.push(0x12);
        put_varint(data.len() as u64, &mut msg);
        msg.extend_from_slice(data);
        envelope(request_id, F_WRITE_REQUEST, &msg)
    }

    /// One decoded `trace.Samples`: a run of one group's records.
    #[derive(Debug, Clone)]
    pub struct Samples {
        pub period_cycles: u32,
        pub first_cycle: u32,
        pub count: u32,
        pub data: Vec<u8>,
    }

    impl Samples {
        /// Cycle index of record `k`, which sits `k * period_cycles` on.
        pub fn cycle_of(&self, k: u32) -> u32 {
            self.first_cycle + (k * self.period_cycles)
        }

        /// Record `k`'s bytes.
        pub fn record(&self, k: u32) -> &[u8] {
            let size = self.data.len() / (self.count as usize);
            let start = (k as usize) * size;
            &self.data[start..start + size]
        }
    }

    pub fn samples(msg: &[u8]) -> Samples {
        let fields = parse_fields(msg);
        Samples {
            period_cycles: field_varint(&fields, 3) as u32,
            first_cycle: field_varint(&fields, 1) as u32,
            count: field_varint(&fields, 4) as u32,
            data: field_bytes(&fields, 2).to_vec(),
        }
    }
}
