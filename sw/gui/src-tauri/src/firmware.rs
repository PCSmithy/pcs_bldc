//! Loaded-firmware backend, owned by the native core: the ELF's DWARF
//! namespace (signal enumeration + resolution) and its embedded build
//! identity. The identity gate compares this ELF identity against the
//! device's reported one. Self-contained: no `crate::` references, so the
//! module also compiles under the integration-test harness.

use std::path::{Path, PathBuf};
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
    pub path: PathBuf,
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
}

/// The image's section ranges with writability, collected once at load so
/// the per-signal read-only check is a range lookup, not an object-file
/// parse.
// [impl->app~obs_006~1]
fn section_ranges(image: &[u8]) -> Result<Vec<(u64, u64, bool)>, String> {
    use object::SectionFlags;
    let file = object::File::parse(image).map_err(|e| format!("parse image: {e}"))?;
    Ok(file
        .sections()
        .filter(|s| s.size() > 0)
        .map(|s| {
            let writable = match s.flags() {
                SectionFlags::Elf { sh_flags } => {
                    sh_flags & u64::from(object::elf::SHF_WRITE) != 0
                }
                SectionFlags::Coff { characteristics } => {
                    characteristics & object::pe::IMAGE_SCN_MEM_WRITE != 0
                }
                // No usable per-section write bit: treat as writable so the
                // exclusion never hides a signal it cannot classify.
                _ => true,
            };
            (s.address(), s.address() + s.size(), writable)
        })
        .collect())
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
fn read_identity(image: &[u8], addr: u64) -> Result<String, String> {
    let object = object::File::parse(image).map_err(|e| format!("parse image: {e}"))?;
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
        let build_id = read_identity(&image, addr)?;
        let sections = section_ranges(&image)?;
        let enumeration = map.enumerate_leaves(ENUM_ARRAY_THRESHOLD, &[]);
        Ok(Self {
            map,
            build_id,
            path: path.to_path_buf(),
            signal_paths: enumeration.paths,
            section_ranges: sections,
        })
    }

    /// Full link address of a resolvable path (no watch-size constraint, no
    /// device-width cast — the readonly lookup needs the whole address).
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

    pub fn signal_paths(&self) -> &[String] {
        &self.signal_paths
    }

    /// Resolve a signal path to a watch entry: device address (link-time ==
    /// runtime on this MCU), byte size, and leaf type. Leaves outside the
    /// 1..8-byte watch range are rejected.
    pub fn resolve_watch(&self, path: &str) -> Result<(u32, u32, dwarf_map::Leaf), String> {
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
        Ok((addr as u32, size as u32, leaf))
    }
}

/// The identity gate's comparison: exact string equality between the
/// device's reported build id and the loaded ELF's embedded one.
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
    *state.0.lock().unwrap() = Some(loaded);
    Ok(info)
}

// [impl->app~obs_001~1]
#[tauri::command]
pub fn list_signals(
    state: State<FirmwareState>,
    filter: String,
    limit: usize,
) -> Result<Vec<SignalInfo>, String> {
    let guard = state.0.lock().unwrap();
    let firmware = guard.as_ref().ok_or("no firmware ELF loaded")?;
    let needle = filter.to_lowercase();
    Ok(firmware
        .signal_paths
        .iter()
        .filter(|p| needle.is_empty() || p.to_lowercase().contains(&needle))
        .filter_map(|p| {
            let (addr, leaf) = firmware.map.resolve(p)?;
            let size = firmware.map.leaf_size(leaf)? as u32;
            Some(SignalInfo {
                path: p.clone(),
                kind: leaf_kind(leaf).to_string(),
                size,
                readonly: firmware.is_readonly(addr),
            })
        })
        .take(limit)
        .collect())
}
