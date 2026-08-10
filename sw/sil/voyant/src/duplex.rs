//! The DuplexRouter: an engine-owned, shared coupling of duplex-transaction
//! initiators to peers.
//!
//! Pins are levels; buses are transactions. A [`DuplexRouter`] lets any initiating
//! [`Member`](crate::member::Member) run a synchronous full-duplex exchange against a
//! linked [`DuplexPeer`]: the initiator hands over a `tx` frame and gets the peer's
//! `rx` frame back within the same call — one full-duplex step, no added latency. A
//! firmware member's C SPI upcall and a model's
//! [`MemberCtx::duplex_transfer`](crate::member::MemberCtx::duplex_transfer) drive the
//! **same** router, so the firmware is one initiator among many. The
//! [`Engine`](crate::engine::Engine) force-records each exchange as `<endpoint>:tx` /
//! `:rx` event entries for the historian.

use std::cell::RefCell;
use std::collections::{HashMap, HashSet};
use std::rc::Rc;

use crate::signal::{ParseError, SignalId};

/// The `:tx` / `:rx` event-entry ids of a duplex endpoint, built from the endpoint's
/// own segments — the one place that pair is spelled.
pub(crate) fn tx_rx_ids(
    sig_type: &str,
    source: &str,
    name: &str,
) -> Result<(SignalId, SignalId), ParseError> {
    let tx = SignalId::new(sig_type, source, name, Some("tx"))?;
    let rx = SignalId::new(sig_type, source, name, Some("rx"))?;
    Ok((tx, rx))
}

/// The responder side of a duplex transaction. A transfer calls [`transfer`](Self::transfer)
/// synchronously: the peer consumes `tx`, updates its own internal state, and returns
/// the `rx` frame the initiator reads back before the call returns.
///
/// A peer runs inside the initiator's advance, where the State Table is off-limits;
/// anything table-worthy surfaces through the framework's `:tx`/`:rx` records or the
/// peer's own advance if it is also a [`Member`](crate::member::Member).
pub trait DuplexPeer {
    /// Answer a transfer: consume the `tx` frame, return the `rx` frame.
    fn transfer(&mut self, tx: &[u8]) -> Vec<u8>;
}

/// A dense handle for a declared duplex endpoint. An initiator resolves it once at
/// wiring time (from [`Engine::link_duplex`](crate::engine::Engine::link_duplex)) and
/// passes it to every transfer — no per-transfer string hashing.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct DuplexHandle(usize);

/// The shared endpoint registry coupling initiators to peers. Cloneable (an `Rc`
/// handle over the shared inner): the [`Engine`](crate::engine::Engine) keeps one, and
/// each firmware rendezvous holds a clone so its C upcall forwards here.
#[derive(Clone, Default)]
pub(crate) struct DuplexRouter {
    inner: Rc<RefCell<DuplexInner>>,
}

#[derive(Default)]
struct DuplexInner {
    /// Handle index → endpoint id (a `spi:<owner>:<local>` string).
    ids: Vec<String>,
    /// Endpoint id → handle.
    handles: HashMap<String, DuplexHandle>,
    /// Handle index → linked peer (`None` = declared but unlinked).
    links: Vec<Option<Rc<RefCell<dyn DuplexPeer>>>>,
    /// Endpoint id → peer linked before its endpoint was declared (open registration).
    pending: HashMap<String, Rc<RefCell<dyn DuplexPeer>>>,
    /// Completed transactions since the last drain, in transfer order.
    transactions: Vec<(DuplexHandle, Vec<u8>, Vec<u8>)>,
    /// Pending-link endpoint ids already reported as dangling (warn once).
    warned: HashSet<String>,
}

impl DuplexRouter {
    pub(crate) fn new() -> Self {
        Self::default()
    }

    /// Declare an endpoint (idempotent); returns its handle and attaches any peer
    /// linked before the declaration. The firmware member declares each C-registered
    /// endpoint; a model endpoint is declared implicitly by
    /// [`Engine::link_duplex`](crate::engine::Engine::link_duplex).
    pub(crate) fn declare(&self, id: &str) -> DuplexHandle {
        let mut inner = self.inner.borrow_mut();
        if let Some(&h) = inner.handles.get(id) {
            return h;
        }
        let h = DuplexHandle(inner.ids.len());
        let peer = inner.pending.remove(id);
        inner.ids.push(id.to_string());
        inner.links.push(peer);
        inner.handles.insert(id.to_string(), h);
        h
    }

    /// Attach a peer to an endpoint. Linking an endpoint nobody has declared yet is
    /// legal: the peer waits in `pending` and attaches when the endpoint is declared.
    pub(crate) fn link(&self, id: &str, peer: Rc<RefCell<dyn DuplexPeer>>) {
        let mut inner = self.inner.borrow_mut();
        match inner.handles.get(id).copied() {
            Some(h) => inner.links[h.0] = Some(peer),
            None => {
                inner.pending.insert(id.to_string(), peer);
            }
        }
    }

    /// The handle of a declared endpoint (`None` if not yet declared).
    #[cfg(test)]
    pub(crate) fn handle_of(&self, id: &str) -> Option<DuplexHandle> {
        self.inner.borrow().handles.get(id).copied()
    }

    /// Run a synchronous transfer: clone the linked peer's `Rc` and drop the router
    /// borrow **before** the upcall, so a nested transfer may legally re-enter (a bus
    /// bridge peer forwarding onward), then re-borrow to buffer the exchange. Returns
    /// `None` for an unlinked or unknown endpoint.
    ///
    /// A true cycle — a peer transferring back onto the same peer while its own
    /// `RefCell` is borrowed — panics on that `RefCell`; that is a wiring bug by
    /// construction.
    pub(crate) fn transfer(&self, handle: DuplexHandle, tx: &[u8]) -> Option<Vec<u8>> {
        let peer = {
            let inner = self.inner.borrow();
            match inner.links.get(handle.0) {
                Some(Some(p)) => p.clone(),
                _ => return None,
            }
        };
        let rx = peer.borrow_mut().transfer(tx);
        self.inner
            .borrow_mut()
            .transactions
            .push((handle, tx.to_vec(), rx.clone()));
        Some(rx)
    }

    /// Drain the completed transactions as `(endpoint_id, tx, rx)`, in transfer order.
    pub(crate) fn drain(&self) -> Vec<(String, Vec<u8>, Vec<u8>)> {
        let mut inner = self.inner.borrow_mut();
        let txns = std::mem::take(&mut inner.transactions);
        txns.into_iter()
            .map(|(h, tx, rx)| (inner.ids[h.0].clone(), tx, rx))
            .collect()
    }

    /// Endpoint ids with a peer still waiting on a never-declared endpoint (a dangling
    /// link), reported **once** each — the engine logs a Warning per returned id.
    pub(crate) fn take_dangling(&self) -> Vec<String> {
        let mut inner = self.inner.borrow_mut();
        let candidates: Vec<String> = inner.pending.keys().cloned().collect();
        let fresh: Vec<String> = candidates
            .into_iter()
            .filter(|id| !inner.warned.contains(id))
            .collect();
        for id in &fresh {
            inner.warned.insert(id.clone());
        }
        fresh
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A fixed-response peer that records every tx frame it saw.
    struct RecordingPeer {
        resp: Vec<u8>,
        seen: Vec<Vec<u8>>,
    }

    impl DuplexPeer for RecordingPeer {
        fn transfer(&mut self, tx: &[u8]) -> Vec<u8> {
            self.seen.push(tx.to_vec());
            self.resp.clone()
        }
    }

    /// Keep a clone of the returned handle to read `seen` back after a transfer.
    fn peer(resp: Vec<u8>) -> Rc<RefCell<RecordingPeer>> {
        Rc::new(RefCell::new(RecordingPeer {
            resp,
            seen: Vec::new(),
        }))
    }

    #[test]
    fn declare_is_idempotent_with_dense_handles() {
        let r = DuplexRouter::new();
        let a = r.declare("spi:m:a");
        let b = r.declare("spi:m:b");
        assert_ne!(a, b);
        assert_eq!(r.declare("spi:m:a"), a); // idempotent
        assert_eq!(r.handle_of("spi:m:a"), Some(a));
        assert_eq!(r.handle_of("spi:m:missing"), None);
    }

    #[test]
    fn transfer_roundtrips_and_buffers_the_exchange() {
        let r = DuplexRouter::new();
        let p = peer(vec![0x90, 0x00]);
        let h = r.declare("spi:enc:cs");
        r.link("spi:enc:cs", p.clone());

        let rx = r.transfer(h, &[0xFF, 0xFF]);
        assert_eq!(rx, Some(vec![0x90, 0x00])); // synchronous response
        assert_eq!(p.borrow().seen, vec![vec![0xFF, 0xFF]]); // peer saw the tx

        let drained = r.drain();
        assert_eq!(drained, vec![("spi:enc:cs".to_string(), vec![0xFF, 0xFF], vec![0x90, 0x00])]);
        assert!(r.drain().is_empty()); // drained once
    }

    #[test]
    fn unlinked_or_unknown_endpoint_transfers_to_none() {
        let r = DuplexRouter::new();
        let h = r.declare("spi:enc:cs"); // declared but no peer
        assert_eq!(r.transfer(h, &[0x00]), None);
        assert_eq!(r.transfer(DuplexHandle(99), &[0x00]), None); // unknown handle
        assert!(r.drain().is_empty()); // nothing buffered
    }

    #[test]
    fn pending_link_resolves_when_the_endpoint_is_declared() {
        let r = DuplexRouter::new();
        // Link before declaration (open registration): the peer waits.
        r.link("spi:enc:cs", peer(vec![0x12]));
        assert_eq!(r.handle_of("spi:enc:cs"), None);
        // Declaring the endpoint attaches the pending peer.
        let h = r.declare("spi:enc:cs");
        assert_eq!(r.transfer(h, &[0xAA]), Some(vec![0x12]));
    }

    #[test]
    fn dangling_pending_link_is_reported_once() {
        let r = DuplexRouter::new();
        r.link("spi:ghost:x", peer(vec![]));
        assert_eq!(r.take_dangling(), vec!["spi:ghost:x".to_string()]);
        assert!(r.take_dangling().is_empty()); // warn once
        // Declaring the endpoint attaches the peer, so it stays clear of dangling.
        r.declare("spi:ghost:x");
        assert!(r.take_dangling().is_empty());
    }

    #[test]
    fn nested_transfer_through_a_bridge_peer_is_legal() {
        // A bridge peer forwards its transfer onto a second endpoint on the same
        // router — legal because `transfer` drops its borrow before the upcall.
        let r = DuplexRouter::new();
        let back = r.declare("spi:back:cs");
        r.link("spi:back:cs", peer(vec![0xBE, 0xEF]));

        struct Bridge {
            router: DuplexRouter,
            onward: DuplexHandle,
        }
        impl DuplexPeer for Bridge {
            fn transfer(&mut self, tx: &[u8]) -> Vec<u8> {
                self.router.transfer(self.onward, tx).unwrap_or_default()
            }
        }
        let front = r.declare("spi:front:cs");
        r.link(
            "spi:front:cs",
            Rc::new(RefCell::new(Bridge {
                router: r.clone(),
                onward: back,
            })),
        );

        assert_eq!(r.transfer(front, &[0x01]), Some(vec![0xBE, 0xEF]));
        // Both legs buffered (front then back — front's push runs after the nested one).
        let drained = r.drain();
        assert_eq!(drained.len(), 2);
    }
}
