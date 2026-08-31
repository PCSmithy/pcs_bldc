//! Generate the prost bindings from the schema split: the framework schema
//! (sw/lib/c/shared/proto) imports this board's schema (sw/proto), so both
//! directories are include paths — mirroring tools/generate_proto.sh and
//! pcs_client.py's ensure_bindings.

use std::path::Path;

fn main() -> Result<(), Box<dyn std::error::Error>> {
    // Canonical paths, so protox's lexical include-path prefix match works
    // regardless of `..` segments and host path separators.
    let root = Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("../../../..")
        .canonicalize()?;
    let shared = root.join("sw/lib/c/shared/proto");
    let board = root.join("sw/proto");
    let files = [
        shared.join("shared.proto"),
        shared.join("trace.proto"),
        board.join("board.proto"),
    ];
    for file in &files {
        println!("cargo:rerun-if-changed={}", file.display());
    }

    let descriptors = protox::compile(&files, [shared.as_path(), board.as_path()])?;
    prost_build::Config::new()
        // The GUI serializes TraceStatus straight into its "trace-status"
        // event; field names are the wire names either way.
        .type_attribute(".trace.TraceStatus", "#[derive(serde::Serialize)]")
        .compile_fds(descriptors)?;
    Ok(())
}
