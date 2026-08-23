//! The firmware backend against the real native firmware DLL (skips with a
//! message when the DLL hasn't been built). The module is pulled in by path
//! because the app is a binary crate; main.rs declares the same file.

#[path = "../src/firmware.rs"]
mod firmware;

use std::path::PathBuf;

fn native_dll() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../../../build/native-fw/src/libpcs_bldc_fw.dll")
}

// [test->app~obs_001~1]
#[test]
fn loads_firmware_and_resolves_signals() {
    let dll = native_dll();
    if !dll.exists() {
        eprintln!(
            "skipping: {} absent (run tools/build_native.sh first)",
            dll.display()
        );
        return;
    }
    let fw = firmware::LoadedFirmware::load(&dll).expect("load native firmware DLL");

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

    // A known 1 kHz counter resolves as a 4-byte unsigned scalar.
    let (addr, size, leaf) = fw
        .resolve_watch("task1msRuns")
        .expect("resolve task1msRuns");
    assert_ne!(addr, 0);
    assert_eq!(size, 4);
    assert!(matches!(
        leaf,
        dwarf_map::Leaf::Scalar(dwarf_map::Scalar::U32)
    ));

    // A bogus path errors instead of resolving.
    assert!(fw.resolve_watch("no_such_symbol_xyz").is_err());

    // An enumeration resolves with its enumerator names by value: some enum
    // leaf exists in the namespace and its list is non-empty and value-sorted
    // (name agreement with the per-value lookup is the dwarf_map crate's own
    // test); a scalar leaf carries none.
    let enum_leaf = fw
        .signal_paths()
        .iter()
        .find_map(|p| match fw.resolve_watch(p) {
            Ok((_, _, leaf @ dwarf_map::Leaf::Enum(_))) => Some(leaf),
            _ => None,
        })
        .expect("the firmware namespace holds at least one enum leaf");
    let enums = fw.enumerators(enum_leaf).expect("enum leaf lists enumerators");
    assert!(!enums.is_empty());
    assert!(enums.windows(2).all(|w| w[0].0 < w[1].0), "value-sorted");
    assert!(fw.enumerators(leaf).is_none(), "scalar leaf carries no enumerators");
}

// [test->app~obs_006~1]
#[test]
fn readonly_follows_section_writability() {
    let dll = native_dll();
    if !dll.exists() {
        eprintln!(
            "skipping: {} absent (run tools/build_native.sh first)",
            dll.display()
        );
        return;
    }
    let fw = firmware::LoadedFirmware::load(&dll).expect("load native firmware DLL");

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
