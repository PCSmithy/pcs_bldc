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
    /// base/enum type offset -> (DW_ATE encoding, byte size)
    base: HashMap<usize, (u8, u64)>,
}

pub(crate) struct DwarfMap(Maps);

impl DwarfMap {
    /// Parse the DWARF in a PE image (the firmware DLL).
    pub(crate) fn parse(bytes: &[u8]) -> Result<Self, Box<dyn Error>> {
        let object = object::File::parse(bytes)?;

        let load = |id: gimli::SectionId| -> Result<Cow<[u8]>, gimli::Error> {
            Ok(match object.section_by_name(id.name()) {
                Some(s) => s.uncompressed_data().unwrap_or(Cow::Borrowed(&[])),
                None => Cow::Borrowed(&[]),
            })
        };
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
    pub(crate) fn resolve(&self, path: &str) -> Option<(u64, Scalar)> {
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

        Some((addr, self.scalar_kind(ty)?))
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
                    maps.base.insert(g, (gimli::DW_ATE_unsigned.0, sz));
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
