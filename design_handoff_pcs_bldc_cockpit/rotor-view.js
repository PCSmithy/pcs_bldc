/* rotor-view — a three.js pane for the pcs_bldc cockpit.
   Stand-in geometry: proportions traced from the board photos, not the real CAD.
   Swap buildBoard()/buildRotor() for a loadGLTF() of the exported assembly and
   everything else here (theming, telemetry drive, cursor scrub, LED drive) still applies.

   Named nodes the real model must provide:
     board · rotorA (driven) · rotorB (encoder test target, static)
     ledRing · led_D{n} (36 RGB, 10 deg pitch) · led_PD_FLT / _5V0 / _3V3 / _VBUS / _HEART */

const THREE_URL = "https://esm.sh/three@0.169.0";
let threePromise = null;
const loadThree = () => (threePromise ||= import(THREE_URL));

const POLE_PAIRS = 14;
const TAU = Math.PI * 2;
const LED_N = 36;                 // 36 RGB LEDs, a complete ring at 10° spacing
const LED_STEP = TAU / LED_N;
/* normalise any angle to [0, TAU) — the ring closes, so every comparison wraps */
const norm = a => ((a % TAU) + TAU) % TAU;

const VIEWS = {
  iso:   { az: -0.62, el: 0.72, dist: 138 },
  top:   { az: 0,     el: 1.48, dist: 118 },
  front: { az: 0,     el: 0.12, dist: 132 }
};

/* ── theme bridge ───────────────────────────────────────────────────────── */
function readTheme() {
  const cs = getComputedStyle(document.documentElement);
  const v = (n, f) => (cs.getPropertyValue(n).trim() || f);
  return {
    accent: v("--color-accent-400", "#f6a06b"),
    ok: v("--color-accent-2-400", "#aebf92"),
    key: document.documentElement.getAttribute("data-theme") || "warm"
  };
}

/* ── geometry helpers ───────────────────────────────────────────────────── */
function roundedRect(THREE, w, h, r) {
  const x = w / 2, y = h / 2, s = new THREE.Shape();
  s.moveTo(-x + r[0], y);
  s.lineTo(x - r[1], y);
  s.quadraticCurveTo(x, y, x, y - r[1]);
  s.lineTo(x, -y + r[2]);
  s.quadraticCurveTo(x, -y, x - r[2], -y);
  s.lineTo(-x + r[3], -y);
  s.quadraticCurveTo(-x, -y, -x, -y + r[3]);
  s.lineTo(-x, y - r[0]);
  s.quadraticCurveTo(-x, y, -x + r[0], y);
  return s;
}

function ring(THREE, outer, bore, holes) {
  const s = new THREE.Shape();
  s.absarc(0, 0, outer, 0, TAU, false);
  const cut = (cx, cy, r) => { const p = new THREE.Path(); p.absarc(cx, cy, r, 0, TAU, true); s.holes.push(p); };
  if (bore) cut(0, 0, bore);
  (holes || []).forEach(([n, hr, r]) => {
    for (let i = 0; i < n; i++) { const a = (i / n) * TAU + Math.PI / n; cut(Math.cos(a) * hr, Math.sin(a) * hr, r); }
  });
  return s;
}

const extrude = (THREE, shape, depth, mat, seg = 64) =>
  new THREE.Mesh(new THREE.ExtrudeGeometry(shape, { depth, bevelEnabled: false, curveSegments: seg }), mat);

/* wrap a delta into (-PI, PI] — the ring is angular, so every distance must be */
const wrap = a => { a %= TAU; if (a > Math.PI) a -= TAU; if (a < -Math.PI) a += TAU; return a; };

/* ── LED drive: what the arc is actually doing ───────────────────────────
   Each pattern returns a 0..1 target per LED. Nothing here snaps: the element
   runs every target through a rise/fall filter so the arc smears at speed the
   way a real LED does, rather than strobing at frame rate.                */
const PATTERNS = {
  /* the real one: a single LED tracks mechanical angle, widened only by the
     10° pitch of the ring and by the persistence filter below */
  position(a, mech) { return Math.exp(-Math.pow(wrap(a - mech) / 0.10, 2)); },

  /* same head, long exponential tail behind the direction of travel */
  comet(a, mech, ctx) {
    const d = wrap(a - mech) * (ctx.dir >= 0 ? -1 : 1);
    const head = Math.exp(-Math.pow(wrap(a - mech) / 0.09, 2));
    const tail = d > 0 ? Math.exp(-d / 0.7) * 0.8 : 0;
    return Math.min(1, head + tail);
  },

  /* six-step block commutation: the 60°-electrical sector the drive is energising */
  commutation(a, mech, ctx) {
    return Math.floor(norm(a) / TAU * 6) === Math.floor(ctx.elec / TAU * 6) ? 1 : 0.04;
  },

  /* electrical angle — fourteen cycles per revolution at fourteen pole pairs */
  electrical(a, mech, ctx) { return Math.exp(-Math.pow(wrap(a - ctx.elec) / 0.13, 2)); },

  /* bargraph: the whole ring from zero up to the current angle */
  sweep(a, mech) { return norm(a) <= mech ? 1 : 0.03; }
};

/* ── the scene ──────────────────────────────────────────────────────────── */
function buildScene(THREE, theme) {
  const scene = new THREE.Scene();
  const root = new THREE.Group();
  scene.add(root);

  const M = {
    pcb: new THREE.MeshStandardMaterial({ color: 0x1c4430, roughness: 0.62, metalness: 0.04 }),
    alu: new THREE.MeshStandardMaterial({ color: 0xc4c6c7, roughness: 0.38, metalness: 0.55 }),
    aluDark: new THREE.MeshStandardMaterial({ color: 0x4a4c4d, roughness: 0.5, metalness: 0.5 }),
    ic: new THREE.MeshStandardMaterial({ color: 0x24262a, roughness: 0.5, metalness: 0.2 }),
    passive: new THREE.MeshStandardMaterial({ color: 0x9aa0a4, roughness: 0.6, metalness: 0.3 }),
    header: new THREE.MeshStandardMaterial({ color: 0xd6b52c, roughness: 0.45, metalness: 0.45 }),
    conn: new THREE.MeshStandardMaterial({ color: 0xe8eae9, roughness: 0.55 }),
    index: new THREE.MeshBasicMaterial({ color: new THREE.Color(theme.accent) })
  };

  const BW = 104, BH = 64, BT = 1.6;
  const board = extrude(THREE, roundedRect(THREE, BW, BH, [5, 11, 30, 5]), BT, M.pcb, 24);
  board.name = "board";
  root.add(board);

  const RA = { x: 23, y: 3, r: 19.5 };
  const RB = { x: -31, y: -17, r: 9.6 };

  /* silkscreen */
  const silkLine = new THREE.LineBasicMaterial({ color: 0xdfe6e0, transparent: true, opacity: 0.85 });
  const line = pts => new THREE.Line(
    new THREE.BufferGeometry().setFromPoints(pts.map(p => new THREE.Vector3(p[0], p[1], BT + 0.03))), silkLine);
  const arc = (cx, cy, r, a0, a1, n = 96) => {
    const p = [];
    for (let i = 0; i <= n; i++) p.push([cx + Math.cos(a0 + (a1 - a0) * (i / n)) * r, cy + Math.sin(a0 + (a1 - a0) * (i / n)) * r]);
    return p;
  };
  root.add(line(arc(RA.x, RA.y, RA.r + 5.4, 0, TAU, 144)));
  root.add(line(arc(RB.x, RB.y, RB.r + 4.6, -0.5, 3.3)));
  for (let i = 0; i < 36; i++) {
    const a = i * LED_STEP, long = i % 9 === 0;   // long ticks at 0 / 90 / 180 / 270
    const r0 = RA.r + 5.4, r1 = r0 + (long ? 3.4 : 1.7);
    root.add(line([[RA.x + Math.cos(a) * r0, RA.y + Math.sin(a) * r0], [RA.x + Math.cos(a) * r1, RA.y + Math.sin(a) * r1]]));
  }
  for (let i = 0; i < 9; i++) {
    const a = -0.5 + (3.8 * i) / 8;
    root.add(line([[RB.x + Math.cos(a) * (RB.r + 0.6), RB.y + Math.sin(a) * (RB.r + 0.6)],
                   [RB.x + Math.cos(a) * (RB.r + 4.6), RB.y + Math.sin(a) * (RB.r + 4.6)]]));
  }

  /* ── LEDs ──────────────────────────────────────────────────────────────
     Each LED is a body (the package, lit from within) plus a flat additive
     disc on the PCB — the spill. Without the spill an emissive box just reads
     as white paint; with it, the arc reads as light.                       */
  const ledGeo = new THREE.BoxGeometry(2.0, 1.3, 0.75);
  const glowGeo = new THREE.CircleGeometry(3.2, 20);
  const smallGeo = new THREE.BoxGeometry(1.7, 1.1, 0.6);
  const smallGlowGeo = new THREE.CircleGeometry(2.3, 16);

  function makeLed(x, y, rot, colorHex, big) {
    const mat = new THREE.MeshStandardMaterial({
      color: 0xf2f4f2, roughness: 0.35,
      emissive: new THREE.Color(colorHex), emissiveIntensity: 0
    });
    const body = new THREE.Mesh(big ? ledGeo : smallGeo, mat);
    body.position.set(x, y, BT + (big ? 0.38 : 0.3));
    body.rotation.z = rot;
    const glowMat = new THREE.MeshBasicMaterial({
      color: new THREE.Color(colorHex), transparent: true, opacity: 0,
      blending: THREE.AdditiveBlending, depthWrite: false
    });
    const glow = new THREE.Mesh(big ? glowGeo : smallGlowGeo, glowMat);
    glow.position.set(x, y, BT + 0.05);
    return { body, glow, mat, glowMat, v: 0 };
  }

  const ledRing = new THREE.Group();
  ledRing.name = "ledRing";
  const arcLeds = [];
  for (let i = 0; i < LED_N; i++) {
    const a = i * LED_STEP;
    const r = RA.r + 2.6;
    const led = makeLed(RA.x + Math.cos(a) * r, RA.y + Math.sin(a) * r, a, theme.accent, true);
    led.a = a;
    led.body.name = "led_D" + (30 + i);   // RGB package; emissive colour comes from the theme
    arcLeds.push(led);
    ledRing.add(led.body, led.glow);
  }
  root.add(ledRing);

  /* status LEDs, left edge — these mirror rails and faults, not motion */
  const statusDefs = [
    ["PD_FLT", -45.5, 2.5, theme.accent],
    ["5V0", -45.5, -2, "#7ee08a"],
    ["3V3", -45.5, -6.5, "#7ee08a"],
    ["VBUS", -45.5, -11, "#7ee08a"],
    ["HEART", -47.5, 21.5, "#8ef0a0"]
  ];
  const status = {};
  statusDefs.forEach(([k, x, y, c]) => {
    const led = makeLed(x, y, 0, c, false);
    led.body.name = "led_" + k;
    status[k] = led;
    root.add(led.body, led.glow);
  });

  /* rotors */
  function buildRotor(cfg, big) {
    const g = new THREE.Group();
    g.add(extrude(THREE, ring(THREE, cfg.r, big ? 6.4 : 2.3, [[big ? 8 : 4, big ? 12.4 : 5.6, big ? 1.0 : 0.8]]), big ? 7.4 : 5.2, M.alu));
    g.add(extrude(THREE, ring(THREE, big ? 8.2 : 3.6, big ? 6.4 : 2.3), big ? 9.2 : 6.4, M.aluDark, 48));
    const skirt = extrude(THREE, ring(THREE, cfg.r * 0.93, 0), 1.1, M.aluDark, 48);
    skirt.position.z = -1.1;
    g.add(skirt);
    const mark = new THREE.Mesh(new THREE.BoxGeometry(cfg.r * 0.34, 0.9, 0.1), M.index);
    mark.position.set(cfg.r * 0.72, 0, (big ? 7.4 : 5.2) + 0.06);
    g.add(mark);
    g.position.set(cfg.x, cfg.y, BT);
    return g;
  }
  const rotorA = buildRotor(RA, true);  rotorA.name = "rotorA";  root.add(rotorA);
  const rotorB = buildRotor(RB, false); rotorB.name = "rotorB";  root.add(rotorB);

  /* populated side: enough parts to read as this board, no more */
  const box = (x, y, w, h, d, mat) => {
    const m = new THREE.Mesh(new THREE.BoxGeometry(w, h, d), mat);
    m.position.set(x, y, BT + d / 2);
    root.add(m);
  };
  box(-6, 5, 8, 8, 1.1, M.ic);
  box(-19, 8, 7, 6.4, 3.2, M.ic);
  box(-28, 8, 7, 6.4, 3.2, M.ic);
  box(-9, -9, 9, 4.6, 3.0, M.conn);
  box(8, -26, 24, 4.2, 2.6, M.header);
  box(-13, -25, 6, 8, 3.4, M.conn);
  box(-5, -25, 6, 8, 3.4, M.conn);
  box(-38, 26, 11, 2.6, 2.4, M.passive);
  box(46, -22, 4.2, 4.2, 1.9, M.passive);
  box(-44, -6, 4.2, 4.2, 1.9, M.passive);
  [[-40, 17], [-33, 17], [-24, 16], [-16, 15], [-2, 14], [4, 13], [10, 12],
   [-40, 1], [-34, -2], [-26, -3], [-18, -6], [-2, -3], [3, -4], [10, -6],
   [-26, -19], [-18, -20], [-10, -18], [2, -14], [8, -16], [14, -18]
  ].forEach(([x, y], i) => box(x, y, 2.6, 1.5, 0.7, i % 3 ? M.passive : M.ic));

  root.rotation.x = -Math.PI / 2;
  return { scene, root, rotorA, rotorB, arcLeds, status, materials: M };
}

/* ── element ────────────────────────────────────────────────────────────── */
class RotorView extends HTMLElement {
  /* both spellings: React-mounted custom elements receive attributes lowercased
     and hyphen-free, while hand-written HTML uses the kebab form */
  static get observedAttributes() {
    return ["mode", "view", "rpm", "stale", "pattern", "fault", "cursor-ms", "cursorms", "theme-key", "themekey"];
  }
  attr(kebab, flat) { return this.getAttribute(kebab) ?? this.getAttribute(flat); }

  constructor() {
    super();
    this.attachShadow({ mode: "open" });
    this.shadowRoot.innerHTML =
      '<style>:host{display:block;position:relative;width:100%;height:100%;overflow:hidden}' +
      'canvas{display:block;width:100%;height:100%;touch-action:none;cursor:grab}' +
      '.msg{position:absolute;inset:0;display:grid;place-items:center;padding:16px;text-align:center;' +
      'font:12px ui-monospace,Menlo,Consolas,monospace;color:#82796a;line-height:1.5}</style>' +
      '<div class="msg">initialising renderer…</div>';
    this._angA = 0; this._last = 0; this._heart = 0;
  }

  connectedCallback() {
    if (this._booted) return;
    this._booted = true;
    this._orbit = { ...VIEWS.iso };
    this.start();
  }

  disconnectedCallback() {
    cancelAnimationFrame(this._raf);
    this._ro?.disconnect();
    this._mo?.disconnect();
    if (this._renderer) { this._renderer.dispose(); this._renderer.forceContextLoss?.(); }
  }

  attributeChangedCallback(n, _o, v) {
    if (n === "view" && this._orbit && VIEWS[v]) this._target = { ...VIEWS[v] };
    if (n === "theme-key" || n === "themekey") this.retheme();
    this._dirty = true;
  }

  get rpm() { return parseFloat(this.getAttribute("rpm") || "0") || 0; }
  get cursorMs() { return parseFloat(this.attr("cursor-ms", "cursorms") || "0") || 0; }
  get isLive() { return (this.getAttribute("mode") || "live") === "live"; }
  get isStale() { return this.hasAttribute("stale") && this.getAttribute("stale") !== "false"; }
  get hasFault() { return this.hasAttribute("fault") && this.getAttribute("fault") !== "false"; }
  get pattern() { return PATTERNS[this.getAttribute("pattern")] ? this.getAttribute("pattern") : "position"; }

  async start() {
    let THREE;
    try { THREE = await loadThree(); }
    catch {
      this.shadowRoot.querySelector(".msg").textContent =
        "3D pane unavailable — renderer could not load. Plots and telemetry are unaffected.";
      return;
    }
    if (!this.isConnected) return;
    this.THREE = THREE;
    Object.assign(this, buildScene(THREE, readTheme()));

    const r = new THREE.WebGLRenderer({ antialias: true, alpha: true, powerPreference: "low-power" });
    r.setPixelRatio(Math.min(devicePixelRatio || 1, 2));
    r.setClearAlpha(0);
    this._renderer = r;
    this.shadowRoot.querySelector(".msg").remove();
    this.shadowRoot.appendChild(r.domElement);

    this.camera = new THREE.PerspectiveCamera(30, 1, 1, 900);
    this.scene.add(new THREE.HemisphereLight(0xc8dae4, 0x14161a, 0.62));
    const d1 = new THREE.DirectionalLight(0xffffff, 1.05); d1.position.set(70, 110, 90); this.scene.add(d1);
    const d2 = new THREE.DirectionalLight(0x8fb4d8, 0.42); d2.position.set(-90, 40, -70); this.scene.add(d2);
    this.scene.add(new THREE.AmbientLight(0xffffff, 0.22));

    this._ro = new ResizeObserver(() => { this._dirty = true; this.resize(); });
    this._ro.observe(this);
    this._mo = new MutationObserver(() => this.retheme());
    this._mo.observe(document.documentElement, { attributes: true, attributeFilter: ["data-theme"] });
    this.retheme();
    this.bindOrbit(r.domElement);
    this.resize();
    this._last = performance.now();
    this.loop();
  }

  retheme() {
    if (!this.THREE) return;
    const t = readTheme();
    this.materials.index.color.set(t.accent);
    this.arcLeds.forEach(l => { l.mat.emissive.set(t.accent); l.glowMat.color.set(t.accent); });
    this.status.PD_FLT.mat.emissive.set(t.accent);
    this.status.PD_FLT.glowMat.color.set(t.accent);
    this._neon = t.key === "neon";
    this._dirty = true;
  }

  resize() {
    const w = this.clientWidth || 1, h = this.clientHeight || 1;
    this._renderer.setSize(w, h, false);
    this.camera.aspect = w / h;
    this.camera.updateProjectionMatrix();
  }

  bindOrbit(el) {
    let drag = null;
    el.addEventListener("pointerdown", e => {
      drag = { x: e.clientX, y: e.clientY, az: this._orbit.az, el: this._orbit.el };
      el.setPointerCapture(e.pointerId); el.style.cursor = "grabbing";
    });
    el.addEventListener("pointermove", e => {
      if (!drag) return;
      this._target = null;
      this._orbit.az = drag.az - (e.clientX - drag.x) * 0.006;
      this._orbit.el = Math.max(0.06, Math.min(1.5, drag.el + (e.clientY - drag.y) * 0.005));
      this._dirty = true;
    });
    const stop = () => { drag = null; el.style.cursor = "grab"; };
    el.addEventListener("pointerup", stop);
    el.addEventListener("pointercancel", stop);
    el.addEventListener("wheel", e => {
      e.preventDefault();
      this._target = null;
      this._orbit.dist = Math.max(70, Math.min(260, this._orbit.dist * (1 + Math.sign(e.deltaY) * 0.08)));
      this._dirty = true;
    }, { passive: false });
  }

  /* rise/fall filter — an LED does not switch, it charges and decays.
     This is what makes the arc smear into a comet at speed instead of strobing. */
  static step(v, target, dt) {
    const tau = target > v ? 0.028 : 0.085;
    return v + (target - v) * (1 - Math.exp(-dt / tau));
  }

  loop = () => {
    this._raf = requestAnimationFrame(this.loop);
    const now = performance.now(), dt = Math.min(0.1, (now - this._last) / 1000);
    this._last = now;
    if (document.hidden) return;

    const live = this.isLive && !this.isStale;
    const w = (this.rpm / 60) * TAU;
    if (live) {
      this._angA = (this._angA + w * dt) % TAU;
      this._heart = (this._heart + dt) % 1;
    } else {
      this._angA = (w * this.cursorMs / 1000) % TAU;
    }

    if (this._target) {
      const k = Math.min(1, dt * 6);
      ["az", "el", "dist"].forEach(p => { this._orbit[p] += (this._target[p] - this._orbit[p]) * k; });
      if (Math.abs(this._target.az - this._orbit.az) < 1e-3) this._target = null;
    }

    this.rotorA.rotation.z = this._angA;
    // rotorB is the encoder test target — nothing in telemetry drives it

    /* arc LEDs */
    const mech = ((this._angA % TAU) + TAU) % TAU;
    const ctx = { elec: (mech * POLE_PAIRS) % TAU, dir: this.rpm >= 0 ? 1 : -1 };
    const fn = PATTERNS[this.pattern];
    const gain = this._neon ? 2.6 : 1.6;
    const stale = this.isStale;
    for (const l of this.arcLeds) {
      const target = stale ? 0.05 : fn(l.a, mech, ctx);
      l.v = RotorView.step(l.v, target, dt);
      l.mat.emissiveIntensity = l.v * gain;
      l.glowMat.opacity = Math.min(0.62, l.v * (this._neon ? 0.6 : 0.42));
    }

    /* status LEDs: rails solid, fault from the drive, heartbeat at the link rate */
    const s = this.status;
    const set = (led, target) => {
      led.v = RotorView.step(led.v, target, dt);
      led.mat.emissiveIntensity = led.v * gain;
      led.glowMat.opacity = Math.min(0.55, led.v * 0.4);
    };
    set(s["5V0"], 1);
    set(s["3V3"], 1);
    set(s.VBUS, 1);
    set(s.PD_FLT, this.hasFault ? (this._heart < 0.5 ? 1 : 0) : 0);
    set(s.HEART, stale ? 0 : (this._heart % 0.1 < 0.03 ? 1 : 0));

    const { az, el, dist } = this._orbit;
    this.camera.position.set(Math.sin(az) * Math.cos(el) * dist, Math.sin(el) * dist, Math.cos(az) * Math.cos(el) * dist);
    this.camera.lookAt(0, 0, 0);
    this._renderer.render(this.scene, this.camera);

    if (now - (this._emit || 0) > 100) {
      this._emit = now;
      this.dispatchEvent(new CustomEvent("pose", {
        bubbles: true, composed: true,
        detail: { mech, elec: ctx.elec, rpm: live ? this.rpm : 0, live }
      }));
    }
  };
}

if (!customElements.get("rotor-view")) customElements.define("rotor-view", RotorView);
