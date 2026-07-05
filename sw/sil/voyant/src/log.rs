//! The unified log system: a bounded, sim-time-stamped ring of [`LogEntry`]s.
//!
//! Logging is a first-class, framework-wide channel that lives on the
//! [`StateTable`](crate::state_table::StateTable): a member (or the driver) calls
//! [`StateTable::log`](crate::state_table::StateTable::log) and the table stamps
//! the entry with its *current sim time*, so members can neither fake a timestamp
//! nor perturb behaviour — logging is pure observation and never feeds back into
//! the sim (determinism, D7, is untouched).
//!
//! The backing store is a [`LogRing`]: a drop-oldest [`VecDeque`] with a
//! configurable capacity and a running **dropped count**, so truncation under a
//! log storm is visible rather than silent. The driver drains it with
//! [`take_logs`](crate::state_table::StateTable::take_logs) and may peek it with
//! [`logs`](crate::state_table::StateTable::logs).

use std::collections::VecDeque;
use std::fmt;

/// Severity of a [`LogEntry`], ascending.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub enum LogLevel {
    Debug,
    Info,
    Warning,
    Error,
}

impl fmt::Display for LogLevel {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let s = match self {
            LogLevel::Debug => "DEBUG",
            LogLevel::Info => "INFO",
            LogLevel::Warning => "WARNING",
            LogLevel::Error => "ERROR",
        };
        f.write_str(s)
    }
}

/// One log record: sim time, severity, the emitting `source` (a member name or a
/// driver tag), and a free-form message.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LogEntry {
    /// Sim time (microseconds) at which the entry was stamped by the State Table.
    pub time_us: u64,
    pub level: LogLevel,
    /// The emitter's name (a [`Member`](crate::member::Member) name, or a driver tag).
    pub source: String,
    pub message: String,
}

impl fmt::Display for LogEntry {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "[{}] t={}us {}: {}",
            self.level, self.time_us, self.source, self.message
        )
    }
}

/// A bounded, drop-oldest ring of [`LogEntry`]s with a dropped-count so truncation
/// is observable. Owned by the [`StateTable`](crate::state_table::StateTable).
#[derive(Debug, Clone)]
pub struct LogRing {
    entries: VecDeque<LogEntry>,
    cap: usize,
    dropped: u64,
}

impl LogRing {
    /// A ring holding at most `cap` entries (clamped to at least 1).
    pub fn new(cap: usize) -> Self {
        Self {
            entries: VecDeque::new(),
            cap: cap.max(1),
            dropped: 0,
        }
    }

    /// Append an entry, evicting the oldest (and bumping the dropped count) when
    /// the ring is full.
    pub fn push(&mut self, entry: LogEntry) {
        if self.entries.len() >= self.cap {
            self.entries.pop_front();
            self.dropped += 1;
        }
        self.entries.push_back(entry);
    }

    /// Drain every buffered entry (oldest first), leaving the ring empty. The
    /// dropped count is preserved across drains.
    pub fn take(&mut self) -> Vec<LogEntry> {
        self.entries.drain(..).collect()
    }

    /// Peek the buffered entries without draining.
    pub fn peek(&self) -> &VecDeque<LogEntry> {
        &self.entries
    }

    /// How many entries were dropped (evicted before a drain) over this ring's life.
    pub fn dropped(&self) -> u64 {
        self.dropped
    }

    pub fn len(&self) -> usize {
        self.entries.len()
    }
    pub fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn entry(t: u64, msg: &str) -> LogEntry {
        LogEntry {
            time_us: t,
            level: LogLevel::Info,
            source: "test".into(),
            message: msg.into(),
        }
    }

    #[test]
    fn ring_drops_oldest_and_counts() {
        let mut ring = LogRing::new(2);
        ring.push(entry(1, "a"));
        ring.push(entry(2, "b"));
        assert_eq!(ring.len(), 2);
        assert_eq!(ring.dropped(), 0);

        // Third push evicts the oldest ("a") and bumps the dropped count.
        ring.push(entry(3, "c"));
        assert_eq!(ring.len(), 2);
        assert_eq!(ring.dropped(), 1);
        let drained = ring.take();
        assert_eq!(
            drained.iter().map(|e| e.message.as_str()).collect::<Vec<_>>(),
            vec!["b", "c"]
        );
        // Drain empties the buffer but keeps the dropped count.
        assert!(ring.is_empty());
        assert_eq!(ring.dropped(), 1);
    }

    #[test]
    fn cap_is_clamped_to_at_least_one() {
        let mut ring = LogRing::new(0);
        ring.push(entry(1, "a"));
        ring.push(entry(2, "b"));
        assert_eq!(ring.len(), 1);
        assert_eq!(ring.dropped(), 1);
        assert_eq!(ring.peek().back().unwrap().message, "b");
    }

    #[test]
    fn level_orders_and_displays() {
        assert!(LogLevel::Error > LogLevel::Warning);
        assert!(LogLevel::Warning > LogLevel::Info);
        assert_eq!(LogLevel::Warning.to_string(), "WARNING");
    }
}
