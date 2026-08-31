//! prost bindings for the board protocol (`shared` envelope, `trace`,
//! `board`), generated at build time from the .proto sources; framing lives
//! in `pcs_wire`. The nanopb `.options` bounds are C-side limits prost does
//! not enforce — senders validate at the application layer.

pub mod shared {
    include!(concat!(env!("OUT_DIR"), "/shared.rs"));
}

pub mod trace {
    include!(concat!(env!("OUT_DIR"), "/trace.rs"));
}

pub mod board {
    include!(concat!(env!("OUT_DIR"), "/board.rs"));
}
