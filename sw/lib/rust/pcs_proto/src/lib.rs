//! prost bindings for the board protocol — the `shared` envelope + framework
//! services, the `trace` feature, and this board's `board` extension
//! messages, generated at build time from the .proto sources (the single
//! source of truth). Wire framing lives in `pcs_wire`; this crate is the
//! payload codec.
//!
//! The nanopb `.options` bounds (max frame 448 B decoded payload, watch size
//! 1..8, Samples data <= 256 B) are C-side static-allocation limits prost
//! does not enforce — senders validate against them at the application layer.

pub mod shared {
    include!(concat!(env!("OUT_DIR"), "/shared.rs"));
}

pub mod trace {
    include!(concat!(env!("OUT_DIR"), "/trace.rs"));
}

pub mod board {
    include!(concat!(env!("OUT_DIR"), "/board.rs"));
}
