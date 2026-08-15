//! The framework interrupt table (design: `docs/sil/sim-interrupts.md`): what turns
//! "this handler is due at time T" into a call into the firmware.
//!
//! [`IrqTable`] is pure scheduling data, owned per image by a
//! [`FirmwareMember`](crate::FirmwareMember); [`IrqRendezvous`] is the append-only
//! op log both registration paths (C upcalls, config-time by name) write, so one
//! handle allocator and one apply order serve both.
//!
//! - Due times are absolute sim-µs and exact: a step dispatches everything due at
//!   or before its own time, so a finer grid tightens quantization with no change here.
//! - Dispatch order is priority (lower first, the NVIC convention), then
//!   registration index — fully deterministic.
//! - A refused (masked) dispatch leaves the due time untouched: held pending, never
//!   dropped. Priority is ordering only — no nesting.

use std::cell::RefCell;

/// A registration handle. Returned by every registration path (C upcall or
/// config-time by name) and accepted by cancel / enable. Handles are never reused,
/// so a stale handle is inert rather than aliasing a later entry.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct IrqHandle(i32);

impl IrqHandle {
    /// The raw C-visible handle value (what `SIL_irq_register*` returned).
    pub fn raw(self) -> i32 {
        self.0
    }

    /// Wrap a handle the rendezvous just allocated (only ever a non-negative one).
    pub(crate) fn from_raw(raw: i32) -> Self {
        Self(raw)
    }
}

/// Whether an entry re-arms after firing.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum IrqKind {
    /// Fires every `rate_or_delay_us` of sim time, drift-free from registration.
    Periodic,
    /// Fires once, `rate_or_delay_us` after registration, then goes dead.
    OneShot,
}

/// One mutation of the interrupt table, queued by the C upcalls (or the config-time
/// path) and applied in queue order at the owning member's next step.
#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) enum IrqOp {
    Register {
        handle: i32,
        handler: usize,
        kind: IrqKind,
        rate_or_delay_us: u64,
        priority: u8,
    },
    Cancel {
        handle: i32,
    },
    SetEnabled {
        handle: i32,
        enabled: bool,
    },
}

/// One table entry — the design's entry structure plus the scheduling state the
/// controller keeps for it.
#[derive(Clone, Copy, Debug)]
struct IrqEntry {
    /// Handler address in the firmware image (a DWARF-resolved function for the
    /// config path, a pointer the sim driver handed up for the runtime path).
    handler: usize,
    kind: IrqKind,
    rate_or_delay_us: u64,
    /// Same-step dispatch order only; lower value first. No preemption.
    priority: u8,
    enabled: bool,
    /// Registration index — the tiebreak within one priority.
    index: usize,
    /// Absolute sim time this entry is next due (exact, unquantized).
    due_us: u64,
    /// False once cancelled or once a one-shot has fired. The slot stays so the
    /// handle is never reused.
    live: bool,
}

/// The framework-owned interrupt table for one firmware image.
#[derive(Default)]
pub(crate) struct IrqTable {
    /// Entries indexed by handle (handles are dense, allocated by the rendezvous).
    entries: Vec<IrqEntry>,
    /// Reusable due-list scratch, so the per-step scan allocates nothing.
    due_scratch: Vec<usize>,
    /// Periodic firings folded away because an entry came due more than once
    /// between dispatches — a period finer than the grid, or a long mask.
    coalesced: u64,
}

impl IrqTable {
    /// An empty table.
    pub(crate) fn new() -> Self {
        Self::default()
    }

    /// Firings folded away by coalescing (see the field docs). Diagnostic.
    pub(crate) fn coalesced(&self) -> u64 {
        self.coalesced
    }

    /// Whether any live, enabled entry is due at or before `now_us`.
    pub(crate) fn any_due(&self, now_us: u64) -> bool {
        self.entries
            .iter()
            .any(|e| e.live && e.enabled && (e.due_us <= now_us))
    }

    /// The handle of the first live entry whose handler is at `handler` — how the
    /// framework reaches an interrupt a sim driver registered by pointer.
    pub(crate) fn find_by_handler(&self, handler: usize) -> Option<IrqHandle> {
        self.entries
            .iter()
            .find(|e| e.live && (e.handler == handler))
            .map(|e| IrqHandle(e.index as i32))
    }

    /// Apply one queued op, scheduling a registration relative to `now_us` (a
    /// mid-step registration is relative to the current sim time). `Err` names a
    /// malformed op for the caller to log; the table is left untouched.
    pub(crate) fn apply(&mut self, op: IrqOp, now_us: u64) -> Result<(), String> {
        match op {
            IrqOp::Register {
                handle,
                handler,
                kind,
                rate_or_delay_us,
                priority,
            } => {
                let index = self.entries.len();
                if handle != (index as i32) {
                    return Err(format!(
                        "registration handle {handle} does not match table index {index}"
                    ));
                }
                if (kind == IrqKind::Periodic) && (rate_or_delay_us == 0) {
                    return Err("periodic interrupt with a zero period".to_string());
                }
                self.entries.push(IrqEntry {
                    handler,
                    kind,
                    rate_or_delay_us,
                    priority,
                    enabled: true,
                    index,
                    due_us: now_us.saturating_add(rate_or_delay_us),
                    live: true,
                });
                Ok(())
            }
            IrqOp::Cancel { handle } => {
                self.entry_mut(handle)?.live = false;
                Ok(())
            }
            IrqOp::SetEnabled { handle, enabled } => {
                self.entry_mut(handle)?.enabled = enabled;
                Ok(())
            }
        }
    }

    /// Dispatch every entry due at `now_us`, in priority-then-registration-index
    /// order, through `dispatch` (the port's ISR bracket). `dispatch` returns false
    /// when the firmware has interrupts masked: that entry keeps its due time and is
    /// re-attempted next step — held pending, never dropped. Returns how many
    /// handlers actually ran.
    pub(crate) fn dispatch_due(
        &mut self,
        now_us: u64,
        mut dispatch: impl FnMut(usize) -> bool,
    ) -> u64 {
        // Move the scratch out so the entry loop can mutate `self.entries`.
        let mut due = std::mem::take(&mut self.due_scratch);
        due.clear();
        due.extend(
            self.entries
                .iter()
                .filter(|e| e.live && e.enabled && (e.due_us <= now_us))
                .map(|e| e.index),
        );
        due.sort_unstable_by_key(|&i| (self.entries[i].priority, self.entries[i].index));

        let mut ran = 0u64;
        for &i in &due {
            if !dispatch(self.entries[i].handler) {
                continue; // masked: due time untouched, re-attempted next step
            }
            ran += 1;
            let entry = &mut self.entries[i];
            match entry.kind {
                IrqKind::OneShot => entry.live = false,
                IrqKind::Periodic => {
                    // Drift-free re-arm. Firings the grid (or a mask) swallowed are
                    // folded into one and counted — the grid is meant to be at least
                    // as fine as the fastest interrupt.
                    entry.due_us += entry.rate_or_delay_us;
                    while entry.due_us <= now_us {
                        entry.due_us += entry.rate_or_delay_us;
                        self.coalesced += 1;
                    }
                }
            }
        }

        self.due_scratch = due;
        ran
    }

    /// Clear the table — the reload path (a rebooted image re-registers from scratch).
    pub(crate) fn clear(&mut self) {
        self.entries.clear();
        self.due_scratch.clear();
        self.coalesced = 0;
    }

    fn entry_mut(&mut self, handle: i32) -> Result<&mut IrqEntry, String> {
        usize::try_from(handle)
            .ok()
            .and_then(|i| self.entries.get_mut(i))
            .ok_or_else(|| format!("unknown interrupt handle {handle}"))
    }
}

/// The append-only op log the registration paths write and the owning member drains.
///
/// The C trampolines target one of these (its address is the hooks' `context`), and
/// the config-time by-name path calls the very same methods — one handle allocator,
/// one ordering. Cloneable-free and `RefCell`-guarded: the sim is single-threaded
/// and C only calls in while firmware code runs, when no Rust borrow is live.
#[derive(Default)]
pub(crate) struct IrqRendezvous {
    inner: RefCell<IrqRendezvousInner>,
}

#[derive(Default)]
struct IrqRendezvousInner {
    ops: Vec<IrqOp>,
    next_handle: i32,
}

impl IrqRendezvous {
    /// Queue a registration and hand back its handle immediately (C needs it before
    /// the framework ever looks at the log).
    pub(crate) fn register(
        &self,
        handler: usize,
        kind: IrqKind,
        rate_or_delay_us: u64,
        priority: u8,
    ) -> i32 {
        let mut inner = self.inner.borrow_mut();
        let handle = inner.next_handle;
        inner.next_handle += 1;
        inner.ops.push(IrqOp::Register {
            handle,
            handler,
            kind,
            rate_or_delay_us,
            priority,
        });
        handle
    }

    pub(crate) fn cancel(&self, handle: i32) {
        self.inner.borrow_mut().ops.push(IrqOp::Cancel { handle });
    }

    pub(crate) fn set_enabled(&self, handle: i32, enabled: bool) {
        self.inner
            .borrow_mut()
            .ops
            .push(IrqOp::SetEnabled { handle, enabled });
    }

    /// Ops queued from index `from` onward. The log is append-only, so a consumer
    /// applies these and advances its cursor by the returned length.
    pub(crate) fn ops_since(&self, from: usize) -> Vec<IrqOp> {
        let inner = self.inner.borrow();
        inner.ops.get(from..).map(<[IrqOp]>::to_vec).unwrap_or_default()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::RefCell;

    /// Register through a rendezvous and apply the queued ops to a table, so the
    /// tests exercise the real (single-allocator) registration path.
    struct Harness {
        rv: IrqRendezvous,
        table: IrqTable,
        cursor: usize,
        /// Handlers dispatched so far, in order.
        fired: RefCell<Vec<usize>>,
        /// When true, every dispatch reports the firmware as masked.
        masked: bool,
    }

    impl Harness {
        fn new() -> Self {
            Self {
                rv: IrqRendezvous::default(),
                table: IrqTable::new(),
                cursor: 0,
                fired: RefCell::new(Vec::new()),
                masked: false,
            }
        }

        fn register(&self, handler: usize, kind: IrqKind, rate: u64, prio: u8) -> IrqHandle {
            IrqHandle(self.rv.register(handler, kind, rate, prio))
        }

        /// Drain the op log into the table as of `now_us`, then dispatch what is due.
        fn step(&mut self, now_us: u64) -> u64 {
            self.sync(now_us);
            let masked = self.masked;
            let fired = &self.fired;
            let ran = self.table.dispatch_due(now_us, |h| {
                if masked {
                    return false;
                }
                fired.borrow_mut().push(h);
                true
            });
            // A handler may have registered during dispatch: schedule it now.
            self.sync(now_us);
            ran
        }

        fn sync(&mut self, now_us: u64) {
            let ops = self.rv.ops_since(self.cursor);
            self.cursor += ops.len();
            for op in ops {
                self.table.apply(op, now_us).expect("op applies");
            }
        }

        fn take_fired(&self) -> Vec<usize> {
            std::mem::take(&mut self.fired.borrow_mut())
        }
    }

    #[test]
    fn same_step_dispatch_is_priority_then_registration_index() {
        // Four interrupts due at the same step: priority orders them (lower first),
        // and equal priorities fall back to registration index — never handler value.
        let mut h = Harness::new();
        h.register(0x40, IrqKind::Periodic, 1_000, 5); // idx 0
        h.register(0x10, IrqKind::Periodic, 1_000, 1); // idx 1
        h.register(0x20, IrqKind::Periodic, 1_000, 5); // idx 2 (ties with idx 0)
        h.register(0x30, IrqKind::Periodic, 1_000, 0); // idx 3
        h.sync(0);

        assert_eq!(h.step(1_000), 4);
        assert_eq!(h.take_fired(), vec![0x30, 0x10, 0x40, 0x20]);
    }

    #[test]
    fn registration_order_is_the_only_tiebreak_and_repeats_exactly() {
        // The same registration sequence yields the same dispatch order, step
        // after step, with no dependence on handler addresses.
        let mut h = Harness::new();
        for (handler, prio) in [(0x99, 2u8), (0x11, 2), (0x55, 2)] {
            h.register(handler, IrqKind::Periodic, 1_000, prio);
        }
        h.sync(0);
        for step in 1..=3u64 {
            h.step(step * 1_000);
            assert_eq!(h.take_fired(), vec![0x99, 0x11, 0x55], "step {step}");
        }
    }

    #[test]
    fn periodic_fires_one_full_period_after_registration_then_drift_free() {
        // Registered mid-step at t=1000 with a 3 ms period, the way a driver's
        // registration during sil_fw_start / a task body lands.
        let mut h = Harness::new();
        h.register(0xAA, IrqKind::Periodic, 3_000, 0);
        h.sync(1_000);

        let fired: Vec<u64> = (1..=10u64)
            .filter(|t| h.step(t * 1_000) == 1)
            .map(|t| t * 1_000)
            .collect();
        assert_eq!(fired, vec![4_000, 7_000, 10_000]);
    }

    #[test]
    fn one_shot_quantizes_to_the_next_grid_step_and_fires_once() {
        // A 2500 us delay registered at t=1000 is due at 3500, so it lands on the
        // next grid step (4000) — and never again.
        let mut h = Harness::new();
        h.register(0xBB, IrqKind::OneShot, 2_500, 0);
        h.sync(1_000);

        for t in 2..=6u64 {
            let ran = h.step(t * 1_000);
            assert_eq!(ran, u64::from(t == 4), "step {t}");
        }
        assert_eq!(h.take_fired(), vec![0xBB]);
    }

    #[test]
    fn one_shot_with_a_sub_grid_delay_fires_at_the_very_next_step() {
        // The SPI/DMA-complete case: a 2 us delay on a 1 ms grid is the next step.
        let mut h = Harness::new();
        h.register(0xCC, IrqKind::OneShot, 2, 0);
        h.sync(1_000);
        assert_eq!(h.step(2_000), 1);
        assert_eq!(h.take_fired(), vec![0xCC]);
    }

    #[test]
    fn cancel_removes_and_disable_masks_without_losing_the_schedule() {
        let mut h = Harness::new();
        let keep = h.register(0x01, IrqKind::Periodic, 1_000, 0);
        let kill = h.register(0x02, IrqKind::Periodic, 1_000, 0);
        h.sync(0);

        assert_eq!(h.step(1_000), 2);
        h.take_fired();

        // Cancel is permanent; disable only masks.
        h.rv.cancel(kill.raw());
        h.rv.set_enabled(keep.raw(), false);
        assert_eq!(h.step(2_000), 0);

        h.rv.set_enabled(keep.raw(), true);
        assert_eq!(h.step(3_000), 1);
        assert_eq!(h.take_fired(), vec![0x01]);

        // A cancelled entry never comes back.
        h.rv.set_enabled(kill.raw(), true);
        assert_eq!(h.step(4_000), 1);
        assert_eq!(h.take_fired(), vec![0x01]);
    }

    #[test]
    fn masked_dispatch_holds_the_interrupt_pending() {
        // A masked firmware must not drop a due interrupt: the controller
        // leaves the due time alone and re-attempts each step until it lands.
        let mut h = Harness::new();
        h.register(0x77, IrqKind::OneShot, 500, 0);
        h.sync(0);
        h.masked = true;

        for t in 1..=3u64 {
            assert_eq!(h.step(t * 1_000), 0, "step {t} is masked");
        }
        assert!(h.take_fired().is_empty());

        h.masked = false;
        assert_eq!(h.step(4_000), 1, "unmasking releases the pending interrupt");
        assert_eq!(h.take_fired(), vec![0x77]);
    }

    #[test]
    fn a_period_finer_than_the_grid_coalesces_and_is_counted() {
        // The grid is meant to be at least as fine as the fastest interrupt;
        // when it is not, an entry fires once per step and the swallowed firings are
        // counted so the member can warn.
        let mut h = Harness::new();
        h.register(0x05, IrqKind::Periodic, 250, 0);
        h.sync(0);
        for t in 1..=4u64 {
            assert_eq!(h.step(t * 1_000), 1, "step {t}");
        }
        assert_eq!(h.table.coalesced(), 12); // 3 swallowed per 1 ms step
    }

    #[test]
    fn a_handler_registering_mid_dispatch_schedules_from_the_current_time() {
        // The runtime one-shot path: a handler that starts a transfer registers its
        // completion interrupt while the framework is inside the step, and it must
        // schedule relative to THIS sim time, not the next drain.
        let mut h = Harness::new();
        h.register(0x0A, IrqKind::OneShot, 500, 0);
        h.sync(0);
        assert_eq!(h.step(1_000), 1);
        // Stand in for the handler's own registration during the dispatch.
        h.rv.register(0x0B, IrqKind::OneShot, 1_500, 0);
        h.sync(1_000);

        assert_eq!(h.step(2_000), 0, "due at 2500, so not yet");
        assert_eq!(h.step(3_000), 1);
        assert_eq!(h.take_fired(), vec![0x0A, 0x0B]);
    }

    #[test]
    fn handles_are_never_reused_and_a_stale_one_is_inert() {
        let mut h = Harness::new();
        let a = h.register(0x01, IrqKind::OneShot, 500, 0);
        let b = h.register(0x02, IrqKind::Periodic, 1_000, 0);
        h.sync(0);
        assert_eq!((a.raw(), b.raw()), (0, 1));

        h.step(1_000); // the one-shot fires and dies
        h.take_fired();
        let c = h.register(0x03, IrqKind::Periodic, 1_000, 0);
        assert_eq!(c.raw(), 2, "a dead entry's handle is not recycled");

        // Poking the dead handle is harmless.
        h.rv.set_enabled(a.raw(), true);
        h.sync(1_000);
        assert_eq!(h.step(2_000), 2, "only the two live periodics");
    }

    #[test]
    fn find_by_handler_reaches_a_driver_registered_interrupt() {
        // The framework's grip on an interrupt only C knows the handle of.
        let mut h = Harness::new();
        h.register(0xF0, IrqKind::Periodic, 1_000, 0);
        let target = h.register(0xF1, IrqKind::Periodic, 1_000, 0);
        h.sync(0);

        assert_eq!(h.table.find_by_handler(0xF1), Some(target));
        assert_eq!(h.table.find_by_handler(0xDEAD), None);
    }

    #[test]
    fn malformed_registrations_are_rejected_not_applied() {
        let mut table = IrqTable::new();
        assert!(table
            .apply(
                IrqOp::Register {
                    handle: 0,
                    handler: 0x10,
                    kind: IrqKind::Periodic,
                    rate_or_delay_us: 0,
                    priority: 0,
                },
                0,
            )
            .is_err());
        // A handle out of step with the table index is a wiring bug, not a schedule.
        assert!(table
            .apply(
                IrqOp::Register {
                    handle: 7,
                    handler: 0x10,
                    kind: IrqKind::OneShot,
                    rate_or_delay_us: 5,
                    priority: 0,
                },
                0,
            )
            .is_err());
        assert!(table.apply(IrqOp::Cancel { handle: 3 }, 0).is_err());
        // Nothing landed in the table.
        assert_eq!(table.find_by_handler(0x10), None);
        assert!(!table.any_due(u64::MAX));
    }

    #[test]
    fn rendezvous_hands_out_dense_handles_and_an_ordered_log() {
        let rv = IrqRendezvous::default();
        assert_eq!(rv.register(0x10, IrqKind::Periodic, 100, 0), 0);
        assert_eq!(rv.register(0x20, IrqKind::OneShot, 5, 3), 1);
        rv.set_enabled(0, false);
        rv.cancel(1);

        let all = rv.ops_since(0);
        assert_eq!(all.len(), 4);
        assert_eq!(all[2], IrqOp::SetEnabled { handle: 0, enabled: false });
        assert_eq!(all[3], IrqOp::Cancel { handle: 1 });
        // Cursor-based drain: nothing is replayed.
        assert_eq!(rv.ops_since(4), Vec::new());
    }
}
