// Setpoint kinds and the board's modes, shared by the Command and Sequence
// widgets. Each kind is one row: its units and rating, the modes that take
// it, its wire fields, its limit bounds and extra limit fields, and the
// target in effect the Sequence chart draws (fw~mc_027, fw~mc_031).
// [impl->app~views_024~1] [impl->app~views_025~1]

/** The board's modes in selector order. */
export const COMMAND_MODES = [
  { mode: "MODE_OFF", label: "off" },
  { mode: "MODE_SIX_STEP_TRAP", label: "six-step" },
  { mode: "MODE_SINUSOIDAL", label: "sinusoidal" },
  { mode: "MODE_FOC_TORQUE", label: "FOC torque" },
  { mode: "MODE_FOC_VELOCITY", label: "FOC velocity" },
  { mode: "MODE_FOC_POSITION", label: "FOC position" },
];

/** |value| held within the bound; no bound known leaves it as written. */
export function clampToBound(value, bound) {
  return Number.isFinite(bound) ? Math.max(-bound, Math.min(bound, value)) : value;
}

/** The stop: MODE_OFF with no setpoints, accepted in every state. */
export const stopCommand = () => ({ mode: "MODE_OFF", setpoints: [] });
export const isStop = (command) => command.mode === "MODE_OFF" && command.setpoints.length === 0;

const atMost = (bound, unit) => (Number.isFinite(bound) ? `≤ ${+bound.toFixed(4)} ${unit} · ` : "");
const atPointBound = (limit, info) => (limit > 0 ? limit : info?.max_velocity_radps);

/** A position's limits held in place: the velocity limit at the maximum
 *  velocity, the velocity at the point at the velocity limit. */
export function boundPositionLimits(p, info) {
  p.limit = clampToBound(p.limit, info?.max_velocity_radps);
  p.at_point = clampToBound(p.at_point ?? 0, atPointBound(p.limit, info));
}

/** Limit fields a kind carries beside its rate limit (`min`: held at or above). */
const ACCEL = {
  key: "accel", attr: "accel", label: "acceleration limit", short: "accel", unit: "rad/s²", zero: "step",
  aria: "Acceleration limit", min: 0, hint: () => "empty or 0 steps",
};
const AT_POINT = {
  key: "at_point", attr: "atpoint", label: "velocity at the point", short: "at point", unit: "rad/s", zero: "stop",
  aria: "Velocity at the point", hint: (p, info) => `${atMost(atPointBound(p.limit, info), "rad/s")}empty or 0 stops`,
};

/** The target in effect over [0, duration] at a rate limit: each pass slews
 *  toward the last written target, a zero limit stepping; from zero before
 *  the first point. Returns [times_ms, values]. */
export function rateTrajectory(points, duration_ms, _info, step_ms = Math.max(1, duration_ms / 2000)) {
  const ts = [], vs = [];
  let effective = 0;
  let i = 0;
  let target = null;
  for (let t = 0; t <= duration_ms + 1e-9; t += step_ms) {
    // A pass slews toward the target written before it; a step lands at once.
    if (target && target.limit > 0) {
      const maxStep = (target.limit * step_ms) / 1000;
      effective += Math.max(-maxStep, Math.min(maxStep, target.value - effective));
    }
    while (i < points.length && points[i].t_ms <= t) {
      target = points[i++];
      if (target.limit <= 0) effective = target.value;
    }
    ts.push(t);
    vs.push(effective);
  }
  return [ts, vs];
}

/** The position target in the board's 1 ms passes (fw~mc_031): each move
 *  slews at its acceleration limit toward its velocity limit, braking to
 *  reach the point at its velocity there. Returns [times_ms, values, velocities]. */
export function positionTrajectory(points, duration_ms, info, step_ms = Math.max(1, duration_ms / 2000)) {
  const T = 1e-3;
  const maxVelocity = info?.max_velocity_radps ?? SETPOINT_KINDS.velocity.fallback;
  const maxPosition = info?.max_position_rad ?? SETPOINT_KINDS.position.fallback;
  let theta = 0, omega = 0, sp = null, arrived = false, held = 0, i = 0, pass = 0;
  const runPass = () => {
    while (i < points.length && points[i].t_ms <= pass) {
      const p = points[i++];
      const v = p.limit > 0 ? Math.min(p.limit, maxVelocity) : maxVelocity;
      sp = {
        position: clampToBound(p.value, maxPosition), v, a: Math.max(0, p.accel ?? 0),
        ws: Math.abs(clampToBound(p.at_point ?? 0, v)),
      };
      arrived = false;
    }
    let w = 0;
    if (sp && arrived) {
      w = held;
    } else if (sp) {
      const distance = sp.position - theta;
      const dir = distance < 0 ? -1 : 1;
      const remaining = Math.abs(distance);
      if (sp.a > 0) {
        // The speed from which braking at a reaches ω_s exactly on the point.
        const aT = sp.a * T, h = aT / 2;
        const speed = Math.min(sp.v, Math.sqrt(Math.max(0, h * h + sp.ws * sp.ws - aT * sp.ws + 2 * sp.a * remaining)) - h);
        w = omega + Math.max(-aT, Math.min(aT, dir * speed - omega));
      } else {
        w = dir * sp.v;
      }
      if (w * dir >= 0 && remaining <= Math.abs(w) * T) {
        arrived = true;
        held = w = dir * sp.ws;
        if (sp.ws === 0) theta = sp.position;
      }
    }
    let next = theta + w * T;
    if (Math.abs(next) > maxPosition) {
      next = Math.sign(next) * maxPosition;
      w = 0;
    }
    theta = next;
    omega = w;
    pass += 1;
  };
  const ts = [], vs = [], ws = [];
  for (let t = 0; t <= duration_ms + 1e-9; t += step_ms) {
    while (pass < t - 1e-9) runPass();
    ts.push(t);
    vs.push(theta);
    ws.push(omega);
  }
  return [ts, vs, ws];
}

const rateLimitHint = () => "per second · empty or 0 steps";

/** One row per setpoint kind. A position's rate limit is its velocity limit,
 *  zero the maximum; `fallback` bounds the chart with no rating reported. */
export const SETPOINT_KINDS = {
  velocity: {
    unit: "rad/s", limitUnit: "rad/s²", limitLabel: "rate limit", limitZero: "step", fallback: 50,
    modes: ["MODE_SIX_STEP_TRAP", "MODE_SINUSOIDAL", "MODE_FOC_VELOCITY"],
    rating: (info) => info?.max_velocity_radps,
    extras: [],
    wireFields: (p) => ({ velocity_radps: p.value, rate_limit: p.limit }),
    limitHint: rateLimitHint,
    trajectory: rateTrajectory,
  },
  torque: {
    unit: "Nm", limitUnit: "Nm/s", limitLabel: "rate limit", limitZero: "step", fallback: 1,
    modes: ["MODE_FOC_TORQUE"],
    rating: (info) => info?.max_torque_nm,
    extras: [],
    wireFields: (p) => ({ torque_nm: p.value, rate_limit: p.limit }),
    limitHint: rateLimitHint,
    trajectory: rateTrajectory,
  },
  position: {
    unit: "rad", limitUnit: "rad/s", limitLabel: "velocity limit", limitZero: "max", fallback: 62.83,
    modes: ["MODE_FOC_POSITION"],
    rating: (info) => info?.max_position_rad,
    extras: [ACCEL, AT_POINT],
    wireFields: (p) => ({
      position_rad: p.value, rate_limit: p.limit, accel_limit_radps2: p.accel ?? 0, velocity_at_point_radps: p.at_point ?? 0,
    }),
    boundLimits: boundPositionLimits,
    limitHint: (info) => `${atMost(info?.max_velocity_radps, "rad/s")}empty or 0 is the maximum`,
    trajectory: positionTrajectory,
  },
};

/** Every extra limit field any kind carries, once each. */
export const EXTRA_FIELDS = [...new Set(Object.values(SETPOINT_KINDS).flatMap((k) => k.extras))];

export const kindOf = (mode) => Object.keys(SETPOINT_KINDS).find((k) => SETPOINT_KINDS[k].modes.includes(mode)) ?? null;
export const kindFor = (mode) => SETPOINT_KINDS[kindOf(mode)] ?? null;

/** The kind's rating, else its fixed fallback. */
export function boundOf(k, info) {
  const rated = k.rating(info);
  return Number.isFinite(rated) ? rated : k.fallback;
}

/** A setpoint's limits held: at or above their minimums, then within the kind's bounds. */
export function boundLimits(k, p, info) {
  p.limit = Number.isFinite(p.limit) ? Math.max(0, p.limit) : 0;
  for (const e of k.extras) if (e.min !== undefined && Number.isFinite(p[e.key])) p[e.key] = Math.max(e.min, p[e.key]);
  k.boundLimits?.(p, info);
}
