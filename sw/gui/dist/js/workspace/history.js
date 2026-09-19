// Per-signal sample history. Ticks are the wire's cycle indices in ms — one
// PWM cycle is 0.05 ms — at period multiples, so an exact-tick lookup either
// hits or the sample is absent, never nearest-neighbor across a
// cycle-index gap. Storage: preallocated Float64Array rings
// (Float64 — u32 counters exceed an f32 mantissa); a null sample (the
// wire's non-finite encoding) stores as NaN and its tick joins the
// `_nullRuns` spans, so read-out decodes it back exactly. A min/max
// pyramid serves range-extreme queries in O(log n), keeping envelope
// decimation (decimate.js, app~views_014) at O(pixel columns).

import { store } from "../state.js";

/** First index with xs[i] >= t (xs ascending). */
export function lowerBound(xs, t) {
  let lo = 0, hi = xs.length;
  while (lo < hi) {
    const mid = (lo + hi) >> 1;
    if (xs[mid] < t) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}

/** First index whose span ends past `t` — strictly past when `strict`, at
 *  or past otherwise. Spans ascend and never overlap, so their ends ascend
 *  with their starts: every consumer bisects to its first relevant span
 *  instead of walking the list. */
function spanIndexPast(spans, t, strict) {
  let lo = 0, hi = spans.length;
  while (lo < hi) {
    const mid = (lo + hi) >> 1;
    const end = spans[mid][1];
    if (strict ? end <= t : end < t) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}

// Marker spans (gaps, null runs) are capped. A lossy link otherwise pushes
// one entry per dropped record: the list grows, every frame costs
// O(entries), frames slow, the host consumes slower, more records drop —
// the spiral this cap exists to break. Touching spans merge on push, and
// past the cap the oldest half coalesces pairwise, so old detail coarsens
// by halves while the newest half keeps full resolution. A coalesced span
// swallows the samples between its two halves: they still read their own
// stored values (a finite sample never decodes as null), they just stop
// being separately marked.
const SPAN_CAP = 4096;

/** Append `span`, merging it into the last when they touch or overlap, and
 *  keep the list under SPAN_CAP. */
function pushSpan(spans, span) {
  const last = spans.length ? spans[spans.length - 1] : null;
  if (last && span[0] <= last[1]) {
    if (span[1] > last[1]) last[1] = span[1];
    return;
  }
  spans.push(span);
  if (spans.length > SPAN_CAP) {
    const half = spans.length >> 1;
    const merged = [];
    for (let i = 0; i < half; i += 2) {
      merged.push(i + 1 < half ? [spans[i][0], spans[i + 1][1]] : spans[i]);
    }
    spans.splice(0, half, ...merged);
  }
}

/** Drop the spans that end below `cut` — one splice, never a shift loop. */
function trimSpansBelow(spans, cut) {
  const k = spanIndexPast(spans, cut, false);
  if (k) spans.splice(0, k);
}

export const CYCLE_MS = 0.05;  // one PWM cycle at 20 kHz

const CAP_MS = 120_000; // live retention horizon, every period alike

// While paused, the frozen span is sacred: nothing at or after its start is
// trimmed. Appends continue this much stream time past the pause (Resume
// lands on continuous data), then stop — bounding a marathon pause's memory;
// the resume-time tick jump renders as an honest gap.
const PAUSE_CATCHUP_MS = 120_000;

// Readers are horizon-bounded by their own window math: let this many stale
// samples pool, then advance `start` once (O(1) on the ring).
const TRIM_SLACK = 4096;

// The wire's u32 cycle index wraps after ~59.65 h of uptime and the tick
// domain restarts near zero. A tick this far behind the newest is that
// restart (or a re-armed stream), not a stale batch tail — the same rule the
// display clock applies in timeline.js. An hour is far past any legitimate
// backwards step and far short of the wrap span.
const RESET_MS = 3_600_000;

// Worst-case retention: live CAP_MS, or paused sacred-span-start → catch-up
// cutoff (max display span 60 s + PAUSE_CATCHUP_MS).
const RETAIN_BOUND_MS = Math.max(CAP_MS, 60_000 + PAUSE_CATCHUP_MS);

// Pyramid levels: bucket sizes 16 / 128 / 1024 / 8192 samples. Buckets are
// aligned to STORAGE index, so trims (start advances) never invalidate
// them — partial edge buckets are raw-scanned by the range query.
const LEVEL_SHIFTS = [4, 7, 10, 13];

// Compaction slides the window by a whole top-level bucket span, keeping
// every level's alignment (see _compact).
const COMPACT_ALIGN = 1 << LEVEL_SHIFTS[LEVEL_SHIFTS.length - 1];

/** Stable small hash of a signal path — compaction stagger only. */
function hashPath(path) {
  let h = 2166136261;
  for (let i = 0; i < path.length; i++) h = Math.imul(h ^ path.charCodeAt(i), 16777619);
  return h >>> 0;
}

export class SignalHistory {
  constructor(period_cycles, path = "") {
    this.periodCycles = period_cycles;
    this.period = period_cycles * CYCLE_MS;
    // Ticks land on 0.05 ms multiples, which binary floats cannot hold
    // exactly, so the gap test carries half a period of slack — a real gap
    // is two periods or more.
    this._gapAfter = this.period * 1.5;
    const live = Math.ceil(RETAIN_BOUND_MS / this.period) + 2;
    // Capacity = live bound + the stale pool + compaction headroom, the
    // headroom setting how often the tail hits capacity and slides back: an
    // eighth of the live bound puts a 20 kHz signal ~22 s apart. A per-path
    // offset of up to a quarter of the headroom staggers those slides so
    // co-watched 20 kHz signals never compact on the same batch.
    // Cost: ≈18 B/sample (two Float64 rings + pyramid) — ~70 MB at 20 kHz.
    const headroom = Math.max(32_768, live >> 3);
    this._cap = live + TRIM_SLACK + headroom - (hashPath(path) % (headroom >> 2));
    this._t = new Float64Array(this._cap);
    this._v = new Float64Array(this._cap);
    this._start = 0;
    this._len = 0;
    this._lv = LEVEL_SHIFTS.map((shift) => {
      const n = Math.ceil(this._cap / (1 << shift));
      return {
        shift,
        mask: (1 << shift) - 1,
        min: new Float64Array(n),
        max: new Float64Array(n),
        amin: new Int32Array(n),
        amax: new Int32Array(n),
      };
    });
    // Detected gaps as [fromTickExclusive, toTickExclusive] spans, merged
    // and capped (pushSpan) and trimmed with the sample horizon.
    this.gaps = [];
    // Null-valued samples (stored as NaN) as inclusive [first, last] tick
    // runs: a persistently null signal costs one entry, not one per sample.
    this._nullRuns = [];
  }

  /** Raw storage view for the envelope query's hot loop (decimate.js):
   *  direct typed-array indexing over storage [start, start+len) beats
   *  per-sample method calls by the constants that matter at 4× CPU
   *  throttle. Read-only by contract. */
  raw() {
    return { t: this._t, v: this._v, start: this._start, len: this._len };
  }

  /** Read-only views for probes/tests; production reads go through the
   *  accessor API. Null-valued samples read as NaN here (see header). */
  get ticks() {
    return this._t.subarray(this._start, this._start + this._len);
  }

  get values() {
    return this._v.subarray(this._start, this._start + this._len);
  }

  get size() {
    return this._len;
  }

  /** Tick at logical index i (0 = oldest retained). */
  tickAtIndex(i) {
    return this._t[this._start + i];
  }

  /** Value at logical index i, null-decoded. */
  valueAtIndex(i) {
    const v = this._v[this._start + i];
    if (Number.isNaN(v) && this._isNullTick(this._t[this._start + i])) return null;
    return v;
  }

  _isNullTick(tick) {
    const k = spanIndexPast(this._nullRuns, tick, false);
    return k < this._nullRuns.length && this._nullRuns[k][0] <= tick;
  }

  /** Record a null sample's tick: consecutive nulls (one period apart)
   *  extend the open run rather than starting a new span. */
  _pushNullTick(tick) {
    const runs = this._nullRuns;
    const last = runs.length ? runs[runs.length - 1] : null;
    if (last && tick - last[1] <= this._gapAfter) last[1] = tick;
    else pushSpan(runs, [tick, tick]);
  }

  /** Drop all samples and derived state; storage stays allocated. */
  clear() {
    this._start = 0;
    this._len = 0;
    this.gaps.length = 0;
    this._nullRuns.length = 0;
    // Pyramid needs no wipe: appends restart at storage 0 and every
    // bucket resets when its first covered index is written.
  }

  append(points) {
    const tl = store.timeline;
    const paused = tl?.mode === "paused" && tl.pausedSpan;
    const appendCutoff = paused ? tl.pausedSpan[1] + PAUSE_CATCHUP_MS : Infinity;
    for (const [tick, value] of points) {
      if (tick > appendCutoff) continue; // past the paused catch-up cap
      let last = this._len ? this._t[this._start + this._len - 1] : null;
      if (last !== null && tick < last - RESET_MS) {
        this.clear(); // tick domain restarted (index wrap): old samples are gone
        last = null;
      }
      if (last !== null && tick <= last) continue; // stale/duplicate batch tail
      if (last !== null && tick - last > this._gapAfter) {
        pushSpan(this.gaps, [last, tick]);
      }
      if (this._start + this._len === this._cap) {
        if (this._len === this._cap) {
          // The ring is entirely live: a single append() call outgrew
          // capacity (only the test/seed surface can — 50 ms wire batches
          // stay orders of magnitude below it, and the horizon trim below
          // bounds len between calls). Drop an oldest chunk now: retention
          // would drop these samples at call end anyway, and chunking
          // amortizes the compact that must follow.
          const n = Math.max(COMPACT_ALIGN, Math.min(this._len >> 3, TRIM_SLACK));
          this._start += n;
          this._len -= n;
          this._dropMarkersBelow(this._t[this._start]);
        }
        this._compact();
      }
      const e = this._start + this._len;
      const isNull = value == null;
      const v = isNull ? NaN : value;
      this._t[e] = tick;
      this._v[e] = v;
      if (isNull) this._pushNullTick(tick);
      this._bucketAdd(e, v);
      this._len++;
    }
    const newest = this._len ? this._t[this._start + this._len - 1] : 0;
    let horizon = newest - CAP_MS;
    if (paused) horizon = Math.min(horizon, tl.pausedSpan[0]);
    const drop = this.indexAtOrAfter(horizon);
    if (drop > TRIM_SLACK) {
      this._start += drop;
      this._len -= drop;
      this._dropMarkersBelow(this._t[this._start]);
    }
    trimSpansBelow(this.gaps, horizon);
  }

  /** Drop the null runs and gaps ending below the oldest retained tick —
   *  bisect and splice, never a shift loop. */
  _dropMarkersBelow(cut) {
    trimSpansBelow(this._nullRuns, cut);
    trimSpansBelow(this.gaps, cut);
  }

  /** Tail reached capacity: slide the live window down by a whole number of
   *  top-level buckets. Every level's bucket size divides that span, so the
   *  pyramid slides with the data — bucket b → b - delta>>shift, extreme
   *  indices - delta — instead of being rebuilt (~5 ms at 3.6 M samples
   *  against ~55 for a rebuild). Rare: once per headroom-many appends. */
  _compact() {
    const delta = this._start & ~(COMPACT_ALIGN - 1);
    const end = this._start + this._len;
    if (!delta) { // window sits inside the first bucket: rebuild instead
      this._t.copyWithin(0, this._start, end);
      this._v.copyWithin(0, this._start, end);
      this._start = 0;
      for (let i = 0; i < this._len; i++) this._bucketAdd(i, this._v[i]);
      return;
    }
    this._t.copyWithin(this._start - delta, this._start, end);
    this._v.copyWithin(this._start - delta, this._start, end);
    for (const L of this._lv) {
      const d = delta >> L.shift;
      const n = Math.ceil(end / (1 << L.shift));
      L.min.copyWithin(0, d, n);
      L.max.copyWithin(0, d, n);
      L.amin.copyWithin(0, d, n);
      L.amax.copyWithin(0, d, n);
      // Buckets left entirely below the new window go negative here and
      // read as empty; they reset when their first index is written again.
      for (let b = n - d; b-- > 0; ) {
        if (L.amin[b] >= 0) { L.amin[b] -= delta; L.amax[b] -= delta; }
      }
    }
    this._start -= delta;
  }

  /** Fold storage index e (value v) into every level's covering bucket.
   *  Writes are append-only and monotonic, so a bucket resets exactly when
   *  its first covered index is written; strict compares keep the FIRST
   *  occurrence of an extreme (the tie rule decimation renders). */
  _bucketAdd(e, v) {
    const finite = Number.isFinite(v);
    for (const L of this._lv) {
      const b = e >> L.shift;
      if ((e & L.mask) === 0) {
        if (finite) {
          L.min[b] = v; L.max[b] = v; L.amin[b] = e; L.amax[b] = e;
        } else {
          L.amin[b] = -1; L.amax[b] = -1;
        }
      } else if (finite) {
        if (L.amin[b] < 0) {
          L.min[b] = v; L.max[b] = v; L.amin[b] = e; L.amax[b] = e;
        } else {
          if (v < L.min[b]) { L.min[b] = v; L.amin[b] = e; }
          if (v > L.max[b]) { L.max[b] = v; L.amax[b] = e; }
        }
      }
    }
  }

  /** First/last-extreme storage fold over logical [i0, i1): returns
   *  logical {amin, amax} of the first minimum and first maximum among
   *  FINITE samples, or amin === -1 when none. Ranges up to 256 scan raw
   *  (a tight typed-array loop beats the level walk's constants there);
   *  larger ranges walk the pyramid in O(log n). Both branches keep the
   *  first occurrence on ties (strict compares, left-to-right fold). */
  rangeMinMax(i0, i1) {
    const vv = this._v;
    let s = this._start + i0;
    const s1 = this._start + i1;
    let minV = Infinity, maxV = -Infinity, amin = -1, amax = -1;
    if (i1 - i0 <= 256) {
      for (; s < s1; s++) {
        const x = vv[s];
        if (!Number.isFinite(x)) continue;
        if (amin < 0) { minV = x; maxV = x; amin = s; amax = s; continue; }
        if (x < minV) { minV = x; amin = s; }
        if (x > maxV) { maxV = x; amax = s; }
      }
    } else {
      while (s < s1) {
        let stepped = false;
        for (let li = this._lv.length - 1; li >= 0; li--) {
          const L = this._lv[li];
          if ((s & L.mask) === 0 && s + L.mask + 1 <= s1) {
            const b = s >> L.shift;
            if (L.amin[b] >= 0) {
              const mn = L.min[b], mx = L.max[b];
              if (amin < 0 || mn < minV) { minV = mn; amin = L.amin[b]; }
              if (amax < 0 || mx > maxV) { maxV = mx; amax = L.amax[b]; }
            }
            s += L.mask + 1;
            stepped = true;
            break;
          }
        }
        if (!stepped) {
          const x = vv[s];
          if (Number.isFinite(x)) {
            if (amin < 0 || x < minV) { minV = x; amin = s; }
            if (amax < 0 || x > maxV) { maxV = x; amax = s; }
          }
          s++;
        }
      }
    }
    return {
      amin: amin < 0 ? -1 : amin - this._start,
      amax: amax < 0 ? -1 : amax - this._start,
    };
  }

  /** First logical index with tick >= t. */
  indexAtOrAfter(t) {
    let lo = this._start, hi = this._start + this._len;
    while (lo < hi) {
      const mid = (lo + hi) >> 1;
      if (this._t[mid] < t) lo = mid + 1;
      else hi = mid;
    }
    return lo - this._start;
  }

  /** First logical index with tick > t. */
  indexAfter(t) {
    let lo = this._start, hi = this._start + this._len;
    while (lo < hi) {
      const mid = (lo + hi) >> 1;
      if (this._t[mid] <= t) lo = mid + 1;
      else hi = mid;
    }
    return lo - this._start;
  }

  /** First logical index >= from with tick >= t — a galloping search that
   *  resumes near `from`, for callers advancing monotonically (the envelope
   *  query's column bounds): O(log distance) instead of O(log n). */
  indexAtOrAfterFrom(t, from) {
    const tt = this._t;
    const end = this._start + this._len;
    let lo = this._start + from;
    if (lo >= end || tt[lo] >= t) return from;
    let step = 1;
    while (lo + step < end && tt[lo + step] < t) {
      lo += step;
      step <<= 1;
    }
    let hi = Math.min(lo + step, end);
    lo += 1; // tt[lo] < t established
    while (lo < hi) {
      const mid = (lo + hi) >> 1;
      if (tt[mid] < t) lo = mid + 1;
      else hi = mid;
    }
    return lo - this._start;
  }

  /** Logical index of the next null-valued sample in [i0, i1), or -1. */
  nextNullIndex(i0, i1) {
    if (!this._nullRuns.length || i0 >= i1) return -1;
    const from = this.tickAtIndex(i0);
    const k = spanIndexPast(this._nullRuns, from, false);
    if (k >= this._nullRuns.length) return -1;
    const idx = this.indexAtOrAfter(Math.max(this._nullRuns[k][0], from));
    return idx < i1 ? idx : -1;
  }

  /** First gap index that can still affect a window starting at `t` (the
   *  first span ending past it). Bisected: a burst-of-drops gap list must
   *  cost a frame nothing to skip. */
  gapIndexFrom(t) {
    return spanIndexPast(this.gaps, t, true);
  }

  newestTick() {
    return this._len ? this._t[this._start + this._len - 1] : null;
  }

  latest() {
    return this._len ? this.valueAtIndex(this._len - 1) : null;
  }

  /** The sample at-or-before `tick`, accepted only within one period — a
   *  tick-count gap reads as absent, never as a stale bridged number. */
  valueNear(tick) {
    const t = this.tickAtOrBefore(tick);
    if (t === null || tick - t >= this.period) return null;
    return this.valueAt(t);
  }

  /** Exact-tick lookup; null when the tick has no sample (gap or off-phase). */
  valueAt(tick) {
    const i = this.indexAtOrAfter(tick);
    if (i >= this._len || this.tickAtIndex(i) !== tick) return null;
    return this.valueAtIndex(i);
  }

  /** Nearest sample tick at or below `tick` (for snapping the cursor to the
   *  signal's own grid); null when history is empty or tick precedes it. */
  tickAtOrBefore(tick) {
    const i = this.indexAfter(tick);
    return i > 0 ? this.tickAtIndex(i - 1) : null;
  }

  /** [ticks, values] within [t0, t1], with an explicit null sample injected
   *  after each real gap so the plot breaks the line there (the renderer
   *  splits a trace into runs at null markers). Off the hot path — the
   *  live refresh goes through decimate.js's ring-native envelope query. */
  windowTable(t0, t1) {
    const xs = [], ys = [];
    let gi = this.gapIndexFrom(t0);
    for (let i = this.indexAtOrAfter(t0); i < this._len; i++) {
      const t = this.tickAtIndex(i);
      if (t > t1) break;
      // A gap breaks the line before the first sample that follows its
      // START. Keying on the span's end would place the marker after the
      // samples a merged span covers, and unsort the table.
      while (gi < this.gaps.length && this.gaps[gi][0] < t) {
        const [from] = this.gaps[gi];
        if (from >= t0) { xs.push(from + this.period); ys.push(null); }
        gi++;
      }
      xs.push(t);
      ys.push(this.valueAtIndex(i));
    }
    return [xs, ys];
  }

  /** Gap spans clipped to [t0, t1] — O(log gaps + spans returned). */
  gapsIn(t0, t1) {
    const out = [];
    for (let i = this.gapIndexFrom(t0); i < this.gaps.length; i++) {
      const [a, b] = this.gaps[i];
      if (a >= t1) break;
      out.push([Math.max(a, t0), Math.min(b, t1)]);
    }
    return out;
  }
}

/** The app-wide history set, keyed by signal path. */
export const histories = new Map();

export function historyFor(path, period_cycles) {
  let h = histories.get(path);
  if (!h || h.periodCycles !== period_cycles) {
    h = new SignalHistory(period_cycles, path);
    histories.set(path, h);
  }
  return h;
}
