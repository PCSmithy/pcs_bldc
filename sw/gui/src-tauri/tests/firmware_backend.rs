//! The firmware backend against the real native firmware DLL (skips with a
//! message when the DLL hasn't been built). The module is pulled in by path
//! because the app is a binary crate; main.rs declares the same file.

// The harness exercises a subset of the module; items the app binary
// consumes (commands, serde types) count as dead in THIS compilation.
#[allow(dead_code)]
#[path = "../src/firmware.rs"]
mod firmware;

use std::path::PathBuf;

fn native_dll() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../../../build/native-fw/src/libpcs_bldc_fw.dll")
}

fn load_or_skip() -> Option<firmware::LoadedFirmware> {
    let dll = native_dll();
    if !dll.exists() {
        eprintln!(
            "skipping: {} absent (run tools/build_native.sh first)",
            dll.display()
        );
        return None;
    }
    Some(firmware::LoadedFirmware::load(&dll).expect("load native firmware DLL"))
}

// [test->app~obs_001~1]
#[test]
fn loads_firmware_and_resolves_signals() {
    let Some(fw) = load_or_skip() else { return };

    // The embedded identity: 12-hex commit, optionally +8-hex dirty suffix.
    assert!(
        !fw.build_id.is_empty(),
        "identity string read from the image"
    );
    assert!(
        fw.build_id
            .chars()
            .all(|c| c.is_ascii_hexdigit() || c == '+'),
        "identity charset: {}",
        fw.build_id
    );
    assert!(fw.build_id.len() == 12 || fw.build_id.len() == 21);

    // The namespace is the real firmware's — hundreds of leaves.
    assert!(
        fw.signal_paths().len() > 100,
        "enumerated {} signal paths",
        fw.signal_paths().len()
    );

    // A known 1 kHz counter resolves as a 4-byte unsigned scalar (full
    // address: the DLL's 64-bit image base is what resolve_watch rejects).
    let (addr, size, leaf) = fw.resolve_leaf("task1msRuns").expect("resolve task1msRuns");
    assert_ne!(addr, 0);
    assert_eq!(size, 4);
    assert!(matches!(
        leaf,
        dwarf_map::Leaf::Scalar(dwarf_map::Scalar::U32)
    ));

    // The device cast refuses (never truncates) an address past u32.
    let err = fw.resolve_watch("task1msRuns").unwrap_err();
    assert!(err.contains("32-bit"), "truncation must error: {err}");

    // A bogus path errors instead of resolving.
    assert!(fw.resolve_leaf("no_such_symbol_xyz").is_err());

    // An enumeration resolves with its enumerator names by value: some enum
    // leaf exists in the namespace and its list is non-empty and value-sorted
    // (name agreement with the per-value lookup is the dwarf_map crate's own
    // test); a scalar leaf carries none.
    let enum_leaf = fw
        .signal_paths()
        .iter()
        .find_map(|p| match fw.resolve_leaf(p) {
            Ok((_, _, leaf @ dwarf_map::Leaf::Enum(_))) => Some(leaf),
            _ => None,
        })
        .expect("the firmware namespace holds at least one enum leaf");
    let enums = fw
        .enumerators(enum_leaf)
        .expect("enum leaf lists enumerators");
    assert!(!enums.is_empty());
    assert!(enums.windows(2).all(|w| w[0].0 < w[1].0), "value-sorted");
    assert!(
        fw.enumerators(leaf).is_none(),
        "scalar leaf carries no enumerators"
    );
}

/// The firmware's module statics, from the image's linker symbol table
/// (independent of the DWARF): `<layer>_<Module>_data` per module, plus
/// main.c's file-local statics (`main_*`, `task1msRuns`). C++
/// internal-linkage names arrive mangled `_ZL<len><name>`; the source name
/// is the tail.
fn module_statics(image: &[u8]) -> Vec<(String, u64)> {
    use object::{Object, ObjectSymbol, SymbolKind};
    let object = object::File::parse(image).expect("parse image");
    let mut out: Vec<(String, u64)> = object
        .symbols()
        .filter(|s| s.kind() == SymbolKind::Data)
        .filter_map(|s| {
            let raw = s.name().ok()?;
            let name = match raw.strip_prefix("_ZL") {
                Some(rest) => rest.trim_start_matches(|c: char| c.is_ascii_digit()),
                None => raw,
            };
            let layered = ["HW_", "IO_", "dev_", "app_", "lib_"]
                .iter()
                .any(|p| name.starts_with(p))
                && name.ends_with("_data");
            let main_local = name.starts_with("main_") || (name == "task1msRuns");
            (layered || main_local).then(|| (name.to_string(), s.address()))
        })
        .collect();
    out.sort();
    out.dedup();
    out
}

/// Module statics with no selectable leaf: a lone config pointer (no scalar
/// signal), or byte buffers past the enumeration's array threshold.
const UNLISTED_STATICS: [&str; 3] = ["IO_SK6805_data", "IO_i2c_data", "IO_serial_data"];

// [test->sys~obs_002~1]
// [test->app~obs_001~1]
#[test]
fn every_module_static_is_selectable_at_its_linked_address() {
    let Some(fw) = load_or_skip() else { return };
    let image = std::fs::read(native_dll()).expect("read image");
    let dwarf = dwarf_map::DwarfMap::from_lib_path(&native_dll()).expect("parse DWARF");
    let statics = module_statics(&image);

    // Every layer links in: a wide set, file-local definitions among them.
    for layer in ["HW_", "IO_", "dev_", "app_", "lib_", "main_"] {
        assert!(
            statics.iter().any(|(n, _)| n.starts_with(layer)),
            "no {layer} statics in the symbol table: {statics:?}"
        );
    }

    let mut absent = Vec::new();
    for (name, sym_addr) in &statics {
        // Listed under its source name, as itself or by member path.
        let prefix = format!("{name}.");
        let prefix_index = format!("{name}[");
        let listed: Vec<&String> = fw
            .signal_paths()
            .iter()
            .filter(|p| *p == name || p.starts_with(&prefix) || p.starts_with(&prefix_index))
            .collect();
        if listed.is_empty() {
            absent.push(name.clone());
        }
        // The debug data places it where the linker did, and every listed
        // leaf resolves inside it.
        assert_eq!(dwarf.var_addr(name), Some(*sym_addr), "{name}: address");
        for path in listed {
            let (addr, size, _) = fw.resolve_leaf(path).expect("listed leaf resolves");
            assert!(addr >= *sym_addr && size > 0, "{path} at {addr:#x} below {name}");
        }
    }
    assert!(
        absent.iter().map(String::as_str).eq(UNLISTED_STATICS),
        "statics absent from the selection list: {absent:?}"
    );

    // Size and type equal the source declarations, file-local and member
    // leaves alike.
    let typed = |path: &str| {
        let (_, size, leaf) = fw.resolve_leaf(path).expect(path);
        (size, leaf)
    };
    let find = |owner: &str, leaf: &str| {
        fw.signal_paths()
            .iter()
            .find(|p| p.starts_with(owner) && p.ends_with(leaf))
            .unwrap_or_else(|| panic!("{owner}…{leaf} listed"))
            .clone()
    };
    use dwarf_map::{Leaf, Scalar};
    // main.c: static volatile uint32_t task1msRuns.
    assert!(matches!(typed("task1msRuns"), (4, Leaf::Scalar(Scalar::U32))));
    // app_cpuLoad.c: static struct { volatile uint32_t taskTime_us[]; ... }.
    assert!(matches!(
        typed("app_cpuLoad_data.taskTime_us[1]"),
        (4, Leaf::Scalar(Scalar::U32))
    ));
    // app_rotorEstimation.cpp: float32_t angleMeasured_rad; bool isSeeded.
    assert!(matches!(
        typed(&find("app_rotorEstimation_data", ".angleMeasured_rad")),
        (4, Leaf::Scalar(Scalar::F32))
    ));
    assert!(matches!(
        typed(&find("app_rotorEstimation_data", ".isSeeded")),
        (1, Leaf::Scalar(Scalar::Bool))
    ));
}

// [test->app~obs_006~1]
#[test]
fn readonly_follows_section_writability() {
    let Some(fw) = load_or_skip() else { return };

    // The identity anchor is a const char[] in read-only storage; the 1 kHz
    // counter is a mutable static in a writable section. Full 64-bit
    // addresses: the DLL's image base overflows resolve_watch's device cast.
    let counter_addr = fw.resolve_addr("task1msRuns").expect("resolve counter");
    assert!(
        !fw.is_readonly(counter_addr),
        "task1msRuns must classify writable"
    );
    let ident_addr = fw
        .resolve_addr("lib_build_identityString[0]")
        .expect("resolve identity anchor byte");
    assert!(
        fw.is_readonly(ident_addr),
        "lib_build_identityString must classify read-only"
    );
}

// [test->app~obs_002~1]
#[test]
fn identity_gate_is_exact_equality() {
    assert!(firmware::identity_matches(
        "6bded7b9907a+197e0212",
        "6bded7b9907a+197e0212"
    ));
    assert!(!firmware::identity_matches(
        "6bded7b9907a+197e0212",
        "6bded7b9907a"
    ));
    assert!(!firmware::identity_matches("", "6bded7b9907a"));
}
