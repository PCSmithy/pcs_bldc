// Firmware log pane. LogText arrives in arbitrary chunks; lines assemble at
// newline boundaries and are timestamped host-side on completion. The text
// itself is firmware output and is never reformatted.

import { icon } from "./icons.js";
import { subscribe, store, notify, prefs } from "./state.js";

const $ = (sel) => document.querySelector(sel);
const MAX_LINES = 2000; // ring: drop oldest past this

let pending = "";
let lines = [];

function ts() {
  const d = new Date();
  const p = (n, w = 2) => String(n).padStart(w, "0");
  return `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}.${p(d.getMilliseconds(), 3)}`;
}

// [impl->app~views_003~1]
function push(text) {
  pending += text;
  let nl;
  let added = false;
  while ((nl = pending.indexOf("\n")) >= 0) {
    lines.push({ ts: ts(), text: pending.slice(0, nl) });
    pending = pending.slice(nl + 1);
    added = true;
  }
  if (lines.length > MAX_LINES) lines = lines.slice(-MAX_LINES);
  if (added) render();
}

function render() {
  const host = $(".log-lines");
  const stick = host.scrollTop + host.clientHeight >= host.scrollHeight - 8;
  host.innerHTML = lines
    .map((l) => `<div><span class="log-ts">${l.ts}</span>${l.text.replace(/[&<>]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;" }[c]))}</div>`)
    .join("");
  $(".log-count").textContent = `${lines.length} lines`;
  if (stick) host.scrollTop = host.scrollHeight;
  store.logCount = lines.length;
}

export function initLogPane() {
  const pane = $(".log-pane");
  pane.innerHTML = `
    <div class="log-head">
      ${icon("terminal")}
      <span class="log-title">Firmware log</span>
      <span class="tag tag-ok">printf · live</span>
      <span class="log-meta">
        <span class="log-count">0 lines</span>
        <button class="log-action" data-log="clear">clear</button>
        <button class="log-action" data-log="collapse">collapse ${icon("chevron-down", "icon icon-chevron")}</button>
      </span>
    </div>
    <div class="log-lines" aria-live="off"></div>`;

  if (prefs.get("cockpit.log.collapsed")) pane.classList.add("log-pane--collapsed");

  pane.addEventListener("click", (ev) => {
    const btn = ev.target.closest("[data-log]");
    if (!btn) return;
    if (btn.dataset.log === "clear") {
      lines = [];
      render();
    } else {
      prefs.set("cockpit.log.collapsed", pane.classList.toggle("log-pane--collapsed"));
    }
  });

  subscribe("log", push);
}
