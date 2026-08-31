//! DWARF reader: resolve a firmware variable path to its **link-time address**
//! and **scalar leaf type**, by parsing the DLL's `.debug_*` sections.
//!
//! Reaches *any* `static`, not just exported symbols (ffi-boundary.md §4), via
//! `object` (PE) + `gimli` (DWARF); native firmware is built `-g`, and at `-O3`
//! an SRA-decomposed aggregate static gets a composite piece-list location
//! (some pieces storage-less), which the reader maps per-member. Supports
//! top-level statics, struct/union members, array indexing (`a[i].b[j]`),
//! typedef/const/volatile pass-through, and base/enum scalars. Not yet:
//! pointer-chasing. Name collisions (function-local statics) are last-wins.

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

/// A DWARF base/enum scalar leaf kind. Consumers coerce these into their own
/// value currency (the SIL's `Value`, the app's plot samples).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Scalar {
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
pub enum Leaf {
    Scalar(Scalar),
    Enum(usize),
}

/// The byte width of a [`Scalar`] leaf — the size a consumer reads, watches,
/// or compares for that leaf.
pub fn scalar_byte_size(kind: Scalar) -> usize {
    match kind {
        Scalar::U8 | Scalar::I8 | Scalar::Bool => 1,
        Scalar::U16 | Scalar::I16 => 2,
        Scalar::U32 | Scalar::I32 | Scalar::F32 => 4,
        Scalar::U64 | Scalar::I64 | Scalar::F64 => 8,
    }
}

/// The result of a whole-namespace leaf enumeration ([`DwarfMap::enumerate_leaves`]).
/// `paths` are traceable scalar/enum leaves in the resolver's path syntax
/// (`var.member[i].field`); the counters report what the exclusion policy dropped.
#[derive(Default)]
pub struct LeafEnumeration {
    /// Every traceable leaf path (scalars, expanded struct members + array
    /// elements), sorted deterministically by top-level variable then walk order.
    pub paths: Vec<String>,
    /// Arrays skipped whole: over the element-count threshold (and not
    /// include-forced), multi-dimensional, or of unknown length.
    pub excluded_arrays: usize,
    /// Leaves skipped because their type is not a traceable scalar/enum (pointer,
    /// function, opaque/forward-declared aggregate, unsupported base type).
    pub skipped_leaves: usize,
    /// A recursion depth / leaf-budget safety cap was hit (walk truncated).
    pub capped: bool,
}

/// Recursion depth safety cap (nested struct/array). Firmware aggregates are
/// shallow; this only guards against pathological/looping DWARF.
const ENUM_DEPTH_CAP: u32 = 64;
/// Leaf-count safety cap: stop enumerating well before any realistic namespace
/// would exhaust memory. Hitting it sets [`LeafEnumeration::capped`].
const ENUM_LEAF_BUDGET: usize = 200_000;

/// An enumeration type: its underlying byte size + numeric-value → name map.
#[derive(Default)]
struct EnumInfo {
    size: u64,
    values: HashMap<i64, String>,
}

/// One piece of a composite variable location: `size` bytes, stored at `addr`,
/// or storage-less (`None` — optimized out, or struct padding) when the piece
/// carries no location.
#[derive(Clone, Debug)]
struct VarPiece {
    size: u64,
    addr: Option<u64>,
}

/// A static's link-time location: one whole-object address, or the composite
/// piece list an optimizer emits for an SRA-decomposed aggregate (observed on
/// macOS clang `-O3`, e.g. `lib_timer_data`: pointer member folded away, the
/// live members at discontiguous piece addresses).
#[derive(Clone, Debug)]
enum VarLoc {
    Addr(u64),
    Pieces(Vec<VarPiece>),
}

#[derive(Default)]
struct Maps {
    /// variable name -> (link location, type's global .debug_info offset)
    vars: HashMap<String, (VarLoc, usize)>,
    /// function name -> link-time entry address (`DW_AT_low_pc`). Only defining
    /// subprograms (with a `low_pc`) are recorded; declarations / abstract
    /// (inlined) instances are skipped. Backs the function-DIE ASLR anchor
    /// fallback for images (ELF + LTO) whose exported data symbols never appear
    /// in the DWARF variable map.
    functions: HashMap<String, u64>,
    /// struct/union type offset -> (member name -> (member offset, member type offset))
    members: HashMap<usize, HashMap<String, (u64, usize)>>,
    /// typedef/const/volatile/restrict offset -> the type it wraps
    underlying: HashMap<usize, usize>,
    /// array type offset -> element type offset
    arrays: HashMap<usize, usize>,
    /// array type offset -> per-dimension element counts (one per
    /// `DW_TAG_subrange_type` child, source order). The single-index resolver only
    /// handles 1-D, so multi-dimensional arrays are enumeration-excluded.
    array_dims: HashMap<usize, Vec<u64>>,
    /// any type offset -> byte size (for member/array address arithmetic)
    sizes: HashMap<usize, u64>,
    /// base type offset -> (DW_ATE encoding, byte size)
    base: HashMap<usize, (u8, u64)>,
    /// enumeration type offset -> its size + value→name map
    enums: HashMap<usize, EnumInfo>,
}

pub struct DwarfMap(Maps);

impl DwarfMap {
    /// Load the firmware's DWARF given the shared-library path. ELF/PE embed it
    /// in the image; macOS ld64 leaves it in a sibling `.dSYM` bundle (produced
    /// by `dsymutil`), so fall back to that when the image itself carries none.
    pub fn from_lib_path(lib: &Path) -> Result<Self, Box<dyn Error>> {
        let image = std::fs::read(lib)?;
        if image_has_dwarf(&image) {
            return Self::parse(&image);
        }
        let dsym = dsym_dwarf_path(lib).filter(|p| p.exists()).ok_or_else(|| {
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
    pub fn parse(bytes: &[u8]) -> Result<Self, Box<dyn Error>> {
        let object = object::File::parse(bytes)?;

        let load = |id: gimli::SectionId| -> Result<Cow<[u8]>, gimli::Error> {
            Ok(section_data(&object, id))
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

    /// Link-time address of a top-level variable. A piece-composed (SRA'd)
    /// variable has no single whole-object address, so it yields `None` — it
    /// cannot serve as an ASLR anchor.
    pub fn var_addr(&self, name: &str) -> Option<u64> {
        match self.0.vars.get(name) {
            Some((VarLoc::Addr(addr), _)) => Some(*addr),
            _ => None,
        }
    }

    /// Link-time entry address (`DW_AT_low_pc`) of a function. Basis matches
    /// [`var_addr`](Self::var_addr) — a file-relative vaddr in a PIC image — so the
    /// same ASLR `slide` (`runtime - link`) applies to either anchor kind.
    pub fn func_addr(&self, name: &str) -> Option<u64> {
        self.0.functions.get(name).copied()
    }

    /// Count of top-level variables carrying a link address (diagnostics).
    pub fn var_count(&self) -> usize {
        self.0.vars.len()
    }

    /// Count of defining functions carrying a `low_pc` (diagnostics).
    pub fn func_count(&self) -> usize {
        self.0.functions.len()
    }

    /// Resolve a `var[.member|[index]]...` path to (link address, scalar leaf
    /// kind). Returns None if any segment, member, index type, or leaf kind is
    /// unknown. The walk accumulates a byte offset within the variable, then
    /// maps it through the variable's location — a plain base+offset for a
    /// whole-object address, or the covering piece for a piece-composed static
    /// (None if the leaf lands in a storage-less piece or spans pieces).
    pub fn resolve(&self, path: &str) -> Option<(u64, Leaf)> {
        let mut segs = path.split('.');

        let (name, indices) = split_indices(segs.next()?)?;
        let (loc, ty) = self.0.vars.get(name)?;
        let mut off = 0u64;
        let mut ty = self.peel(*ty);
        ty = self.index(&mut off, ty, &indices)?;

        for seg in segs {
            let (name, indices) = split_indices(seg)?;
            let &(moff, mty) = self.0.members.get(&ty)?.get(name)?;
            off += moff;
            ty = self.peel(mty);
            ty = self.index(&mut off, ty, &indices)?;
        }

        let leaf = if self.0.enums.contains_key(&ty) {
            Leaf::Enum(ty)
        } else {
            Leaf::Scalar(self.scalar_kind(ty)?)
        };
        let addr = match loc {
            VarLoc::Addr(base) => base + off,
            VarLoc::Pieces(pieces) => piece_addr(pieces, off, *self.0.sizes.get(&ty)?)?,
        };
        Some((addr, leaf))
    }

    /// Enumerate **every traceable leaf** under every firmware `static`: scalars,
    /// recursively-expanded struct/union members, and expanded array elements,
    /// each as a path in the resolver's syntax (`var.member[i].field`) so
    /// [`resolve`](Self::resolve) accepts it verbatim.
    ///
    /// **Exclusion policy.** An array over `threshold` elements is skipped whole
    /// (drops task stacks, `ucHeap`, large scratch buffers) — unless an `include`
    /// prefix reaches into it, forcing just the reached element(s). Multi-dimensional
    /// and unknown-length arrays are skipped whole (resolver is 1-D only). Non-scalar
    /// leaves (pointers, functions, opaque aggregates) are skipped and counted.
    pub fn enumerate_leaves(&self, threshold: usize, includes: &[String]) -> LeafEnumeration {
        let mut ctx = EnumCtx {
            threshold,
            includes,
            out: Vec::new(),
            excluded_arrays: 0,
            skipped_leaves: 0,
            capped: false,
        };
        // Deterministic top-level order.
        let mut names: Vec<&String> = self.0.vars.keys().collect();
        names.sort();
        for name in names {
            let ty = self.0.vars[name].1;
            self.walk_leaves(name.clone(), ty, 0, &mut ctx);
        }
        LeafEnumeration {
            paths: ctx.out,
            excluded_arrays: ctx.excluded_arrays,
            skipped_leaves: ctx.skipped_leaves,
            capped: ctx.capped,
        }
    }

    /// Recursive leaf walk for [`enumerate_leaves`](Self::enumerate_leaves).
    fn walk_leaves(&self, path: String, ty: usize, depth: u32, ctx: &mut EnumCtx) {
        ctx.capped |= (depth > ENUM_DEPTH_CAP) || (ctx.out.len() >= ENUM_LEAF_BUDGET);
        if ctx.capped {
            return;
        }
        let ty = self.peel(ty);

        // Struct / union: recurse members in offset order (deterministic).
        if let Some(members) = self.0.members.get(&ty) {
            let mut ms: Vec<(&String, &(u64, usize))> = members.iter().collect();
            ms.sort_by(|(an, &(aoff, _)), (bn, &(boff, _))| (aoff, an).cmp(&(boff, bn)));
            for (name, (_off, mty)) in ms {
                self.walk_leaves(format!("{path}.{name}"), *mty, depth + 1, ctx);
            }
            return;
        }

        // Array: expand elements, applying the threshold / include policy.
        if let Some(&elem) = self.0.arrays.get(&ty) {
            let dims = match self.0.array_dims.get(&ty) {
                Some(dims) if !dims.is_empty() && dims.iter().all(|&d| d > 0) => dims.clone(),
                _ => {
                    // Unknown-length array: not indexable, so exclude whole.
                    ctx.excluded_arrays += 1;
                    return;
                }
            };
            let total: u64 = dims.iter().product();
            // A multi-dimensional array is never auto-mirrored: the resolver
            // addresses `[i][j]` row-major, but expanding the whole grid would bloat
            // the mirror, so only the element(s) an include reaches are kept.
            let force = ((total as usize) > ctx.threshold) || (dims.len() > 1);
            if force && !wanted(&path, ctx.includes) {
                ctx.excluded_arrays += 1;
                return;
            }
            let elem = self.peel(elem);
            let mut idx = vec![0u64; dims.len()];
            loop {
                let mut child = path.clone();
                for &i in &idx {
                    child.push('[');
                    child.push_str(&i.to_string());
                    child.push(']');
                }
                // On a force-expanded (over-threshold or multi-dim) array, keep only
                // the element(s) an include actually reaches.
                if !force || wanted(&child, ctx.includes) {
                    self.walk_leaves(child, elem, depth + 1, ctx);
                    if ctx.capped {
                        return;
                    }
                }
                // Row-major increment, last dimension fastest.
                let mut d = dims.len();
                loop {
                    if d == 0 {
                        return;
                    }
                    d -= 1;
                    idx[d] += 1;
                    if idx[d] < dims[d] {
                        break;
                    }
                    idx[d] = 0;
                }
            }
        }

        // Leaf: a traceable scalar or enum, else a skipped non-data type.
        if self.0.enums.contains_key(&ty) || self.scalar_kind(ty).is_some() {
            ctx.out.push(path);
        } else {
            ctx.skipped_leaves += 1;
        }
    }

    /// The byte size of an enum type.
    pub fn enum_size(&self, off: usize) -> Option<u64> {
        self.0.enums.get(&off).map(|e| e.size)
    }

    /// The enumerator name for a numeric value.
    pub fn enum_name(&self, off: usize, value: i64) -> Option<&str> {
        self.0
            .enums
            .get(&off)?
            .values
            .get(&value)
            .map(String::as_str)
    }

    /// The numeric value for an enumerator name (for writes).
    pub fn enum_value(&self, off: usize, name: &str) -> Option<i64> {
        let e = self.0.enums.get(&off)?;
        e.values.iter().find(|(_, n)| *n == name).map(|(&v, _)| v)
    }

    /// Every (value, name) enumerator of an enum type, sorted by value.
    pub fn enumerators(&self, off: usize) -> Option<Vec<(i64, String)>> {
        let e = self.0.enums.get(&off)?;
        let mut out: Vec<(i64, String)> = e.values.iter().map(|(&v, n)| (v, n.clone())).collect();
        out.sort_unstable_by_key(|&(v, _)| v);
        Some(out)
    }

    /// Apply `[i][j]...` to an array type, advancing the byte offset and returning
    /// the (peeled) element type. A flat multi-dimensional array (GCC emits one
    /// `array_type` with N `subrange` children) consumes N indices row-major against
    /// its extents; a 1-D array (or one with no recorded dims) consumes one.
    fn index(&self, addr: &mut u64, mut ty: usize, indices: &[usize]) -> Option<usize> {
        let mut consumed = 0usize;
        while consumed < indices.len() {
            let elem = self.peel(*self.0.arrays.get(&ty)?);
            let elem_size = *self.0.sizes.get(&elem)?;
            let ndims = self.0.array_dims.get(&ty).map_or(1, |d| d.len().max(1));
            if ndims > 1 {
                let dims = &self.0.array_dims[&ty];
                if (indices.len() - consumed) < ndims {
                    return None;
                }
                let mut flat = 0u64;
                for k in 0..ndims {
                    flat = (flat * dims[k]) + (indices[consumed + k] as u64);
                }
                *addr += flat * elem_size;
                consumed += ndims;
            } else {
                *addr += (indices[consumed] as u64) * elem_size;
                consumed += 1;
            }
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
            e if e == gimli::DW_ATE_unsigned.0 || e == gimli::DW_ATE_unsigned_char.0 => {
                match size {
                    1 => Scalar::U8,
                    2 => Scalar::U16,
                    4 => Scalar::U32,
                    8 => Scalar::U64,
                    _ => return None,
                }
            }
            _ => return None,
        })
    }

    /// The byte size of a resolved [`Leaf`] — a scalar's fixed width, or an
    /// enum's DWARF `byte_size` (`None` for an unknown enum offset).
    pub fn leaf_size(&self, leaf: Leaf) -> Option<usize> {
        match leaf {
            Leaf::Scalar(kind) => Some(scalar_byte_size(kind)),
            Leaf::Enum(off) => self.enum_size(off).map(|s| s as usize),
        }
    }

    /// Build a [`DwarfMap`] with only the var/func address maps populated — for
    /// anchor-selection tests in consumer crates (which cannot reach the private
    /// [`Maps`]). Types are irrelevant to the anchor (only the address delta matters).
    #[doc(hidden)]
    pub fn for_anchor_test(vars: &[(&str, u64)], funcs: &[(&str, u64)]) -> Self {
        let mut m = Maps::default();
        for (n, a) in vars {
            m.vars.insert((*n).to_string(), (VarLoc::Addr(*a), 0));
        }
        for (n, a) in funcs {
            m.functions.insert((*n).to_string(), *a);
        }
        DwarfMap(m)
    }
}

/// Mutable state threaded through the recursive leaf walk.
struct EnumCtx<'a> {
    threshold: usize,
    includes: &'a [String],
    out: Vec<String>,
    excluded_arrays: usize,
    skipped_leaves: usize,
    capped: bool,
}

/// Whether `x` names an ancestor-or-equal of `y` in the path syntax: `y == x`,
/// or `y` continues `x` with a `.member` or `[index]` step.
fn is_ancestor_or_eq(x: &str, y: &str) -> bool {
    (y == x)
        || (y.len() > x.len() && y.starts_with(x) && matches!(y.as_bytes()[x.len()], b'.' | b'['))
}

/// Whether the include list "wants" `path`: some include is an ancestor of
/// `path` (include the whole subtree) **or** `path` is an ancestor of some
/// include (descend toward a specifically-included leaf). Drives the
/// over-threshold array force-expand.
fn wanted(path: &str, includes: &[String]) -> bool {
    includes
        .iter()
        .any(|inc| is_ancestor_or_eq(inc, path) || is_ancestor_or_eq(path, inc))
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

        let goff = entry
            .offset()
            .to_debug_info_offset(&unit.header)
            .map(|o| o.0);
        let tag = entry.tag();

        match tag {
            gimli::DW_TAG_variable => {
                if let (Some(name), Some(loc), Some(ty)) = (
                    die_name(dwarf, unit, entry),
                    die_loc(dwarf, unit, entry),
                    type_goff(unit, entry),
                ) {
                    maps.vars.insert(name, (loc, ty));
                }
            }
            gimli::DW_TAG_subprogram => {
                // Only a defining subprogram carries a `low_pc`; declarations and
                // abstract (inlined) instances have none and are skipped by the
                // `die_low_pc` -> None guard. name+low_pc is all the anchor needs.
                if let (Some(name), Some(addr)) =
                    (die_name(dwarf, unit, entry), die_low_pc(dwarf, unit, entry))
                {
                    maps.functions.insert(name, addr);
                }
            }
            gimli::DW_TAG_member => {
                if let Some(&(_, parent, ptag)) = stack.last() {
                    if matches!(
                        ptag,
                        gimli::DW_TAG_structure_type | gimli::DW_TAG_union_type
                    ) {
                        if let (Some(name), Some(off), Some(ty)) = (
                            die_name(dwarf, unit, entry),
                            member_offset(entry),
                            type_goff(unit, entry),
                        ) {
                            maps.members
                                .entry(parent)
                                .or_default()
                                .insert(name, (off, ty));
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
                    // Ensure a dims vec exists even for a 0-subrange (flexible)
                    // array, so enumeration treats it as unknown-length.
                    maps.array_dims.entry(g).or_default();
                }
            }
            gimli::DW_TAG_subrange_type => {
                // A subrange is a dimension of its enclosing array_type; append
                // its element count (in source order) to that array's dims.
                if let Some(&(_, parent, ptag)) = stack.last() {
                    if ptag == gimli::DW_TAG_array_type {
                        if let Some(len) = subrange_len(entry) {
                            maps.array_dims.entry(parent).or_default().push(len);
                        }
                    }
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
                            maps.enums
                                .entry(parent)
                                .or_default()
                                .values
                                .insert(val, name);
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

/// A static's memory location from its `DW_AT_location` exprloc; anything
/// else (frame/register locals, loclists) yields `None`. Producer shapes:
/// see [`eval_static_loc`].
fn die_loc(
    dwarf: &gimli::Dwarf<Slice>,
    unit: &gimli::Unit<Slice>,
    entry: &gimli::DebuggingInformationEntry<Slice>,
) -> Option<VarLoc> {
    match entry.attr_value(gimli::DW_AT_location).ok().flatten()? {
        gimli::AttributeValue::Exprloc(expr) => eval_static_loc(dwarf, unit, expr),
        _ => None,
    }
}

/// Fully evaluate a location expression to memory storage — one address or a
/// byte-granular piece list; `None` for anything that is not plain static
/// storage. Fully evaluating (never first-op matching) covers the three
/// producer shapes: ELF/PE lone `DW_OP_addr`; dsymutil's shared-anchor
/// `DW_OP_addr + DW_OP_plus_uconst`; `-O3` SRA `DW_OP_piece` composites
/// (storage-less pieces keep their extent). `DW_OP_addrx` resolves via
/// `.debug_addr`.
fn eval_static_loc(
    dwarf: &gimli::Dwarf<Slice>,
    unit: &gimli::Unit<Slice>,
    expr: gimli::Expression<Slice>,
) -> Option<VarLoc> {
    let mut eval = expr.evaluation(unit.encoding());
    let mut result = eval.evaluate().ok()?;
    loop {
        match result {
            gimli::EvaluationResult::Complete => break,
            // DW_OP_addr: our addresses are already final — resume unchanged.
            gimli::EvaluationResult::RequiresRelocatedAddress(addr) => {
                result = eval.resume_with_relocated_address(addr).ok()?;
            }
            gimli::EvaluationResult::RequiresIndexedAddress { index, .. } => {
                let addr = dwarf.address(unit, index).ok()?;
                result = eval.resume_with_indexed_address(addr).ok()?;
            }
            _ => return None,
        }
    }
    match eval.result().as_slice() {
        // A whole-object (unsized) single piece at an address: the common case.
        [piece] if piece.size_in_bits.is_none() => match piece.location {
            gimli::Location::Address { address } => Some(VarLoc::Addr(address)),
            _ => None,
        },
        [] => None,
        // A DW_OP_piece composite: keep each piece's byte size + storage address
        // (storage-less pieces keep their extent so later pieces stay aligned).
        pieces => {
            let mut out = Vec::with_capacity(pieces.len());
            for piece in pieces {
                let bits = piece.size_in_bits?;
                if ((bits % 8) != 0) || piece.bit_offset.is_some() {
                    return None;
                }
                let addr = match piece.location {
                    gimli::Location::Address { address } => Some(address),
                    gimli::Location::Empty => None,
                    _ => return None,
                };
                out.push(VarPiece {
                    size: bits / 8,
                    addr,
                });
            }
            Some(VarLoc::Pieces(out))
        }
    }
}

/// Map a byte range `[off, off+size)` within a piece-composed variable to its
/// storage address. The leaf must lie wholly inside one addressed piece; a
/// storage-less piece (member folded away by the optimizer) or a piece-spanning
/// range yields `None`.
fn piece_addr(pieces: &[VarPiece], off: u64, size: u64) -> Option<u64> {
    let mut start = 0u64;
    let mut found = None;
    for piece in pieces {
        let end = start + piece.size;
        if (off >= start) && ((off + size) <= end) {
            found = piece.addr.map(|a| a + (off - start));
            break;
        }
        start = end;
    }
    found
}

/// A subprogram's link-time entry address from `DW_AT_low_pc`. Handles both the
/// DWARF≤4 absolute form (`DW_FORM_addr` → `AttributeValue::Addr`) and the DWARF 5
/// indexed form (`DW_FORM_addrx` → `DebugAddrIndex`, resolved via `.debug_addr`),
/// since GCC 15 defaults to DWARF 5. Any other form (or a subprogram without a
/// `low_pc` — a declaration / abstract instance) yields `None`.
fn die_low_pc(
    dwarf: &gimli::Dwarf<Slice>,
    unit: &gimli::Unit<Slice>,
    entry: &gimli::DebuggingInformationEntry<Slice>,
) -> Option<u64> {
    match entry.attr_value(gimli::DW_AT_low_pc).ok().flatten()? {
        gimli::AttributeValue::Addr(a) => Some(a),
        gimli::AttributeValue::DebugAddrIndex(index) => dwarf.address(unit, index).ok(),
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
    entry
        .attr_value(gimli::DW_AT_byte_size)
        .ok()
        .flatten()?
        .udata_value()
}

fn encoding(entry: &gimli::DebuggingInformationEntry<Slice>) -> Option<u8> {
    match entry.attr_value(gimli::DW_AT_encoding).ok().flatten()? {
        gimli::AttributeValue::Encoding(e) => Some(e.0),
        other => other.udata_value().map(|u| u as u8),
    }
}

/// An array dimension's element count from a `DW_TAG_subrange_type`:
/// `DW_AT_count` directly, else `DW_AT_upper_bound + 1`. `None` for an
/// unbounded (flexible) array member.
fn subrange_len(entry: &gimli::DebuggingInformationEntry<Slice>) -> Option<u64> {
    if let Some(c) = entry.attr_value(gimli::DW_AT_count).ok().flatten() {
        return c.udata_value();
    }
    entry
        .attr_value(gimli::DW_AT_upper_bound)
        .ok()
        .flatten()?
        .udata_value()
        .map(|u| u + 1)
}

/// An enumerator's `DW_AT_const_value` as an i64 (signed first, else unsigned).
fn const_value(entry: &gimli::DebuggingInformationEntry<Slice>) -> Option<i64> {
    let v = entry.attr_value(gimli::DW_AT_const_value).ok().flatten()?;
    v.sdata_value()
        .or_else(|| v.udata_value().map(|u| u as i64))
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

#[cfg(test)]
mod tests {
    use super::*;

    /// dsymutil relocates same-TU statics as `DW_OP_addr <anchor> ;
    /// DW_OP_plus_uconst <addend>`, several sharing one anchor — the reader
    /// must evaluate the full expression or the group aliases. Fixture: real
    /// arm64 dSYM DWARF (`tests/fixtures/`); the true, distinct addresses:
    fn macho_dsym_static_addresses() -> Vec<(&'static str, u64)> {
        vec![
            ("taskUsbRuns", 0x14000),
            ("task1msRuns", 0x14008),
            ("task10msRuns", 0x1400c),
            ("telemRuns", 0x14010),
            ("task200msRuns", 0x14014),
        ]
    }

    #[test]
    fn macho_dsym_same_tu_statics_do_not_collapse() {
        let path = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("tests/fixtures/macho_static_collapse.dSYM.dwarf");
        let bytes = std::fs::read(&path).expect("read dSYM DWARF fixture");
        let map = DwarfMap::parse(&bytes).expect("parse dSYM DWARF");

        let expected = macho_dsym_static_addresses();
        // Each collapsing static resolves to its own true address (the
        // DW_OP_plus_uconst addend applied, not dropped).
        for (name, addr) in &expected {
            assert_eq!(map.var_addr(name), Some(*addr), "{name} address");
        }
        // And no two of them alias — the collapse would violate this.
        let mut addrs: Vec<u64> = expected.iter().map(|(_, a)| *a).collect();
        addrs.sort_unstable();
        addrs.dedup();
        assert_eq!(
            addrs.len(),
            expected.len(),
            "static addresses must be distinct"
        );
    }

    /// At `-O3`, SRA decomposes an aggregate static into a `DW_OP_piece`
    /// composite (some pieces storage-less). Fixture: real arm64 dSYM DWARF;
    /// members resolve at their piece addresses, folded members and the
    /// whole-object anchor do not.
    #[test]
    fn macho_dsym_sra_pieced_static_resolves_members() {
        let path = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("tests/fixtures/macho_sra_piece_location.dSYM.dwarf");
        let bytes = std::fs::read(&path).expect("read dSYM DWARF fixture");
        let map = DwarfMap::parse(&bytes).expect("parse dSYM DWARF");

        // Live members land on their piece addresses (base 0x36488, u64 at +8).
        let addr = |p: &str| map.resolve(p).map(|(a, _)| a);
        assert_eq!(addr("lib_timer_data.lastTime_u32"), Some(0x36488));
        assert_eq!(addr("lib_timer_data.currentTime_us"), Some(0x36490));
        // The folded-away `config` pointer piece is storage-less: no address.
        assert_eq!(addr("lib_timer_data.config"), None);
        // No single whole-object address, so the var can't anchor ASLR.
        assert_eq!(map.var_addr("lib_timer_data"), None);
    }

    #[test]
    fn pieced_var_members_resolve_through_their_pieces() {
        // The lib_timer_data shape: { ptr@0 folded, u32@8, pad@12, u64@16 } as
        // [8 storage-less][4 @0x100][4 storage-less][8 @0x108].
        let mut m = Maps::default();
        m.base.insert(10, (gimli::DW_ATE_unsigned.0, 4));
        m.sizes.insert(10, 4);
        m.base.insert(11, (gimli::DW_ATE_unsigned.0, 8));
        m.sizes.insert(11, 8);
        m.sizes.insert(12, 8); // pointer type: sized but not a scalar leaf
        let mut members = HashMap::new();
        members.insert("config".to_string(), (0u64, 12usize));
        members.insert("lastTime_u32".to_string(), (8, 10));
        members.insert("currentTime_us".to_string(), (16, 11));
        m.members.insert(100, members);
        let pieces = vec![
            VarPiece {
                size: 8,
                addr: None,
            },
            VarPiece {
                size: 4,
                addr: Some(0x100),
            },
            VarPiece {
                size: 4,
                addr: None,
            },
            VarPiece {
                size: 8,
                addr: Some(0x108),
            },
        ];
        m.vars
            .insert("t".to_string(), (VarLoc::Pieces(pieces), 100));
        let dw = DwarfMap(m);

        let addr = |p: &str| dw.resolve(p).map(|(a, _)| a);
        assert_eq!(addr("t.lastTime_u32"), Some(0x100));
        assert_eq!(addr("t.currentTime_us"), Some(0x108));
        assert_eq!(addr("t.config"), None); // storage-less piece
        assert_eq!(dw.var_addr("t"), None); // pieced var is not an anchor
    }

    #[test]
    fn func_addr_looks_up_subprogram_low_pc() {
        let dw = DwarfMap::for_anchor_test(&[("g", 0x10)], &[("sil_fw_start", 0xBEEF)]);
        assert_eq!(dw.func_addr("sil_fw_start"), Some(0xBEEF));
        assert_eq!(dw.func_addr("g"), None); // a variable is not a function
        assert_eq!(dw.func_addr("missing"), None);
        assert_eq!((dw.var_count(), dw.func_count()), (1, 1));
    }

    /// Build a synthetic [`DwarfMap`] exercising the leaf walk without a real DLL:
    /// a scalar `n`, a struct `s { a; arr[3]; big[100] }`, and a 2-D `grid[2][3]`.
    fn synthetic() -> DwarfMap {
        let mut m = Maps::default();
        // base u32 @10
        m.base.insert(10, (gimli::DW_ATE_unsigned.0, 4));
        m.sizes.insert(10, 4);
        // small array @20 (elem u32, 3 elems)
        m.arrays.insert(20, 10);
        m.array_dims.insert(20, vec![3]);
        // big array @30 (elem u32, 100 elems — over threshold)
        m.arrays.insert(30, 10);
        m.array_dims.insert(30, vec![100]);
        // 2-D array @40 (elem u32, 2x3 — multi-dim)
        m.arrays.insert(40, 10);
        m.array_dims.insert(40, vec![2, 3]);
        // struct S @100 { a@0:u32, arr@4:arr20, big@8:arr30 }
        let mut members = HashMap::new();
        members.insert("a".to_string(), (0u64, 10usize));
        members.insert("arr".to_string(), (4, 20));
        members.insert("big".to_string(), (8, 30));
        m.members.insert(100, members);
        // vars
        m.vars.insert("n".to_string(), (VarLoc::Addr(0x1000), 10));
        m.vars.insert("s".to_string(), (VarLoc::Addr(0x2000), 100));
        m.vars
            .insert("grid".to_string(), (VarLoc::Addr(0x3000), 40));
        DwarfMap(m)
    }

    #[test]
    fn enumerates_scalars_structs_arrays_with_threshold() {
        let dw = synthetic();
        let en = dw.enumerate_leaves(32, &[]);
        assert_eq!(
            en.paths,
            vec![
                "n".to_string(),
                "s.a".to_string(),
                "s.arr[0]".to_string(),
                "s.arr[1]".to_string(),
                "s.arr[2]".to_string(),
            ]
        );
        // grid (multi-dim) + s.big (over threshold) both excluded whole.
        assert_eq!(en.excluded_arrays, 2);
        assert_eq!(en.skipped_leaves, 0);
        assert!(!en.capped);
    }

    #[test]
    fn include_forces_a_single_over_threshold_element() {
        let dw = synthetic();
        let en = dw.enumerate_leaves(32, &["s.big[1]".to_string()]);
        // Exactly the one included element joins; the rest of big stays excluded.
        assert!(en.paths.contains(&"s.big[1]".to_string()));
        assert!(!en.paths.contains(&"s.big[0]".to_string()));
        assert!(!en.paths.contains(&"s.big[2]".to_string()));
        // Only grid remains excluded now (big was force-expanded).
        assert_eq!(en.excluded_arrays, 1);
    }

    #[test]
    fn include_whole_array_prefix_expands_all_elements() {
        let dw = synthetic();
        let en = dw.enumerate_leaves(32, &["s.big".to_string()]);
        let big_leaves = en.paths.iter().filter(|p| p.starts_with("s.big[")).count();
        assert_eq!(big_leaves, 100);
    }

    #[test]
    fn include_forces_a_multidim_element() {
        let dw = synthetic();
        let en = dw.enumerate_leaves(32, &["grid[1][2]".to_string()]);
        // Exactly the one reached element joins; no other grid cell does.
        assert!(en.paths.contains(&"grid[1][2]".to_string()));
        assert_eq!(
            en.paths.iter().filter(|p| p.starts_with("grid[")).count(),
            1
        );
        // s.big stays excluded; grid is now force-expanded to its one element.
        assert_eq!(en.excluded_arrays, 1);
    }

    #[test]
    fn resolve_addresses_a_flat_multidim_element() {
        let dw = synthetic();
        // grid[2][3] of u32 @0x3000: [1][2] is row-major offset (1*3 + 2)*4 = 20.
        let (addr, leaf) = dw.resolve("grid[1][2]").expect("grid[1][2] resolves");
        assert_eq!(addr, 0x3000 + 20);
        assert!(matches!(leaf, Leaf::Scalar(Scalar::U32)));
        // A partial index (fewer than the array's dimensions) does not resolve.
        assert!(dw.resolve("grid[1]").is_none());
    }

    #[test]
    fn enumerators_list_sorted_by_value() {
        let mut m = Maps::default();
        let mut e = EnumInfo {
            size: 1,
            ..Default::default()
        };
        e.values.insert(2, "FAULT".to_string());
        e.values.insert(0, "IDLE".to_string());
        e.values.insert(1, "RUN".to_string());
        m.enums.insert(50, e);
        m.sizes.insert(50, 1);
        m.vars
            .insert("mode".to_string(), (VarLoc::Addr(0x4000), 50));
        let dw = DwarfMap(m);

        // The variable resolves as an enum leaf carrying its type offset...
        let (_, leaf) = dw.resolve("mode").expect("mode resolves");
        let Leaf::Enum(off) = leaf else {
            panic!("mode must be an enum leaf")
        };
        // ...whose full enumerator list comes back value-sorted, agreeing with
        // the per-value lookup.
        assert_eq!(
            dw.enumerators(off),
            Some(vec![
                (0, "IDLE".to_string()),
                (1, "RUN".to_string()),
                (2, "FAULT".to_string()),
            ])
        );
        assert_eq!(dw.enum_name(off, 2), Some("FAULT"));
        assert_eq!(dw.enumerators(10), None); // not an enum type
    }

    #[test]
    fn ancestor_predicate() {
        assert!(is_ancestor_or_eq("a.b", "a.b"));
        assert!(is_ancestor_or_eq("a.b", "a.b.c"));
        assert!(is_ancestor_or_eq("a.b", "a.b[0]"));
        assert!(!is_ancestor_or_eq("a.b", "a.bc")); // not a boundary
        assert!(!is_ancestor_or_eq("a.b.c", "a.b"));
    }
}
