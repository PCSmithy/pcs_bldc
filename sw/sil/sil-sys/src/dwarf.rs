//! Minimal DWARF reader: resolve a firmware variable (or `var.member.member`
//! path) to its **link-time address** and leaf size, by parsing the DLL's
//! `.debug_*` sections.
//!
//! This reaches *any* `static` — not just exported symbols — which is what the
//! State Table needs (ffi-boundary.md §4). Built with `object` (PE + sections)
//! + `gimli` (DWARF). The native firmware is `-g -O0`, so every static keeps a
//! real address and structs keep their members.
//!
//! Scope (first cut): top-level/static variables with a `DW_OP_addr` location,
//! struct/union member offsets, and typedef/const/volatile pass-through to find
//! the underlying struct (for member lookup) and base-type size. Arrays and
//! pointer-chasing come later; name collisions (e.g. function-local statics)
//! are last-wins until the State Table adds qualified paths.

use object::{Object, ObjectSection};
use std::borrow::Cow;
use std::collections::HashMap;
use std::error::Error;

type Slice<'a> = gimli::EndianSlice<'a, gimli::LittleEndian>;

#[derive(Default)]
struct Maps {
    /// variable name -> (link address, type's global .debug_info offset)
    vars: HashMap<String, (u64, usize)>,
    /// struct/union type offset -> (member name -> (member offset, member type offset))
    members: HashMap<usize, HashMap<String, (u64, usize)>>,
    /// typedef/const/volatile/restrict offset -> the type it wraps
    underlying: HashMap<usize, usize>,
    /// type offset -> byte size (base/struct/pointer/enum types)
    sizes: HashMap<usize, u64>,
}

pub struct DwarfMap(Maps);

impl DwarfMap {
    /// Parse the DWARF in a PE image (the firmware DLL).
    pub fn parse(bytes: &[u8]) -> Result<Self, Box<dyn Error>> {
        let object = object::File::parse(bytes)?;

        let load = |id: gimli::SectionId| -> Result<Cow<[u8]>, gimli::Error> {
            Ok(match object.section_by_name(id.name()) {
                Some(s) => s.uncompressed_data().unwrap_or(Cow::Borrowed(&[])),
                None => Cow::Borrowed(&[]),
            })
        };
        let dwarf_sections = gimli::DwarfSections::load(&load)?;
        let dwarf =
            dwarf_sections.borrow(|section| gimli::EndianSlice::new(section, gimli::LittleEndian));

        let mut maps = Maps::default();
        let mut units = dwarf.units();
        while let Some(header) = units.next()? {
            let unit = dwarf.unit(header)?;
            collect_unit(&dwarf, &unit, &mut maps)?;
        }
        Ok(DwarfMap(maps))
    }

    /// Link-time address of a top-level variable.
    pub fn var_addr(&self, name: &str) -> Option<u64> {
        self.0.vars.get(name).map(|&(addr, _)| addr)
    }

    /// Resolve a `var` or `var.member[.member...]` path to (link address, leaf
    /// byte size). Struct membership only (no arrays/pointers yet).
    pub fn resolve(&self, path: &str) -> Option<(u64, u64)> {
        let mut parts = path.split('.');
        let (mut addr, ty) = *self.0.vars.get(parts.next()?)?;
        let mut ty = self.peel(ty);
        for member in parts {
            let &(off, mty) = self.0.members.get(&ty)?.get(member)?;
            addr += off;
            ty = self.peel(mty);
        }
        Some((addr, self.0.sizes.get(&ty).copied().unwrap_or(0)))
    }

    /// Strip typedef/const/volatile/restrict to the underlying (struct/base) type.
    fn peel(&self, mut ty: usize) -> usize {
        for _ in 0..32 {
            match self.0.underlying.get(&ty) {
                Some(&u) => ty = u,
                None => break,
            }
        }
        ty
    }
}

fn collect_unit(
    dwarf: &gimli::Dwarf<Slice>,
    unit: &gimli::Unit<Slice>,
    maps: &mut Maps,
) -> Result<(), gimli::Error> {
    // Flat DFS with an ancestor stack so members can find their enclosing struct.
    let mut stack: Vec<(isize, usize, gimli::DwTag)> = Vec::new();
    let mut depth = 0isize;

    let mut entries = unit.entries();
    while let Some((delta, entry)) = entries.next_dfs()? {
        depth += delta;
        while matches!(stack.last(), Some(&(d, _, _)) if d >= depth) {
            stack.pop();
        }

        let goff = entry
            .offset()
            .to_debug_info_offset(&unit.header)
            .map(|o| o.0);
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
            gimli::DW_TAG_base_type
            | gimli::DW_TAG_pointer_type
            | gimli::DW_TAG_enumeration_type
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
