//! Loaded-firmware backend, owned by the native core: the ELF's DWARF
//! namespace (signal enumeration + resolution) and its embedded build
//! identity. The identity gate compares this ELF identity against the
//! device's reported one. Self-contained: no `crate::` references, so the
//! module also compiles under the integration-test harness.

use std::path::Path;
use std::sync::Mutex;

use object::{Object, ObjectSection};
use tauri::State;

/// Arrays above this element count are enumeration-excluded (the dwarf_map
/// threshold) — keeps the picker to individually-meaningful leaves.
const ENUM_ARRAY_THRESHOLD: usize = 64;

/// Longest identity string read out of an image: 12 hex + '+' + 8 hex + NUL
/// is 22; the cap only guards against a corrupt/missing terminator.
const IDENTITY_READ_CAP: usize = 64;

pub struct LoadedFirmware {
    map: dwarf_map::DwarfMap,
    pub build_id: String,
    signal_paths: Vec<String>,
    /// Section ranges of the image with their writability, for the
    /// read-only distinction.
    section_ranges: Vec<(u64, u64, bool)>,
}

#[derive(Default)]
pub struct FirmwareState(pub Mutex<Option<LoadedFirmware>>);

#[derive(serde::Serialize)]
pub struct ElfInfo {
    pub build_id: String,
    pub signal_count: usize,
}

#[derive(serde::Serialize)]
pub struct SignalInfo {
    pub path: String,
    pub kind: String,
    pub size: u32,
    /// True when the signal's storage is a non-writable section (const /
    /// rodata) — it cannot change at runtime.
    pub readonly: bool,
    /// An enum signal's (value, name) enumerators, value-sorted; absent for
    /// every other kind.
    // [impl->app~obs_001~1]
    #[serde(skip_serializing_if = "Option::is_none")]
    pub enums: Option<Vec<(i64, String)>>,
}

/// The image's section ranges with writability, collected once at load so
/// the per-signal read-only check is a range lookup, not an object-file
/// parse.
// [impl->app~obs_006~1]
fn section_ranges(file: &object::File) -> Vec<(u64, u64, bool)> {
    use object::SectionFlags;
    file.sections()
        .filter(|s| s.size() > 0)
        .map(|s| {
            let writable = match s.flags() {
                SectionFlags::Elf { sh_flags } => sh_flags & u64::from(object::elf::SHF_WRITE) != 0,
                SectionFlags::Coff { characteristics } => {
                    characteristics & object::pe::IMAGE_SCN_MEM_WRITE != 0
                }
                // No usable per-section write bit: treat as writable so the
                // exclusion never hides a signal it cannot classify.
                _ => true,
            };
            (s.address(), s.address() + s.size(), writable)
        })
        .collect()
}

fn leaf_kind(leaf: dwarf_map::Leaf) -> &'static str {
    use dwarf_map::Scalar;
    match leaf {
        dwarf_map::Leaf::Enum(_) => "enum",
        dwarf_map::Leaf::Scalar(kind) => match kind {
            Scalar::U8 => "u8",
            Scalar::U16 => "u16",
            Scalar::U32 => "u32",
            Scalar::U64 => "u64",
            Scalar::I8 => "i8",
            Scalar::I16 => "i16",
            Scalar::I32 => "i32",
            Scalar::I64 => "i64",
            Scalar::F32 => "f32",
            Scalar::F64 => "f64",
            Scalar::Bool => "bool",
        },
    }
}

/// Read the NUL-terminated identity string at link address `addr` out of the
/// image file's section data (the DWARF may live in a sibling .dSYM, but the
/// bytes live in the image itself).
fn read_identity(object: &object::File, addr: u64) -> Result<String, String> {
    for section in object.sections() {
        let start = section.address();
        if addr < start || addr >= start + section.size() {
            continue;
        }
        let data = section
            .data()
            .map_err(|e| format!("read section data: {e}"))?;
        let offset = (addr - start) as usize;
        if offset >= data.len() {
            break; // a NOBITS section (.bss) carries no file data
        }
        let slice = &data[offset..data.len().min(offset + IDENTITY_READ_CAP)];
        let len = slice
            .iter()
            .position(|&b| b == 0)
            .ok_or("identity string is unterminated")?;
        return String::from_utf8(slice[..len].to_vec())
            .map_err(|_| "identity string is not UTF-8".to_string());
    }
    Err("identity address lies outside every section with file data".to_string())
}

impl LoadedFirmware {
    pub fn load(path: &Path) -> Result<Self, String> {
        let map = dwarf_map::DwarfMap::from_lib_path(path)
            .map_err(|e| format!("parse DWARF from {}: {e}", path.display()))?;
        let addr = map.var_addr("lib_build_identityString").ok_or_else(|| {
            format!(
                "{} carries no lib_build_identityString — rebuild the firmware \
                 (builds before the identity anchor cannot be gate-checked)",
                path.display()
            )
        })?;
        let image = std::fs::read(path).map_err(|e| format!("read {}: {e}", path.display()))?;
        let object = object::File::parse(&*image).map_err(|e| format!("parse image: {e}"))?;
        let build_id = read_identity(&object, addr)?;
        let sections = section_ranges(&object);
        let enumeration = map.enumerate_leaves(ENUM_ARRAY_THRESHOLD, &[]);
        Ok(Self {
            map,
            build_id,
            signal_paths: enumeration.paths,
            section_ranges: sections,
        })
    }

    /// Full link address of a resolvable path (no watch-size constraint, no
    /// device-width cast — the readonly lookup needs the whole address).
    /// Consumed only by the firmware_backend integration harness, which
    /// compiles this module via #[path]; the app binary sees it as dead.
    #[allow(dead_code)]
    pub fn resolve_addr(&self, path: &str) -> Option<u64> {
        self.map.resolve(path).map(|(addr, _)| addr)
    }

    /// True when `addr` lies in a section without the write flag. An address
    /// outside every known section reports writable — unknown placement must
    /// never hide a signal.
    pub fn is_readonly(&self, addr: u64) -> bool {
        self.section_ranges
            .iter()
            .find(|&&(start, end, _)| addr >= start && addr < end)
            .is_some_and(|&(_, _, writable)| !writable)
    }

    /// Consumed only by the firmware_backend integration harness, which
    /// compiles this module via #[path]; the app binary sees it as dead.
    #[allow(dead_code)]
    pub fn signal_paths(&self) -> &[String] {
        &self.signal_paths
    }

    /// An enum leaf's (value, name) enumerators, value-sorted in the wire's
    /// unsigned domain. `None` for scalar leaves.
    // [impl->app~obs_001~1]
    pub fn enumerators(&self, leaf: dwarf_map::Leaf) -> Option<Vec<(i64, String)>> {
        match leaf {
            dwarf_map::Leaf::Enum(off) => {
                let size = self.map.enum_size(off).unwrap_or(4);
                self.map.enumerators(off).map(|e| wrap_enumerators(size, e))
            }
            dwarf_map::Leaf::Scalar(_) => None,
        }
    }

    /// Resolve a signal path to its full link address, byte size, and leaf
    /// type; leaves outside the 1..8-byte watch range are rejected.
    pub fn resolve_leaf(&self, path: &str) -> Result<(u64, u32, dwarf_map::Leaf), String> {
        let (addr, leaf) = self
            .map
            .resolve(path)
            .ok_or_else(|| format!("{path}: not a resolvable signal"))?;
        let size = self
            .map
            .leaf_size(leaf)
            .ok_or_else(|| format!("{path}: unknown size"))?;
        if size == 0 || size > 8 {
            return Err(format!(
                "{path}: {size}-byte leaf is outside the 1..8-byte watch range"
            ));
        }
        Ok((addr, size as u32, leaf))
    }

    /// [`resolve_leaf`](Self::resolve_leaf) narrowed to the device's 32-bit
    /// address space (link-time == runtime on this MCU) — never truncated.
    pub fn resolve_watch(&self, path: &str) -> Result<(u32, u32, dwarf_map::Leaf), String> {
        let (addr, size, leaf) = self.resolve_leaf(path)?;
        let address = u32::try_from(addr)
            .map_err(|_| format!("{path}: address {addr:#x} exceeds the device's 32-bit space"))?;
        Ok((address, size, leaf))
    }
}

/// Reinterpret enumerator values into the unsigned domain of the enum's
/// byte size, re-sorted. DWARF parses enumerator constants signed-first
/// (a 1-byte `0x80` arrives as -128), but trace samples decode zero-extended
/// — the display map must live in the wire's domain or negative enumerators
/// never match a sample. 8-byte enums pass through (no wider domain to
/// wrap into).
fn wrap_enumerators(size: u64, mut enums: Vec<(i64, String)>) -> Vec<(i64, String)> {
    if size < 8 {
        let mask = (1i64 << (8 * size)) - 1;
        for e in &mut enums {
            e.0 &= mask;
        }
        enums.sort_by(|a, b| a.0.cmp(&b.0).then_with(|| a.1.cmp(&b.1)));
    }
    enums
}

/// The identity gate: exact equality — no prefix or dirty-suffix tolerance.
// [impl->app~obs_002~1]
pub fn identity_matches(device_build_id: &str, elf_build_id: &str) -> bool {
    device_build_id == elf_build_id
}

// [impl->app~obs_001~1]
#[tauri::command]
pub fn load_elf(state: State<FirmwareState>, path: String) -> Result<ElfInfo, String> {
    let loaded = LoadedFirmware::load(Path::new(&path))?;
    let info = ElfInfo {
        build_id: loaded.build_id.clone(),
        signal_count: loaded.signal_paths.len(),
    };
    *state.0.lock().map_err(|_| "firmware state poisoned")? = Some(loaded);
    Ok(info)
}

// [impl->app~obs_001~1]
#[tauri::command]
pub fn list_signals(
    state: State<FirmwareState>,
    filter: String,
    limit: usize,
) -> Result<Vec<SignalInfo>, String> {
    let guard = state.0.lock().map_err(|_| "firmware state poisoned")?;
    let firmware = guard.as_ref().ok_or("no firmware ELF loaded")?;
    let needle = filter.to_lowercase();
    Ok(firmware
        .signal_paths
        .iter()
        .filter(|p| needle.is_empty() || p.to_lowercase().contains(&needle))
        .filter_map(|p| {
            let (addr, leaf) = firmware.map.resolve(p)?;
            let size = firmware.map.leaf_size(leaf)? as u32;
            let enums = firmware.enumerators(leaf);
            Some(SignalInfo {
                path: p.clone(),
                kind: leaf_kind(leaf).to_string(),
                size,
                readonly: firmware.is_readonly(addr),
                enums,
            })
        })
        .take(limit)
        .collect())
}

#[cfg(test)]
mod tests {
    use super::*;

    // [test->app~obs_001~1] enumerator values reach the frontend in the
    // wire's unsigned domain: a signed-parsed -1 in a 1-byte enum names the
    // zero-extended sample value 255.
    #[test]
    fn wrap_enumerators_reinterprets_into_the_unsigned_domain() {
        let wrapped = wrap_enumerators(
            1,
            vec![
                (0, "OK".to_string()),
                (-1, "ERR".to_string()),
                (-128, "STATUS".to_string()),
            ],
        );
        assert_eq!(
            wrapped,
            vec![
                (0, "OK".to_string()),
                (128, "STATUS".to_string()),
                (255, "ERR".to_string()),
            ]
        );
        // 8-byte enums pass through: there is no wider domain to wrap into.
        let full = vec![(-1, "ALL".to_string()), (7, "SEVEN".to_string())];
        assert_eq!(wrap_enumerators(8, full.clone()), full);
    }
}
