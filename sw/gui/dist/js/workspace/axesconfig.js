// Widget configuration popover, opened from a widget's ⋯ menu (or its
// derived axis-mode chip). Plot widgets get the full surface — per-signal
// axis side (L/R), remove, the trace-appearance editor (color, line style,
// dots, interpolation — global per signal), and per-side scale mode (auto,
// or manual min–max); table widgets get the signal rows and remove only:
// they have no axes, and a signal's appearance is edited from any plot
// holding it.
// Removing a signal no other widget holds also removes its watch, so the
// budgets recompute and the list re-commits (watchflow's normal path).
// [impl->app~views_007~1] (the axes-configuration surface)
// [impl->app~views_011~1] (the appearance editor)

import { store } from "../state.js";
import { holdersOf } from "./layout.js";
import { removeWatch } from "./watchflow.js";
import { appearanceOf, setAppearance, resolvedColor, effectiveStyle } from "./appearance.js";

const esc = (s) =>
  String(s).replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));
const shortName = (path) => path.split(".").pop();

let open = null; // { el, widget, appearancePath }

export function closeAxesConfig() {
  open?.el.remove();
  open = null;
}

/** Close if the popover belongs to `widget` (widget teardown path). */
export function closeAxesConfigFor(widget) {
  if (open?.widget === widget) closeAxesConfig();
}

export function toggleAxesConfig(widget, anchor) {
  if (open?.widget === widget) {
    closeAxesConfig();
    return;
  }
  closeAxesConfig();
  const el = document.createElement("div");
  el.className = "axes-popover";
  el.setAttribute("role", "dialog");
  el.setAttribute("aria-label", "Widget configuration");
  document.body.appendChild(el);
  open = { el, widget, appearancePath: null };
  render();
  position(el, anchor);
  wire(el, widget);
}

function position(el, anchor) {
  const a = anchor.getBoundingClientRect();
  const r = el.getBoundingClientRect();
  const left = Math.max(8, Math.min(a.right - r.width, window.innerWidth - r.width - 8));
  const top = Math.min(a.bottom + 6, window.innerHeight - r.height - 8);
  el.style.left = `${left}px`;
  el.style.top = `${Math.max(8, top)}px`;
}

function isPlot(widget) {
  return typeof widget.setSide === "function";
}

/** The per-signal appearance editor (app~views_011): color swatches (the six
 *  cycle tokens + auto + arbitrary), line style, sample dots, interpolation. */
function appearanceEditor(path) {
  const a = appearanceOf(path);
  const swatches = [...Array(6)]
    .map(
      (_, i) => `<button class="swatch" data-color="var(--trace-${i + 1})"
        style="background:var(--trace-${i + 1})" aria-label="Trace color ${i + 1}"></button>`,
    )
    .join("");
  const seg = (name, opts, current) =>
    `<span class="side-seg appearance-seg" role="group" aria-label="${name}">
       ${opts
         .map(
           ([val, label]) =>
             `<button data-${name}="${val}" class="${val === current ? "is-selected" : ""}">${label}</button>`,
         )
         .join("")}
     </span>`;
  return `<div class="axes-appearance" data-path="${esc(path)}">
    <div class="appearance-line">
      <span class="appearance-label">color</span>
      ${swatches}
      <input type="color" data-colorpick value="${toHex(resolvedColor(path))}" aria-label="Arbitrary color">
      <button class="swatch swatch--auto ${a.color ? "" : "is-selected"}" data-color="">auto</button>
    </div>
    <div class="appearance-line">
      <span class="appearance-label">style</span>
      ${seg("style", [["solid", "solid"], ["dotted", "dotted"], ["dashed", "dashed"]], effectiveStyle(path))}
      <button class="dots-toggle ${a.dots ? "is-selected" : ""}" data-dots>dots</button>
    </div>
    <div class="appearance-line">
      <span class="appearance-label">interp</span>
      ${seg("interp", [["zoh", "ZOH"], ["linear", "linear"], ["cubic", "cubic"]], a.interp)}
    </div>
  </div>`;
}

/** var(--trace-N) → resolved #rrggbb for the native color input's value. */
function toHex(color) {
  const probe = document.createElement("i");
  probe.style.color = color;
  document.body.appendChild(probe);
  const rgb = getComputedStyle(probe).color.match(/\d+/g) || [0, 0, 0];
  probe.remove();
  return `#${rgb.slice(0, 3).map((v) => (+v).toString(16).padStart(2, "0")).join("")}`;
}

function render() {
  if (!open) return;
  const { el, widget } = open;
  const plot = isPlot(widget);

  const rows = widget.cfg.signals
    .map((path) => {
      const color = store.watched.get(path)?.color || resolvedColor(path);
      const side = plot ? widget.sideOf(path) : null;
      const editing = plot && open.appearancePath === path;
      return `<div class="axes-row ${editing ? "axes-row--editing" : ""}" data-path="${esc(path)}">
        ${
          plot
            ? `<button class="swatch-btn" data-appearance aria-label="Trace appearance"
                 aria-expanded="${editing}"><span class="legend-bar" style="background:${color}"></span></button>`
            : `<span class="legend-bar" style="background:${color}"></span>`
        }
        <span class="axes-name mono" title="${esc(path)}">${esc(shortName(path))}</span>
        ${
          plot
            ? `<span class="side-seg" role="group" aria-label="Axis side">
                 <button data-side="L" class="${side === "L" ? "is-selected" : ""}">L</button>
                 <button data-side="R" class="${side === "R" ? "is-selected" : ""}">R</button>
               </span>`
            : ""
        }
        <button class="axes-remove" data-deselect>remove</button>
      </div>
      ${editing ? appearanceEditor(path) : ""}`;
    })
    .join("");

  const scales = plot
    ? widget
        .scaleGroups()
        .map((g) => {
          const sc = widget.scaleConfig(g.key);
          const manual = sc.mode === "manual";
          return `<div class="axes-scale" data-scale="${g.key}">
            <span class="axes-scale-label">${g.key === "L" ? "left" : "right"} axis</span>
            <span class="side-seg scale-seg" role="group" aria-label="Scale mode">
              <button data-mode="auto" class="${manual ? "" : "is-selected"}">auto</button>
              <button data-mode="manual" class="${manual ? "is-selected" : ""}">manual</button>
            </span>
            <span class="scale-range" ${manual ? "" : "hidden"}>
              <input class="mono" data-rmin inputmode="decimal" aria-label="Minimum" value="${manual ? sc.min : ""}">
              <span class="scale-range-dash">–</span>
              <input class="mono" data-rmax inputmode="decimal" aria-label="Maximum" value="${manual ? sc.max : ""}">
            </span>
          </div>`;
        })
        .join("")
    : "";

  el.innerHTML = `
    <div class="axes-title display">${plot ? "Axes" : "Signals"}</div>
    <div class="axes-rows">${rows || `<div class="axes-empty">no signals — drop one on the widget</div>`}</div>
    ${scales}
    <div class="axes-foot"><button class="btn btn-secondary" data-removewidget>Remove widget</button></div>`;
}

function wire(el, widget) {
  el.addEventListener("click", (ev) => {
    const row = ev.target.closest(".axes-row");
    const editor = ev.target.closest(".axes-appearance");

    if (ev.target.closest("[data-appearance]") && row) {
      open.appearancePath = open.appearancePath === row.dataset.path ? null : row.dataset.path;
      render();
      return;
    }
    if (editor) {
      const path = editor.dataset.path;
      const t = ev.target;
      if (t.dataset.color !== undefined) {
        // A cycle swatch stores its RESOLVED hex: an override is absolute and
        // survives theme switches exactly as chosen (auto = null retints).
        setAppearance(path, { color: t.dataset.color ? toHex(t.dataset.color) : null });
      } else if (t.dataset.style) {
        setAppearance(path, { style: t.dataset.style });
      } else if (t.dataset.dots !== undefined) {
        setAppearance(path, { dots: !appearanceOf(path).dots });
      } else if (t.dataset.interp) {
        setAppearance(path, { interp: t.dataset.interp });
      } else {
        return;
      }
      render();
      return;
    }
    if (ev.target.dataset.side && row) {
      widget.setSide(row.dataset.path, ev.target.dataset.side);
      render();
      return;
    }
    if (ev.target.dataset.deselect !== undefined && row) {
      const path = row.dataset.path;
      widget.removeSignal(path);
      // Last holder gone → the watch itself goes, budgets recompute, the
      // list re-commits.
      if (holdersOf(path) === 0) removeWatch(path);
      render();
      return;
    }
    const scaleEl = ev.target.closest(".axes-scale");
    if (ev.target.dataset.mode && scaleEl) {
      widget.setScaleMode(scaleEl.dataset.scale, ev.target.dataset.mode);
      render();
      return;
    }
    if (ev.target.dataset.removewidget !== undefined) {
      const id = widget.cfg.id;
      closeAxesConfig();
      widget.hooks.onRemove(id);
    }
  });

  // The native color input commits on change (close of the OS picker).
  el.addEventListener("change", (ev) => {
    if ("colorpick" in ev.target.dataset) {
      const editor = ev.target.closest(".axes-appearance");
      if (editor) {
        setAppearance(editor.dataset.path, { color: ev.target.value });
        render();
      }
      return;
    }
    onScaleChange(ev);
  });
}

// Manual bounds: apply only a finite min < max pair; otherwise tint the
// fields and keep the previous valid pair.
function onScaleChange(ev) {
  const widget = open?.widget;
  if (widget) {
    const scaleEl = ev.target.closest(".axes-scale");
    if (!scaleEl || (!("rmin" in ev.target.dataset) && !("rmax" in ev.target.dataset))) return;
    const range = scaleEl.querySelector(".scale-range");
    const min = parseFloat(scaleEl.querySelector("[data-rmin]").value);
    const max = parseFloat(scaleEl.querySelector("[data-rmax]").value);
    if (Number.isFinite(min) && Number.isFinite(max) && min < max) {
      range.classList.remove("is-invalid");
      widget.setManualRange(scaleEl.dataset.scale, min, max);
    } else {
      range.classList.add("is-invalid");
    }
  }
}

// Dismiss on outside click or Esc (once, module-level).
document.addEventListener("pointerdown", (ev) => {
  if (open && !open.el.contains(ev.target) && !ev.target.closest("[data-axesconfig]")) {
    closeAxesConfig();
  }
});
document.addEventListener("keydown", (ev) => {
  if (ev.key === "Escape") closeAxesConfig();
});
window.addEventListener("resize", () => closeAxesConfig());
