"""Read the S4 captures of a velocity-gain sweep (sil/<label>/S4_velstep_*.csv)
and tabulate each step response: rise, overshoot, settling, steady error, torque
effort, on the plant's velocity. Saves velocity_sweep.png beside the captures.

    .venv/Scripts/python tools/trace_analysis/velocity_loop/analyze.py
"""
import csv
import pathlib
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402

HERE = pathlib.Path(__file__).resolve().parent
EST = "app_rotorEstimation_data.channels[0]"
MC = "app_motorControl_data.channels[0]"
K_T = 0.825
STEP = 10.0      # rad/s, the S4 target
T_UP, T_DOWN = 0.1, 0.7   # s, the two steps
BAND = 0.02 * STEP        # the settling band


def load(path):
    with open(path, newline="") as f:
        reader = csv.reader(f)
        header = next(reader)
        a = np.array([[float(c) if c else np.nan for c in row] for row in reader])
    idx = {h: k for k, h in enumerate(header)}
    return {
        "t": a[:, idx["time_ms"]] / 1000.0,
        "plant": a[:, idx["plant.velocity_radps"]],
        "omega_hat": a[:, idx[f"{EST}.x_hat.e[1][0]"]],
        "tau": K_T * a[:, idx[f"{MC}.iQ_a"]],
    }


def step_metrics(t, v, t0, t1, target, start):
    # The response from `start` to `target` between t0 and t1.
    m = (t >= t0) & (t < t1)
    tt, vv = t[m] - t0, v[m]
    span = target - start
    out = {}
    x = (vv - start) / span if span else vv
    k10 = np.argmax(x >= 0.1) if (x >= 0.1).any() else None
    k90 = np.argmax(x >= 0.9) if (x >= 0.9).any() else None
    out["rise ms"] = 1e3 * (tt[k90] - tt[k10]) if (k10 is not None and k90 is not None) else np.nan
    out["overshoot %"] = 100 * (x.max() - 1.0)
    outside = np.flatnonzero(np.abs(vv - target) > BAND)
    out["settle ms"] = 1e3 * tt[outside[-1]] if len(outside) else 0.0
    tail = tt >= (tt[-1] - 0.2)
    out["steady err"] = np.mean(vv[tail] - target)
    out["steady rms"] = np.sqrt(np.mean((vv[tail] - target) ** 2))
    return out


def main():
    root = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else HERE / "sil"
    runs = {}
    for d in sorted(root.iterdir()):
        files = sorted(d.glob("S4_velstep_*.csv"))
        if d.is_dir() and files:
            runs[d.name] = load(files[0])
    if not runs:
        sys.exit(f"no captures under {root}")

    names = ["rise ms", "overshoot %", "settle ms", "steady err", "steady rms", "peak Nm", "stop under %", "stop settle ms"]
    print(f"{'gains':10s}" + "".join(f"{n:>15s}" for n in names))
    for label, r in runs.items():
        up = step_metrics(r["t"], r["plant"], T_UP, T_DOWN, STEP, 0.0)
        down = step_metrics(r["t"], r["plant"], T_DOWN, r["t"][-1], 0.0, STEP)
        m = (r["t"] >= T_UP) & (r["t"] < T_DOWN)
        row = {**up, "peak Nm": np.nanmax(np.abs(r["tau"][m])),
               "stop under %": -100 * np.min(r["plant"][r["t"] >= T_DOWN]) / STEP,
               "stop settle ms": down["settle ms"]}
        print(f"{label:10s}" + "".join(f"{row[n]:15.3f}" for n in names))

    fig, axes = plt.subplots(2, 1, figsize=(10, 6), sharex=True)
    for label, r in runs.items():
        axes[0].plot(r["t"], r["plant"], lw=0.8, label=label)
        axes[1].plot(r["t"], r["tau"], lw=0.5, label=label)
    axes[0].axhline(STEP, color="0.6", ls=":", lw=0.8)
    axes[0].set_ylabel("plant velocity [rad/s]")
    axes[0].set_xlim(0.05, 1.0)
    axes[0].legend(fontsize=8, ncol=5)
    axes[1].set_ylabel("K_t I_q [Nm]")
    axes[1].set_xlabel("t [s]")
    fig.suptitle("S4 velocity step, SIL, bench-like plant: velocity gains swept")
    fig.tight_layout()
    out = root / "velocity_sweep.png"
    fig.savefig(out, dpi=100)
    print("plot:", out)


if __name__ == "__main__":
    main()
