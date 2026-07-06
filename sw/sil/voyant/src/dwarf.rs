//! DWARF reader: resolve a firmware variable path to its **link-time address**
//! and **scalar leaf type**, by parsing the DLL's `.debug_*` sections.
//!
//! Reaches *any* `static` (not just exported symbols) — the State Table needs
//! this (ffi-boundary.md §4). Built with `object` (PE + sections) + `gimli`
//! (DWARF). The native firmware is `-g -O0`, so every static keeps a real
//! address and aggregates keep their layout.
//!
//! Supports: top-level/static variables (`DW_OP_addr`), struct/union member
//! offsets, **array indexing** (`a[i].b[j]`), typedef/const/volatile
//! pass-through, and base/enum scalar kinds (signed/unsigned/float/bool).
//! Not yet: pointer-chasing. Name collisions (function-local statics) are
//! last-wins until the State Table adds qualified paths.

use object::{Object, ObjectSection};
use std::borrow::Cow;
use std::collections::HashMap;
use std::error::Error;
use std::path::{Path, PathBuf};

/// Fetch a DWARF section by its gimli name, tolerant of the host object format.
/// ELF/PE name the sections `.debug_info`; Mach-O uses `__debug_info` in the
/// `__DWARF` segment, capped to the 16-char sectname limit (so `.debug_str_offsets`
/// becomes `__debug_str_offs`). Try the gimli name first, then the Mach-O form.
fn section_data<'a>(obj: &'a object::File, id: gimli::SectionId) -> Cow<'a, [u8]> {
    let name = id.name();
    let mut macho = String::from("__");
    macho.push_str(&name[1..]);
    macho.truncate(16);
    obj.section_by_name(name)
        .or_else(|| obj.section_by_name(&macho))
        .and_then(|s| s.uncompressed_data().ok())
        .unwrap_or(Cow::Borrowed(&[]))
}

/// True if the parsed image carries embedded DWARF (ELF/PE do; a linked Mach-O
/// does not — its DWARF lives in a sibling .dSYM).
fn image_has_dwarf(bytes: &[u8]) -> bool {
    object::File::parse(bytes)
        .ok()
        .and_then(|o| {
            o.section_by_name(".debug_info")
                .or_else(|| o.section_by_name("__debug_info"))
                .map(|s| s.size() > 0)
        })
        .unwrap_or(false)
}

/// The DWARF file inside a macOS `.dSYM` bundle for `lib`, i.e.
/// `<lib>.dSYM/Contents/Resources/DWARF/<lib filename>`.
fn dsym_dwarf_path(lib: &Path) -> Option<PathBuf> {
    let file = lib.file_name()?;
    let mut bundle = lib.as_os_str().to_owned();
    bundle.push(".dSYM");
    let mut p = PathBuf::from(bundle);
    p.push("Contents");
    p.push("Resources");
    p.push("DWARF");
    p.push(file);
    Some(p)
}

type Slice<'a> = gimli::EndianSlice<'a, gimli::LittleEndian>;

/// A DWARF base/enum scalar leaf kind. The cvar resolver coerces these into the
/// logical [`crate::signal::Value`].
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Scalar {
    U8,
    U16,
    U32,
    U64,
    I8,
    I16,
    I32,
    I64,
    F32,
    F64,
    Bool,
}

/// A resolved leaf type: a plain scalar, or an enum (identified by its type
/// offset, so the cvar resolver can map the raw integer to an enumerator name).
#[derive(Clone, Copy, Debug)]
pub(crate) enum Leaf {
    Scalar(Scalar),
    Enum(usize),
}

/// An enumeration type: its underlying byte size + numeric-value → name map.
#[derive(Default)]
struct EnumInfo {
    size: u64,
    values: HashMap<i64, String>,
}

#[derive(Default)]
struct Maps {
    /// variable name -> (link address, type's global .debug_info offset)
    vars: HashMap<String, (u64, usize)>,
    /// struct/union type offset -> (member name -> (member offset, member type offset))
    members: HashMap<usize, HashMap<String, (u64, usize)>>,
    /// typedef/const/volatile/restrict offset -> the type it wraps
    underlying: HashMap<usize, usize>,
    /// array type offset -> element type offset
    arrays: HashMap<usize, usize>,
    /// any type offset -> byte size (for member/array address arithmetic)
    sizes: HashMap<usize, u64>,
    /// base type offset -> (DW_ATE encoding, byte size)
    base: HashMap<usize, (u8, u64)>,
    /// enumeration type offset -> its size + value→name map
    enums: HashMap<usize, EnumInfo>,
}

pub(crate) struct DwarfMap(Maps);

impl DwarfMap {
    /// Load the firmware's DWARF given the shared-library path. ELF/PE embed it
    /// in the image; macOS ld64 leaves it in a sibling `.dSYM` bundle (produced
    /// by `dsymutil`), so fall back to that when the image itself carries none.
    pub(crate) fn from_lib_path(lib: &Path) -> Result<Self, Box<dyn Error>> {
        let image = std::fs::read(lib)?;
        if image_has_dwarf(&image) {
            return Self::parse(&image);
        }
        let dsym = dsym_dwarf_path(lib)
            .filter(|p| p.exists())
            .ok_or_else(|| {
                format!(
                    "no DWARF in {} and no .dSYM alongside it (run dsymutil)",
                    lib.display()
                )
            })?;
        let dwarf = std::fs::read(&dsym)?;
        Self::parse(&dwarf)
    }

    /// Parse DWARF from an object image (ELF/PE firmware, or the Mach-O inside a
    /// macOS `.dSYM`).
    pub(crate) fn parse(bytes: &[u8]) -> Result<Self, Box<dyn Error>> {
        let object = object::File::parse(bytes)?;

        let load =
            |id: gimli::SectionId| -> Result<Cow<[u8]>, gimli::Error> { Ok(section_data(&object, id)) };
        let sections = gimli::DwarfSections::load(&load)?;
        let dwarf = sections.borrow(|s| gimli::EndianSlice::new(s, gimli::LittleEndian));

        let mut maps = Maps::default();
        let mut units = dwarf.units();
        while let Some(header) = units.next()? {
            let unit = dwarf.unit(header)?;
            collect_unit(&dwarf, &unit, &mut maps)?;
        }
        Ok(DwarfMap(maps))
    }

    /// Link-time address of a top-level variable.
    pub(crate) fn var_addr(&self, name: &str) -> Option<u64> {
        self.0.vars.get(name).map(|&(addr, _)| addr)
    }

    /// Resolve a `var[.member|[index]]...` path to (link address, scalar leaf
    /// kind). Returns None if any segment, member, index type, or leaf kind is
    /// unknown.
    pub(crate) fn resolve(&self, path: &str) -> Option<(u64, Leaf)> {
        let mut segs = path.split('.');

        let (name, indices) = split_indices(segs.next()?)?;
        let (mut addr, ty) = *self.0.vars.get(name)?;
        let mut ty = self.peel(ty);
        ty = self.index(&mut addr, ty, &indices)?;

        for seg in segs {
            let (name, indices) = split_indices(seg)?;
            let &(off, mty) = self.0.members.get(&ty)?.get(name)?;
            addr += off;
            ty = self.peel(mty);
            ty = self.index(&mut addr, ty, &indices)?;
        }

        let leaf = if self.0.enums.contains_key(&ty) {
            Leaf::Enum(ty)
        } else {
            Leaf::Scalar(self.scalar_kind(ty)?)
        };
        Some((addr, leaf))
    }

    /// The byte size of an enum type.
    pub(crate) fn enum_size(&self, off: usize) -> Option<u64> {
        self.0.enums.get(&off).map(|e| e.size)
    }

    /// The enumerator name for a numeric value.
    pub(crate) fn enum_name(&self, off: usize, value: i64) -> Option<&str> {
        self.0.enums.get(&off)?.values.get(&value).map(String::as_str)
    }

    /// The numeric value for an enumerator name (for writes).
    pub(crate) fn enum_value(&self, off: usize, name: &str) -> Option<i64> {
        let e = self.0.enums.get(&off)?;
        e.values.iter().find(|(_, n)| *n == name).map(|(&v, _)| v)
    }

    /// Apply `[i][j]...` to an array type, advancing `addr` and returning the
    /// (peeled) element type.
    fn index(&self, addr: &mut u64, mut ty: usize, indices: &[usize]) -> Option<usize> {
        for &i in indices {
            let elem = self.peel(*self.0.arrays.get(&ty)?);
            let elem_size = *self.0.sizes.get(&elem)?;
            *addr += (i as u64) * elem_size;
            ty = elem;
        }
        Some(ty)
    }

    /// Strip typedef/const/volatile/restrict to the underlying type.
    fn peel(&self, mut ty: usize) -> usize {
        for _ in 0..32 {
            match self.0.underlying.get(&ty) {
                Some(&u) => ty = u,
                None => break,
            }
        }
        ty
    }

    fn scalar_kind(&self, ty: usize) -> Option<Scalar> {
        let &(enc, size) = self.0.base.get(&ty)?;
        Some(match enc {
            e if e == gimli::DW_ATE_float.0 => match size {
                4 => Scalar::F32,
                8 => Scalar::F64,
                _ => return None,
            },
            e if e == gimli::DW_ATE_boolean.0 => Scalar::Bool,
            e if e == gimli::DW_ATE_signed.0 || e == gimli::DW_ATE_signed_char.0 => match size {
                1 => Scalar::I8,
                2 => Scalar::I16,
                4 => Scalar::I32,
                8 => Scalar::I64,
                _ => return None,
            },
            e if e == gimli::DW_ATE_unsigned.0 || e == gimli::DW_ATE_unsigned_char.0 => match size {
                1 => Scalar::U8,
                2 => Scalar::U16,
                4 => Scalar::U32,
                8 => Scalar::U64,
                _ => return None,
            },
            _ => return None,
        })
    }
}

fn collect_unit(
    dwarf: &gimli::Dwarf<Slice>,
    unit: &gimli::Unit<Slice>,
    maps: &mut Maps,
) -> Result<(), gimli::Error> {
    // Flat DFS with an ancestor stack so members find their enclosing struct.
    let mut stack: Vec<(isize, usize, gimli::DwTag)> = Vec::new();
    let mut depth = 0isize;

    let mut entries = unit.entries();
    while let Some((delta, entry)) = entries.next_dfs()? {
        depth += delta;
        while matches!(stack.last(), Some(&(d, _, _)) if d >= depth) {
            stack.pop();
        }

        let goff = entry.offset().to_debug_info_offset(&unit.header).map(|o| o.0);
        let tag = entry.tag();

        match tag {
            gimli::DW_TAG_variable => {
                if let (Some(name), Some(addr), Some(ty)) =
                    (die_name(dwarf, unit, entry), die_addr(entry), type_goff(unit, entry))
                {
                    maps.vars.insert(name, (addr, ty));
                }
            }
            gimli::DW_TAG_member => {
                if let Some(&(_, parent, ptag)) = stack.last() {
                    if matches!(ptag, gimli::DW_TAG_structure_type | gimli::DW_TAG_union_type) {
                        if let (Some(name), Some(off), Some(ty)) = (
                            die_name(dwarf, unit, entry),
                            member_offset(entry),
                            type_goff(unit, entry),
                        ) {
                            maps.members.entry(parent).or_default().insert(name, (off, ty));
                        }
                    }
                }
            }
            gimli::DW_TAG_typedef
            | gimli::DW_TAG_const_type
            | gimli::DW_TAG_volatile_type
            | gimli::DW_TAG_restrict_type => {
                if let (Some(g), Some(ty)) = (goff, type_goff(unit, entry)) {
                    maps.underlying.insert(g, ty);
                }
            }
            gimli::DW_TAG_array_type => {
                if let (Some(g), Some(elem)) = (goff, type_goff(unit, entry)) {
                    maps.arrays.insert(g, elem);
                }
            }
            gimli::DW_TAG_base_type => {
                if let (Some(g), Some(sz)) = (goff, byte_size(entry)) {
                    maps.sizes.insert(g, sz);
                    if let Some(enc) = encoding(entry) {
                        maps.base.insert(g, (enc, sz));
                    }
                }
            }
            gimli::DW_TAG_enumeration_type => {
                if let (Some(g), Some(sz)) = (goff, byte_size(entry)) {
                    maps.sizes.insert(g, sz);
                    // Enumerator children (below) fill the value→name map.
                    maps.enums.entry(g).or_default().size = sz;
                }
            }
            gimli::DW_TAG_enumerator => {
                if let Some(&(_, parent, ptag)) = stack.last() {
                    if ptag == gimli::DW_TAG_enumeration_type {
                        if let (Some(name), Some(val)) =
                            (die_name(dwarf, unit, entry), const_value(entry))
                        {
                            maps.enums.entry(parent).or_default().values.insert(val, name);
                        }
                    }
                }
            }
            gimli::DW_TAG_pointer_type
            | gimli::DW_TAG_structure_type
            | gimli::DW_TAG_union_type => {
                if let (Some(g), Some(sz)) = (goff, byte_size(entry)) {
                    maps.sizes.insert(g, sz);
                }
            }
            _ => {}
        }

        if let Some(g) = goff {
            stack.push((depth, g, tag));
        }
    }
    Ok(())
}

fn die_name(
    dwarf: &gimli::Dwarf<Slice>,
    unit: &gimli::Unit<Slice>,
    entry: &gimli::DebuggingInformationEntry<Slice>,
) -> Option<String> {
    let attr = entry.attr_value(gimli::DW_AT_name).ok().flatten()?;
    let s = dwarf.attr_string(unit, attr).ok()?;
    Some(s.to_string_lossy().into_owned())
}

/// Address from a `DW_OP_addr` location expression (the only form statics use).
fn die_addr(entry: &gimli::DebuggingInformationEntry<Slice>) -> Option<u64> {
    match entry.attr_value(gimli::DW_AT_location).ok().flatten()? {
        gimli::AttributeValue::Exprloc(expr) => {
            let bytes = expr.0.slice();
            if bytes.len() >= 9 && bytes[0] == gimli::constants::DW_OP_addr.0 {
                Some(u64::from_le_bytes(bytes[1..9].try_into().ok()?))
            } else {
                None
            }
        }
        _ => None,
    }
}

fn type_goff(
    unit: &gimli::Unit<Slice>,
    entry: &gimli::DebuggingInformationEntry<Slice>,
) -> Option<usize> {
    match entry.attr_value(gimli::DW_AT_type).ok().flatten()? {
        gimli::AttributeValue::UnitRef(o) => o.to_debug_info_offset(&unit.header).map(|d| d.0),
        gimli::AttributeValue::DebugInfoRef(d) => Some(d.0),
        _ => None,
    }
}

fn member_offset(entry: &gimli::DebuggingInformationEntry<Slice>) -> Option<u64> {
    entry
        .attr_value(gimli::DW_AT_data_member_location)
        .ok()
        .flatten()?
        .udata_value()
}

fn byte_size(entry: &gimli::DebuggingInformationEntry<Slice>) -> Option<u64> {
    entry.attr_value(gimli::DW_AT_byte_size).ok().flatten()?.udata_value()
}

fn encoding(entry: &gimli::DebuggingInformationEntry<Slice>) -> Option<u8> {
    match entry.attr_value(gimli::DW_AT_encoding).ok().flatten()? {
        gimli::AttributeValue::Encoding(e) => Some(e.0),
        other => other.udata_value().map(|u| u as u8),
    }
}

/// An enumerator's `DW_AT_const_value` as an i64 (signed first, else unsigned).
fn const_value(entry: &gimli::DebuggingInformationEntry<Slice>) -> Option<i64> {
    let v = entry.attr_value(gimli::DW_AT_const_value).ok().flatten()?;
    v.sdata_value().or_else(|| v.udata_value().map(|u| u as i64))
}

/// Split a path segment like `counts[6]` into (`"counts"`, `[6]`), or
/// `tickCounter` into (`"tickCounter"`, `[]`).
fn split_indices(seg: &str) -> Option<(&str, Vec<usize>)> {
    match seg.find('[') {
        None => Some((seg, Vec::new())),
        Some(b) => {
            let mut indices = Vec::new();
            for tok in seg[b..].split(['[', ']']) {
                let tok = tok.trim();
                if !tok.is_empty() {
                    indices.push(tok.parse::<usize>().ok()?);
                }
            }
            Some((&seg[..b], indices))
        }
    }
}
