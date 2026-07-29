//! The flagship path: command the AS5048 encoder model's angle through the unit
//! boundary and read it back out of real firmware telemetry — the model answers the
//! firmware's actual READ-ANGLE polls over the duplex bus, IO_AS5048 decodes,
//! telemetryTask reports, and the sim USB capture carries the Teleplot text.

use pcs_bldc_sil::{cid, As5048Model, Sil, SOURCE};
use voyant::{Firmware, SignalId, Value};

/// Read the sim USB TX capture buffer (txLen + tx[] bytes) by DWARF and decode it as
/// the Teleplot text the firmware emitted.
fn read_tx_capture(fw: &Firmware) -> String {
    let len = fw.read_cvar("HW_USB_sim_data.txLen").as_u64().unwrap_or(0);
    let mut bytes = Vec::with_capacity(len as usize);
    for i in 0..len {
        let b = fw.read_cvar(&format!("HW_USB_sim_data.tx[{i}]")).as_u64().unwrap_or(0) as u8;
        bytes.push(b);
    }
    String::from_utf8_lossy(&bytes).into_owned()
}

#[test]
fn end_to_end() {
    const CH: usize = 0; // HW_SPI_CHANNEL_AS5048_1
    const CMD_DEG: f64 = 90.0;

    let mut sim = Sil::new();
    // Both encoder channels get a real model instance. Member order [models,
    // firmware] gives zero-lag freshness: a model folds its commanded angle before
    // the firmware's same-tick SPI polls read it. (Tick 1's first poll returns the
    // model's power-on sentinel — an error frame — exactly like real hardware's first
    // pipelined read; the second poll onward carries the angle.)
    let motor = sim.add_member(As5048Model::new("as5048_motor", 0.0));
    let dial = sim.add_member(As5048Model::new("dial", 0.0));
    let fwm = sim.load_firmware(SOURCE);
    sim.add_member(fwm);
    sim.link_duplex("spi:pcs_bldc:AS5048_1", motor).expect("link motor encoder");
    sim.link_duplex("spi:pcs_bldc:AS5048_2", dial).expect("link dial encoder");

    // Both encoder channels register their SPI endpoints as :tx/:rx event entries —
    // the dial (AS5048_2) too, which stage 3 drives as the velocity demand.
    let ep = |ch: &str, m: &str| format!("spi:{SOURCE}:{ch}:{m}");
    let endpoints_present = ["AS5048_1", "AS5048_2"].iter().all(|ch| {
        ["tx", "rx"]
            .iter()
            .all(|m| sim.state().signals().any(|s| s.as_str() == ep(ch, m)))
    });
    assert!(
        endpoints_present,
        "both AS5048 duplex endpoints registered: {}, {}",
        ep("AS5048_1", "rx"),
        ep("AS5048_2", "rx")
    );

    // Expected raw/angle derive from the commanded angle plus the firmware's own
    // channel config (reverse maps raw to 16384 - raw): the check asserts the decode
    // contract, not a frozen board convention.
    let reverse = match sim.fw().read_cvar(&format!("IO_AS5048_channelConfig[{CH}].reverse")) {
        Value::Bool(b) => b,
        other => panic!("IO_AS5048_channelConfig[{CH}].reverse read back as {other:?}, not Bool"),
    };
    let wire_raw = ((CMD_DEG / 360.0) * 16384.0).round() as u64; // the model's frame
    let exp_raw: u64 = if reverse { 16384 - wire_raw } else { wire_raw };
    let exp_deg = (exp_raw as f64) * 360.0 / 16384.0;
    let exp_deg_str = format!("{exp_deg:.2}");

    // Command the shaft angle in degrees — the boundary converts to canonical rad.
    // Open the USB port (telemetryTask skips its body otherwise) and drain the stale
    // TX capture; the cvar flush lands during step 1's in-sync.
    sim.write("vsig:as5048_motor:angle[deg]", CMD_DEG).expect("write angle[deg]");
    sim.write(&cid("HW_USB_sim_data.connected"), true).expect("write connected");
    sim.write(&cid("HW_USB_sim_data.txLen"), 0u32).expect("drain txLen");

    // task_1ms samples the encoder each tick; telemetry fires every 2 ms.
    for _ in 0..10 {
        sim.step().expect("engine step");
    }

    // The SPI transactions land as event records: the AS5048 does two transfers per
    // tick, and events are force-recorded (never deduped), so :tx / :rx each
    // accumulate more entries than the 10 ticks would allow under level dedup. :tx
    // carries the READ-ANGLE command (0xFFFF -> {0xFF,0xFF}); :rx the model's angle
    // frame (raw 4096 + parity = 0x9000 -> {0x90,0x00}).
    let tx_id = SignalId::parse(&ep("AS5048_1", "tx")).expect("valid spi id");
    let rx_id = SignalId::parse(&ep("AS5048_1", "rx")).expect("valid spi id");
    let n_tx = sim.state().changes(&tx_id).map(|c| c.len()).unwrap_or(0);
    let n_rx = sim.state().changes(&rx_id).map(|c| c.len()).unwrap_or(0);
    let last_tx = sim.state().current_value(&tx_id).ok().flatten();
    let last_rx = sim.state().current_value(&rx_id).ok().flatten();
    assert!(
        (n_tx > 10) && (n_rx > 10)
            && (last_tx == Some(Value::Bytes(vec![0xFF, 0xFF])))
            && (last_rx == Some(Value::Bytes(vec![0x90, 0x00]))),
        "SPI recorded as :tx/:rx events: {n_tx} tx + {n_rx} rx over 10 ticks; last tx={last_tx:?}, rx={last_rx:?}"
    );

    // The connected flag reached firmware (the mirror reads it back true).
    let connected = matches!(
        sim.read(&cid("HW_USB_sim_data.connected")).ok().flatten(),
        Some(Value::Bool(true))
    );
    assert!(connected, "sim USB marked connected via table write + flush");

    // The decoded encoder statics reflect the model's frame (auto-mirrored).
    let raw = sim
        .read(&cid(&format!("IO_AS5048_data.channels[{CH}].raw")))
        .ok()
        .flatten()
        .as_ref()
        .and_then(Value::as_u64)
        .unwrap_or(0);
    let deg = sim
        .read(&cid(&format!("IO_AS5048_data.channels[{CH}].angle_deg")))
        .ok()
        .flatten()
        .as_ref()
        .and_then(Value::as_f64)
        .unwrap_or(0.0);
    assert!(
        (raw == exp_raw) && ((deg - exp_deg).abs() < 0.05),
        "AS5048 decodes the model's SPI frame: raw = {raw} (expect {exp_raw}, reverse={reverse}), angle_deg = {deg:.2} (expect {exp_deg_str})"
    );

    // Pull the sim USB TX capture and confirm the Teleplot telemetry text.
    // Direct read: tx[] is over the mirror threshold — see backlog usb_cdc/teleplot.
    let text = read_tx_capture(&sim.fw());
    let has_keys = text.contains("motor_angle:") && text.contains("motor_raw:");
    let has_angle = text.contains(&exp_deg_str);
    let has_raw = text.contains(&exp_raw.to_string());
    assert!(
        has_keys,
        "telemetry text present with expected keys: captured {} bytes; motor_angle/motor_raw keys MISSING",
        text.len()
    );
    assert!(
        has_angle && has_raw,
        "telemetry carries the commanded angle end-to-end: contains angle {exp_deg_str} = {has_angle}, raw {exp_raw} = {has_raw}"
    );

    // Drain the capture (txLen back to 0, flushed on the next step's in-sync before
    // telemetry runs) and re-verify the next windows refill it — proves the path
    // keeps flowing and the drain works.
    sim.write(&cid("HW_USB_sim_data.txLen"), 0u32).expect("drain txLen");
    for _ in 0..6 {
        sim.step().expect("engine step");
    }
    let text2 = read_tx_capture(&sim.fw());
    // motor_angle goes out in the fast (2 ms) tier every window; motor_raw is
    // slow-tier (200 ms) and won't appear in a short post-drain capture.
    assert!(
        text2.contains(&exp_deg_str) && text2.contains("motor_angle:"),
        "TX capture drains and refills across windows: post-drain capture {} bytes, still carries the angle",
        text2.len()
    );
}
