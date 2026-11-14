//! Shared helpers for the `tests/*.rs` scenarios and the characterisation
//! examples: world setup, the DWARF read/write accessors every white-box suite
//! needs, the protocol exchange over the sim USB, and the driver status tables.
//! Each integration test is its own crate, so this module is compiled once per
//! binary and most of it is unused in any single one.
#![allow(dead_code)]

use pcs_bldc_sil::board::{
    board_from, fault_latched, align_dwell_ms, DIAL, GATE_BRINGUP_MS, GPIO_LEVEL_HIGH, GPIO_LEVEL_LOW,
    INPUT_LEVEL_PB10, MOTOR, USB_RX_REG,
};
use pcs_bldc_sil::wire::{frame, Deframer};
use pcs_bldc_sil::{cid, vid, CurrentSenseParams, MotorParams, Sil, SilOptions, SOURCE};
use std::f64::consts::{PI, TAU};
use voyant::{Firmware, Value};

pub use proto::Mode;
use proto::{Envelope, Payload};

/// The PWM period at 20 kHz — the control-rate grid a board world steps on.
pub const GRID_US: u64 = 50;

/// The kernel-tick handler the fiber port registers with the interrupt table at
/// scheduler start. Shared so the literal stays in sync across the suites.
pub const SYSTICK_ISR: &str = "vSilSysTickHandler";

/// Channel 0 of the motor-control application and the rotor estimator: their
/// state and their configuration.
pub const MC: &str = "app_motorControl_data.channels[0]";
pub const MC_CONFIG: &str = "app_motorControl_channelConfig[0]";
pub const EST: &str = "app_rotorEstimation_data.channels[0]";
pub const EST_CONFIG: &str = "app_rotorEstimation_channelConfig[0]";

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
    f64_in(&sim.fw(), path)
}

/// A floating-point field of a loaded image, before any world runs it.
pub fn f64_in(fw: &Firmware, path: &str) -> f64 {
    fw.read_cvar(path)
        .as_f64()
        .unwrap_or_else(|| panic!("{path} reads as a floating-point value"))
}

/// An unsigned field of a loaded image, before any world runs it.
pub fn u64_in(fw: &Firmware, path: &str) -> u64 {
    fw.read_cvar(path)
        .as_u64()
        .unwrap_or_else(|| panic!("{path} reads as an unsigned value"))
}

/// A field of channel 0's motor-control state.
pub fn mc(sim: &Sil, field: &str) -> f64 {
    f64_at(sim, &format!("{MC}.{field}"))
}

/// A field of channel 0's motor-control configuration.
pub fn mc_config(sim: &Sil, field: &str) -> f64 {
    f64_at(sim, &format!("{MC_CONFIG}.{field}"))
}

/// The rotor estimator's configured inertia, kg·m².
pub fn configured_inertia(fw: &Firmware) -> f64 {
    f64_in(fw, &format!("{EST_CONFIG}.inertia_kgm2"))
}

/// One of the plant's own signals, the physical truth a firmware reading is
/// measured against.
pub fn plant(sim: &Sil, local: &str) -> f64 {
    sim.read_f64(&vid(MOTOR, local))
}

/// The plant's shaft angle unwrapped across the seam, from zero at construction.
pub struct Unwrapped {
    last: f64,
    total: f64,
}

impl Unwrapped {
    pub fn new(sim: &Sil) -> Self {
        Self { last: plant(sim, "angle"), total: 0.0 }
    }

    /// Fold in the plant's current angle; the travel since construction.
    pub fn update(&mut self, sim: &Sil) -> f64 {
        let angle = plant(sim, "angle");
        let delta = (angle - self.last + PI).rem_euclid(TAU) - PI;
        self.last = angle;
        self.total += delta;
        self.total
    }
}

/// Press and release the mode button, then wait out the double-tap window.
pub fn tap_button(sim: &mut Sil) {
    sim.write(&cid(INPUT_LEVEL_PB10), GPIO_LEVEL_LOW).expect("button press");
    sim.run_for_ms(30);
    sim.write(&cid(INPUT_LEVEL_PB10), GPIO_LEVEL_HIGH).expect("button release");
    sim.run_for_ms(30);
    sim.run_for_ms(320); // APP_USERCONTROLS_DOUBLE_TAP_WINDOW_MS plus slack
}

/// `board.Mode` values, as `board.proto` numbers them.
pub const MODE_OFF: Mode = Mode::Off;
pub const MODE_SIX_STEP_TRAP: Mode = Mode::SixStepTrap;
pub const MODE_SINUSOIDAL: Mode = Mode::Sinusoidal;
pub const MODE_FOC_TORQUE: Mode = Mode::FocTorque;
pub const MODE_FOC_VELOCITY: Mode = Mode::FocVelocity;
pub const MODE_FOC_POSITION: Mode = Mode::FocPosition;

/// Select a commutation method over the protocol: a `Command` naming the method
/// selects (and enables), one naming `MODE_OFF` disables with the selection kept,
/// so the next button enable runs it. The board world's USB comes up connected.
pub fn select_method(sim: &mut Sil, method: Mode) {
    sim.write(&cid("HW_USB_sim_data.connected"), true)
        .expect("connect the host");
    for (id, mode) in [(1u32, method), (2u32, MODE_OFF)] {
        inject(sim, &proto::envelope(id, proto::command(mode, vec![])));
        sim.run_for_ms(5);
        let _ = drain_tx(sim);
    }
}

/// The board world on the 50 us grid with the gate up and `mode` selected,
/// enabled by a button tap and aligned, on a plant chosen from the loaded image
/// and a sense front end of `sense`.
pub fn enabled_world_from(mode: Mode, plant: impl FnOnce(&Firmware) -> MotorParams, sense: CurrentSenseParams) -> Sil {
    let mut sim = board_from(SilOptions::default().grid_us(GRID_US), 0.0, plant, sense).sim;
    sim.run_for_ms(GATE_BRINGUP_MS);
    select_method(&mut sim, mode);
    tap_button(&mut sim);
    sim.run_for_ms(align_dwell_ms(&sim) + 100);
    assert!(sim.read_bool(&cid(&format!("{MC}.isAligned"))), "aligned through the enable");
    assert!(!fault_latched(&sim), "no fault through the enable");
    sim
}

/// [`enabled_world_from`] on the plant `motor` and the board's sense front end.
pub fn enabled_world(mode: Mode, motor: MotorParams) -> Sil {
    enabled_world_from(mode, |_| motor, CurrentSenseParams::default())
}

/// Bring the gate driver up, select six-step, align, and dial the shaft to a
/// steady spin — the operating point the current-sense suites measure against.
pub fn spin_up(sim: &mut Sil) {
    sim.run_for_ms(GATE_BRINGUP_MS);
    select_method(sim, MODE_SIX_STEP_TRAP);
    tap_button(sim);
    sim.run_for_ms(align_dwell_ms(sim) + 100);
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
pub const RX_REG: usize = USB_RX_REG;

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

/// Place raw wire bytes in the sim USB RX ring: through the State Table for the
/// registered `rx[]` elements, straight into firmware memory past them.
pub fn inject_wire(sim: &mut Sil, wire: &[u8]) {
    for (i, &byte) in wire.iter().enumerate() {
        let path = format!("HW_USB_sim_data.rx[{i}]");
        if i < RX_REG {
            sim.write(&cid(&path), u32::from(byte)).expect("write rx byte");
        } else {
            sim.fw().write_cvar(&path, &Value::U32(u32::from(byte)));
        }
    }
    sim.write(&cid("HW_USB_sim_data.rxHead"), 0u32)
        .expect("write rxHead");
    sim.write(&cid("HW_USB_sim_data.rxTail"), wire.len() as u32)
        .expect("write rxTail");
}

/// Frame one envelope and place it in the sim USB RX ring.
pub fn inject(sim: &mut Sil, envelope_bytes: &[u8]) {
    inject_wire(sim, &frame(envelope_bytes));
}

/// Read the sim USB TX capture and zero it, so the next step sees a drained
/// transport — the host end of the link the board writes into.
pub fn drain_tx(sim: &mut Sil) -> Vec<u8> {
    let bytes = pcs_bldc_sil::wire::read_tx_capture(&sim.fw());
    sim.write(&cid("HW_USB_sim_data.txLen"), 0u32)
        .expect("drain tx capture");
    bytes
}

/// The host end of the link: drains the sim USB capture into a deframer and
/// decodes every envelope a drain completes.
#[derive(Default)]
pub struct Host {
    deframer: Deframer,
}

impl Host {
    pub fn new() -> Self {
        Self::default()
    }

    /// Every envelope the board sent since the last poll.
    pub fn poll(&mut self, sim: &mut Sil) -> Vec<Envelope> {
        let capture = drain_tx(sim);
        self.deframer
            .push(&capture)
            .iter()
            .filter_map(|payload| proto::decode(payload))
            .collect()
    }

    /// Step `steps` engine steps, polling after each; every envelope received.
    pub fn run_steps(&mut self, sim: &mut Sil, steps: u32) -> Vec<Envelope> {
        let mut envelopes = Vec::new();
        for _ in 0..steps {
            sim.step().expect("engine step");
            envelopes.extend(self.poll(sim));
        }
        envelopes
    }

    /// The reply to `id` among the envelopes the next poll brings, if there.
    pub fn reply_to(&mut self, sim: &mut Sil, id: u32) -> Option<Payload> {
        self.poll(sim)
            .into_iter()
            .find(|env| env.request_id == id)
            .and_then(|env| env.payload)
    }
}

/// The reply to request `id` among `envelopes`, if there.
pub fn reply_in(envelopes: &[Envelope], id: u32) -> Option<&Payload> {
    envelopes
        .iter()
        .find(|env| env.request_id == id)
        .and_then(|env| env.payload.as_ref())
}

/// Send `payload` as request `id` and run a millisecond at a time until its
/// reply: the reply payload and the milliseconds it took.
pub fn exchange(sim: &mut Sil, id: u32, payload: Payload) -> (Payload, u32) {
    inject(sim, &proto::envelope(id, payload));
    let mut host = Host::new();
    for ms in 1..=50u32 {
        sim.run_for_ms(1);
        if let Some(reply) = host.reply_to(sim, id) {
            return (reply, ms);
        }
    }
    panic!("no reply to request {id} within 50 ms");
}

/// A `Response` verdict's acceptance; any other reply fails the check.
pub fn accepted(reply: &Payload) -> bool {
    match reply {
        Payload::Response(r) => r.accepted,
        other => panic!("a Response verdict, got {other:?}"),
    }
}

/// A rejected `Response`'s cause.
pub fn cause(reply: &Payload) -> String {
    match reply {
        Payload::Response(r) => r.cause.clone(),
        other => panic!("a Response verdict, got {other:?}"),
    }
}

/// The board protocol's prost bindings (`pcs_proto`) and the request builders
/// the suites send.
pub mod proto {
    pub use pcs_proto::board::{self, Mode};
    pub use pcs_proto::shared::{envelope::Payload, Envelope};
    pub use pcs_proto::trace;
    use prost::Message;

    use board::request::Command as BoardCommand;
    use board::setpoint::Target;

    /// One envelope's wire bytes, unframed.
    pub fn envelope(request_id: u32, payload: Payload) -> Vec<u8> {
        Envelope { request_id, payload: Some(payload) }.encode_to_vec()
    }

    /// Decode one deframed payload; `None` when it is not an Envelope.
    pub fn decode(bytes: &[u8]) -> Option<Envelope> {
        Envelope::decode(bytes).ok()
    }

    fn setpoint(t_us: u32, target: Target, rate_limit: f32) -> board::Setpoint {
        board::Setpoint { t_us, target: Some(target), rate_limit, ..Default::default() }
    }

    pub fn speed_setpoint(t_us: u32, velocity: f32, rate_limit: f32) -> board::Setpoint {
        setpoint(t_us, Target::VelocityRadps(velocity), rate_limit)
    }

    pub fn torque_setpoint(t_us: u32, torque: f32, rate_limit: f32) -> board::Setpoint {
        setpoint(t_us, Target::TorqueNm(torque), rate_limit)
    }

    /// A position with its velocity limit (the rate limit), acceleration limit and
    /// velocity at the point.
    pub fn position_setpoint(t_us: u32, position: f32, velocity_limit: f32, accel_limit: f32, at_point: f32) -> board::Setpoint {
        board::Setpoint {
            accel_limit_radps2: accel_limit,
            velocity_at_point_radps: at_point,
            ..setpoint(t_us, Target::PositionRad(position), velocity_limit)
        }
    }

    pub fn stop_setpoint(t_us: u32) -> board::Setpoint {
        setpoint(t_us, Target::Stop(board::Stop {}), 0.0)
    }

    fn board_request(command: BoardCommand) -> Payload {
        Payload::BoardRequest(board::Request { command: Some(command) })
    }

    /// A `Command`: a mode and its setpoint sequence.
    pub fn command(mode: Mode, setpoints: Vec<board::Setpoint>) -> Payload {
        board_request(BoardCommand::Run(board::Command { mode: mode as i32, setpoints }))
    }

    pub fn get_motor_info() -> Payload {
        board_request(BoardCommand::GetMotorInfo(board::GetMotorInfo {}))
    }

    pub fn clear_fault() -> Payload {
        board_request(BoardCommand::ClearFault(board::ClearFault {}))
    }

    pub fn ping() -> Payload {
        Payload::Ping(pcs_proto::shared::PingRequest {})
    }

    /// A WatchRequest over (address, size, period_cycles) triples.
    pub fn watch_request(request_id: u32, watches: &[(u32, u32, u32)]) -> Vec<u8> {
        let watches = watches
            .iter()
            .map(|&(address, size, period_cycles)| trace::Watch { address, size, period_cycles })
            .collect();
        envelope(request_id, Payload::WatchRequest(trace::WatchRequest { watches }))
    }

    /// A LinkTestRequest for `frame_count` frames of `payload_bytes` each.
    pub fn link_test_request(request_id: u32, payload_bytes: u32, frame_count: u32) -> Vec<u8> {
        envelope(
            request_id,
            Payload::LinkTestRequest(pcs_proto::shared::LinkTestRequest { payload_bytes, frame_count }),
        )
    }

    pub fn read_request(request_id: u32, address: u32, size: u32) -> Vec<u8> {
        envelope(request_id, Payload::ReadRequest(trace::ReadRequest { address, size }))
    }

    pub fn write_request(request_id: u32, address: u32, data: &[u8]) -> Vec<u8> {
        envelope(request_id, Payload::WriteRequest(trace::WriteRequest { address, data: data.to_vec() }))
    }

    /// Record access on a `trace.Samples`: a run of one group's records.
    pub trait SamplesExt {
        /// Cycle index of record `k`, which sits `k * period_cycles` on.
        fn cycle_of(&self, k: u32) -> u32;
        /// Bytes per record — the group's watched spans concatenated.
        fn record_size(&self) -> usize;
        /// Record `k`'s bytes.
        fn record(&self, k: u32) -> &[u8];
    }

    impl SamplesExt for trace::Samples {
        fn cycle_of(&self, k: u32) -> u32 {
            self.first_cycle + (k * self.period_cycles)
        }

        fn record_size(&self) -> usize {
            assert!(self.count > 0, "a Samples message with no records has no record size: {self:?}");
            self.data.len() / (self.count as usize)
        }

        fn record(&self, k: u32) -> &[u8] {
            let size = self.record_size();
            let start = (k as usize) * size;
            &self.data[start..start + size]
        }
    }
}
