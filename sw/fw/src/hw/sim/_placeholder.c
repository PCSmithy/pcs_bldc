// Placeholder source so the native (SIL) `fw_hw` library can be STATIC,
// matching the embedded side. Per-module project channel configs
// (HW_*_config.c) attach to fw_hw via target_sources(fw_hw PRIVATE ...)
// from sibling CMakeLists.txt files; that requires fw_hw to be a real
// compiled target, not an INTERFACE library.
//
// Delete this file once sw/fw/src/hw/sim/ has any real sim
// infrastructure of its own (motor model, gate-driver model, etc.).

void _fw_hw_sim_placeholder(void) {}
