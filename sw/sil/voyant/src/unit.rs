//! Runtime unit registry: a linear (scale + offset) conversion table keyed by
//! unit name, relative to each dimension's base unit. Converting value `v` in
//! unit `U` to the dimension base is `v*scale + offset`; base back to `U` is
//! `(v - offset)/scale`. A cross-unit conversion goes through the base and
//! requires both units under the same dimension.
//!
//! Built-ins cover only what the sim consumes: dimension `"angle"`, base `rad`
//! (scale 1, offset 0) plus `deg` (scale π/180). [`add_unit`](UnitRegistry::add_unit)
//! extends the table at runtime.

use std::collections::HashMap;
use thiserror::Error;

/// One unit's linear relation to its dimension's base unit.
#[derive(Debug, Clone, PartialEq)]
struct UnitDef {
    dimension: String,
    scale: f64,
    offset: f64,
}

/// Failure from [`UnitRegistry::add_unit`].
#[derive(Debug, Clone, PartialEq, Error)]
pub enum UnitError {
    #[error("unit name {0:?} must not start with a digit")]
    DigitLeadingName(String),
    #[error("conflicting redefinition of unit {0:?} (existing definition differs)")]
    Conflict(String),
}

/// A name → (dimension, scale, offset) table with the built-in angle units.
pub struct UnitRegistry {
    units: HashMap<String, UnitDef>,
}

impl Default for UnitRegistry {
    fn default() -> Self {
        Self::new()
    }
}

impl UnitRegistry {
    /// A registry seeded with the built-in units (only what has consumers).
    pub fn new() -> Self {
        let mut units = HashMap::new();
        units.insert(
            "rad".to_string(),
            UnitDef { dimension: "angle".to_string(), scale: 1.0, offset: 0.0 },
        );
        units.insert(
            "deg".to_string(),
            UnitDef {
                dimension: "angle".to_string(),
                scale: std::f64::consts::PI / 180.0,
                offset: 0.0,
            },
        );
        Self { units }
    }

    /// Register a unit relative to its dimension's base. Rejects a name starting
    /// with an ASCII digit (that grammar is reserved for cvar array indices — see
    /// the bracket parse rule). Idempotent for an identical definition; a differing
    /// definition of the same name is a [`UnitError::Conflict`].
    pub fn add_unit(
        &mut self,
        name: &str,
        dimension: &str,
        scale: f64,
        offset: f64,
    ) -> Result<(), UnitError> {
        if name.as_bytes().first().is_some_and(u8::is_ascii_digit) {
            return Err(UnitError::DigitLeadingName(name.to_string()));
        }
        let def = UnitDef {
            dimension: dimension.to_string(),
            scale,
            offset,
        };
        match self.units.get(name) {
            Some(existing) if *existing == def => Ok(()),
            Some(_) => Err(UnitError::Conflict(name.to_string())),
            None => {
                self.units.insert(name.to_string(), def);
                Ok(())
            }
        }
    }

    /// Whether `name` is a registered unit.
    pub fn is_registered(&self, name: &str) -> bool {
        self.units.contains_key(name)
    }

    /// The dimension a unit belongs to, `None` if unregistered.
    pub fn dimension_of(&self, name: &str) -> Option<&str> {
        self.units.get(name).map(|d| d.dimension.as_str())
    }

    /// Convert `value` from unit `from` to unit `to` through the shared dimension
    /// base. `None` if either unit is unregistered; the caller pre-validates unit
    /// registration and dimension compatibility to surface precise errors.
    pub fn convert(&self, value: f64, from: &str, to: &str) -> Option<f64> {
        let from = self.units.get(from)?;
        let to = self.units.get(to)?;
        let base = value * from.scale + from.offset;
        Some((base - to.offset) / to.scale)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn builtin_angle_roundtrips_through_base() {
        let r = UnitRegistry::new();
        // 90 deg -> rad -> 90 deg.
        let rad = r.convert(90.0, "deg", "rad").unwrap();
        assert!((rad - std::f64::consts::FRAC_PI_2).abs() < 1e-12);
        let deg = r.convert(rad, "rad", "deg").unwrap();
        assert!((deg - 90.0).abs() < 1e-12);
    }

    #[test]
    fn add_unit_supports_an_offset_scale_pair() {
        // A °C/K style pair: base K, celsius = scale 1, offset 273.15.
        let mut r = UnitRegistry::new();
        r.add_unit("K", "temperature", 1.0, 0.0).unwrap();
        r.add_unit("degC", "temperature", 1.0, 273.15).unwrap();
        // 25 °C -> 298.15 K -> 25 °C.
        let k = r.convert(25.0, "degC", "K").unwrap();
        assert!((k - 298.15).abs() < 1e-9);
        let c = r.convert(k, "K", "degC").unwrap();
        assert!((c - 25.0).abs() < 1e-9);
    }

    #[test]
    fn add_unit_rejects_digit_leading_names() {
        let mut r = UnitRegistry::new();
        assert!(matches!(
            r.add_unit("2pi", "angle", 1.0, 0.0),
            Err(UnitError::DigitLeadingName(_))
        ));
    }

    #[test]
    fn add_unit_is_idempotent_but_rejects_conflict() {
        let mut r = UnitRegistry::new();
        r.add_unit("rpm", "angular_velocity", 0.104_719_755_119_66, 0.0).unwrap();
        // Identical redefinition is a no-op.
        r.add_unit("rpm", "angular_velocity", 0.104_719_755_119_66, 0.0).unwrap();
        // A differing definition conflicts.
        assert!(matches!(
            r.add_unit("rpm", "angular_velocity", 1.0, 0.0),
            Err(UnitError::Conflict(_))
        ));
    }

    #[test]
    fn convert_on_an_unregistered_unit_is_none() {
        let r = UnitRegistry::new();
        assert_eq!(r.convert(1.0, "deg", "nope"), None);
        assert_eq!(r.convert(1.0, "nope", "rad"), None);
    }
}
