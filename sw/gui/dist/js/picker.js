// Signal picker: search over the DWARF namespace, grouped rows, budget
// meters, and the watch panel host. Rows are drag SOURCES (dataTransfer
// "text/x-signal" = path) and re-render from store.watched; period/watch
// interactivity belongs to the workspace module, which owns store.watched
// and the install flow. The column is user-resizable and collapsible to a
// slim rail (persisted).

import { icon } from "./icons.js";
import { api, store, set, subscribe, notify, prefs } from "./state.js";

const $ = (sel) => document.querySelector(sel);
const esc = (s) =>
  String(s).replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));

const groupOf = (path) => path.split(/[.[]/)[0];
const HOT_FRACTION = 0.85; // sage while comfortable, accent as it nears the limit

const PICKER_LS = "cockpit.picker.v1";
const MIN_W = 220;

// The full enumeration, cached so filtering is client-side (the backend's
// list_signals filter is substring-only; the spec's glob/regex forms need
// the whole namespace here).
let allSignals = [];

// The current pattern + the read-only exclusion, composed as intersection.
let currentQuery = "";
let hideConst = false;

// Collapsed group headers (persisted; search leaves them collapsed).
let collapsedGroups = new Set();

// ── the filter (app~obs_005): substring / glob / regex, case-insensitive ──

const escapeRe = (s) => s.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");

// [impl->app~obs_005~1]
export function compileFilter(q) {
  if (!q) return () => true;
  // Literal-only: no regex metacharacters at all — plain substring.
  if (/^[^\\^$.|?*+()[\]{}]+$/.test(q)) {
    const needle = q.toLowerCase();
    return (path) => path.toLowerCase().includes(needle);
  }
  // Literals and '*' only: each '*' matches any run of characters.
  if (/^[^\\^$.|?+()[\]{}]+$/.test(q)) {
    const re = new RegExp(q.split("*").map(escapeRe).join(".*"), "i");
    return (path) => re.test(path);
  }
  // Anything else is a regular expression; one that fails to compile
  // matches as a substring.
  try {
    const re = new RegExp(q, "i");
    return (path) => re.test(path);
  } catch {
    const needle = q.toLowerCase();
    return (path) => path.toLowerCase().includes(needle);
  }
}

// [impl->app~obs_006~1] the read-only exclusion, intersected with the pattern
function refreshPresented() {
  // Until the full-namespace fetch lands, filter whatever is shown.
  const base = allSignals.length ? allSignals : store.signals;
  const match = compileFilter(currentQuery);
  set({ signals: base.filter((s) => match(s.path) && (!hideConst || !s.readonly)) });
}

// ── renderers ──────────────────────────────────────────────────────────────

function renderBudgets() {
  const host = $(".budget-meters");
  const s = store.traceStatus;
  const meter = (label, valueText, frac) => `
    <div class="budget-meter">
      <div class="budget-meter-row">
        <span class="budget-meter-label">${label}</span>
        <span class="budget-meter-value">${valueText}</span>
      </div>
      <div class="budget-track">
        <div class="budget-fill ${frac >= HOT_FRACTION ? "budget-fill--hot" : ""}"
             style="width:${Math.min(100, frac * 100).toFixed(1)}%"></div>
      </div>
    </div>`;
  const ramFrac = s ? s.ram_worst_tick_bytes / s.ram_budget_bytes : 0;
  const linkFrac = s ? s.link_rate_bytes_per_s / s.link_budget_bytes_per_s : 0;
  host.innerHTML = `
    <div class="budget-head">
      <span class="field-label">device budget</span>
      <span class="budget-status ${store.budgetVerdict !== "accepted" && store.budgetVerdict !== "—" ? "budget-status--rejected" : ""}">${esc(store.budgetVerdict)}</span>
    </div>
    ${meter("watch RAM", s ? `${s.ram_worst_tick_bytes} / ${s.ram_budget_bytes} B` : "— / —", ramFrac)}
    ${meter("link bandwidth", s ? `${Math.round(linkFrac * 100)} / 100 %` : "— / —", linkFrac)}`;
}

/** Group names of the currently presented (filtered) list. */
function presentedGroupNames() {
  const names = new Set();
  for (const sig of store.signals) names.add(groupOf(sig.path));
  return names;
}

function allGroupsCollapsed() {
  const names = presentedGroupNames();
  return names.size > 0 && [...names].every((n) => collapsedGroups.has(n));
}

function renderRows() {
  const tree = $(".picker-tree");
  if (store.gate === "no-elf" || store.gate === "offline") {
    tree.innerHTML = "";
    $(".picker-hint").textContent =
      store.gate === "offline"
        ? "Connect to the board, then load its firmware .elf to enumerate signals."
        : "Load the firmware .elf to enumerate its signals.";
    $(".picker-results").textContent = "";
    return;
  }
  $(".picker-hint").textContent =
    "Drag a signal onto a plot to trace it, or onto a table for latest value. Changing the watch list restarts the stream.";
  // Group first, so a collapsed header can carry its hidden-row count.
  const groups = [];
  for (const sig of store.signals) {
    const group = groupOf(sig.path);
    if (!groups.length || groups[groups.length - 1].name !== group) {
      groups.push({ name: group, sigs: [] });
    }
    groups[groups.length - 1].sigs.push(sig);
  }
  let html = "";
  for (const { name, sigs } of groups) {
    const collapsed = collapsedGroups.has(name);
    html += `
      <div class="picker-group ${collapsed ? "picker-group--collapsed" : ""}"
           data-group="${esc(name)}" tabindex="0" role="button" aria-expanded="${!collapsed}">
        ${icon("chevron-down", "icon icon-group-chevron")}
        <span class="picker-group-name">${esc(name)}</span>
        ${collapsed ? `<span class="picker-group-count">${sigs.length}</span>` : ""}
      </div>`;
    if (collapsed) continue;
    for (const sig of sigs) {
      const watched = store.watched.get(sig.path);
      const shortName = sig.path === name ? sig.path : sig.path.slice(name.length).replace(/^\./, "");
      html += `
        <div class="signal-row ${watched ? "signal-row--watched" : ""}" draggable="true"
             data-path="${esc(sig.path)}" title="${esc(sig.path)}">
          <span class="signal-chip" ${watched ? `style="background:${watched.color}"` : ""}></span>
          <span class="signal-name">${esc(shortName)}</span>
          <span class="signal-type">${esc(sig.kind)}</span>
          ${watched ? `<span class="period-pill">${watched.period_ms} ms</span>` : `<span class="signal-add">add</span>`}
        </div>`;
    }
  }
  tree.innerHTML = html;
  const q = $(".picker-search input");
  $(".picker-results").textContent = q && q.value ? `${store.signals.length}` : "";
  // The collapse-all button acts on (and labels itself from) whatever is
  // presented right now, so per-group toggles flip it back naturally.
  const allBtn = $("[data-collapseall]");
  if (allBtn) allBtn.textContent = allGroupsCollapsed() ? "expand all" : "collapse all";
}

// ── column resize + collapse (chrome ergonomics; persisted) ────────────────

function loadPickerPrefs() {
  return prefs.get(PICKER_LS, {}) || {};
}

function savePickerPrefs(patch) {
  prefs.set(PICKER_LS, { ...loadPickerPrefs(), ...patch });
}

function wireColumn(host) {
  const prefs = loadPickerPrefs();
  const maxW = () => Math.max(MIN_W, Math.floor(window.innerWidth / 2));
  const setWidth = (w) => {
    host.style.width = `${Math.max(MIN_W, Math.min(maxW(), w))}px`;
  };
  if (Number.isFinite(prefs.width)) setWidth(prefs.width);
  host.classList.toggle("signal-picker--collapsed", !!prefs.collapsed);

  const resizer = host.querySelector(".picker-resizer");
  let drag = null;
  resizer.addEventListener("pointerdown", (ev) => {
    if (host.classList.contains("signal-picker--collapsed")) return;
    drag = { x0: ev.clientX, w0: host.getBoundingClientRect().width };
    resizer.setPointerCapture(ev.pointerId);
  });
  resizer.addEventListener("pointermove", (ev) => {
    if (!drag) return;
    setWidth(drag.w0 + (ev.clientX - drag.x0));
  });
  const end = () => {
    if (drag) savePickerPrefs({ width: host.getBoundingClientRect().width });
    drag = null;
  };
  resizer.addEventListener("pointerup", end);
  resizer.addEventListener("pointercancel", end);

  const toggle = (collapsed) => {
    host.classList.toggle("signal-picker--collapsed", collapsed);
    savePickerPrefs({ collapsed });
  };
  host.querySelector(".picker-collapse").addEventListener("click", () => toggle(true));
  host.querySelector(".picker-expand").addEventListener("click", () => toggle(false));
}

// ── init ───────────────────────────────────────────────────────────────────

export function initPicker() {
  const host = $(".signal-picker");
  host.innerHTML = `
    <div class="picker-body">
      <div class="picker-head">
        <span class="picker-title display">Signals</span>
        <span class="picker-count"></span>
        <button class="picker-collapse" aria-label="Collapse the signal picker"
          title="Collapse">${icon("chevron-left")}</button>
      </div>
      <label class="picker-search">
        ${icon("search")}
        <input type="search" placeholder="filter · text, *, or regex" aria-label="Filter signals" />
        <span class="picker-results"></span>
      </label>
      <div class="picker-filters">
        <button class="filter-toggle" data-hideconst aria-pressed="false"
          title="Hide signals in read-only storage (const / rodata)">hide const</button>
        <button class="filter-toggle" data-collapseall
          title="Collapse every group — or expand them all back">collapse all</button>
      </div>
      <div class="budget-meters"></div>
      <div class="picker-tree"></div>
      <div class="watch-panel"></div>
      <div class="picker-hint"></div>
    </div>
    <button class="picker-expand" aria-label="Expand the signal picker"
      title="Signals">${icon("chevron-right")}</button>
    <div class="picker-resizer" aria-hidden="true"></div>`;

  const input = host.querySelector("input");
  let debounce = null;
  input.addEventListener("input", () => {
    clearTimeout(debounce);
    debounce = setTimeout(() => {
      currentQuery = input.value;
      refreshPresented();
    }, 120);
  });

  // The read-only exclusion toggle (state persisted with the picker prefs).
  hideConst = !!loadPickerPrefs().hideConst;
  collapsedGroups = new Set(loadPickerPrefs().collapsedGroups || []);
  const constToggle = host.querySelector("[data-hideconst]");
  const syncConstToggle = () => constToggle.setAttribute("aria-pressed", String(hideConst));
  syncConstToggle();
  constToggle.addEventListener("click", () => {
    hideConst = !hideConst;
    savePickerPrefs({ hideConst });
    syncConstToggle();
    refreshPresented();
  });

  // Collapse (or, once nothing is left to collapse, expand) every presented
  // group at once. Only presented names are touched, so a search-narrowed
  // sweep leaves the collapsed state of hidden groups alone.
  host.querySelector("[data-collapseall]").addEventListener("click", () => {
    const expand = allGroupsCollapsed();
    for (const name of presentedGroupNames()) {
      if (expand) collapsedGroups.delete(name);
      else collapsedGroups.add(name);
    }
    savePickerPrefs({ collapsedGroups: [...collapsedGroups] });
    renderRows();
  });

  // Drag source + row activation hooks for the workspace module; group
  // headers toggle their section instead.
  host.addEventListener("dragstart", (ev) => {
    const row = ev.target.closest(".signal-row");
    if (!row) return;
    ev.dataTransfer.setData("text/x-signal", row.dataset.path);
    ev.dataTransfer.effectAllowed = "copy";
  });
  const toggleGroup = (name) => {
    if (collapsedGroups.has(name)) collapsedGroups.delete(name);
    else collapsedGroups.add(name);
    savePickerPrefs({ collapsedGroups: [...collapsedGroups] });
    renderRows();
  };
  host.addEventListener("click", (ev) => {
    const header = ev.target.closest(".picker-group");
    if (header) return toggleGroup(header.dataset.group);
    const row = ev.target.closest(".signal-row");
    if (row) notify("row-activate", row.dataset.path);
  });
  host.addEventListener("keydown", (ev) => {
    const header = ev.target.closest?.(".picker-group");
    if (header && (ev.key === "Enter" || ev.key === " ")) {
      ev.preventDefault();
      toggleGroup(header.dataset.group);
    }
  });

  wireColumn(host);

  subscribe("elf", () => {
    $(".picker-count").textContent = store.elf.signalCount ? `${store.elf.signalCount} from DWARF` : "";
    // Refresh the full-namespace cache the client-side filter runs over,
    // then re-apply the active filters over it.
    if (store.elf.signalCount) {
      api.listSignals("", 100000)
        .then((s) => {
          allSignals = s;
          if (currentQuery || hideConst) refreshPresented();
        })
        .catch(() => {});
    }
  });
  subscribe("signals", (s) => {
    // The initial (or any unfiltered) backend fetch is the cache source.
    if (!input.value && !hideConst && s.length > allSignals.length) allSignals = s;
  });
  for (const topic of ["signals", "watched", "gate"]) subscribe(topic, renderRows);
  for (const topic of ["trace-status", "traceStatus", "budgetVerdict", "budgetPreview"]) subscribe(topic, renderBudgets);
  renderRows();
  renderBudgets();
}
