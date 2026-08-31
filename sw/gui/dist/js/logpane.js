// Firmware log pane. LogText arrives in arbitrary chunks; lines assemble at
// newline boundaries and are timestamped host-side on completion. The text
// itself is firmware output and is never reformatted.

import { icon } from "./icons.js";
import { subscribe, prefs } from "./state.js";
import { $, esc } from "./dom.js";

const MAX_LINES = 2000; // ring: drop oldest past this

let pending = "";
let lines = [];

function ts() {
  const d = new Date();
  const p = (n, w = 2) => String(n).padStart(w, "0");
  return `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}.${p(d.getMilliseconds(), 3)}`;
}

const lineHtml = (l) => `<div><span class="log-ts">${l.ts}</span>${esc(l.text)}</div>`;

// [impl->app~views_003~1] Completed lines APPEND — a chunk never re-renders
// the whole backlog.
function push(text) {
  pending += text;
  let nl;
  const fresh = [];
  while ((nl = pending.indexOf("\n")) >= 0) {
    fresh.push({ ts: ts(), text: pending.slice(0, nl) });
    pending = pending.slice(nl + 1);
  }
  if (!fresh.length) return;
  lines.push(...fresh);
  const host = $(".log-lines");
  const stick = host.scrollTop + host.clientHeight >= host.scrollHeight - 8;
  host.insertAdjacentHTML("beforeend", fresh.map(lineHtml).join(""));
  const drop = lines.length - MAX_LINES;
  if (drop > 0) {
    lines = lines.slice(drop);
    for (let i = 0; i < drop; i++) host.firstElementChild?.remove();
  }
  $(".log-count").textContent = `${lines.length} lines`;
  if (stick) host.scrollTop = host.scrollHeight;
}

function render() {
  const host = $(".log-lines");
  host.innerHTML = lines.map(lineHtml).join("");
  $(".log-count").textContent = `${lines.length} lines`;
  host.scrollTop = host.scrollHeight;
}

export function initLogPane() {
  const pane = $(".log-pane");
  pane.innerHTML = `
    <div class="log-head">
      ${icon("terminal")}
      <span class="field-label log-title">Firmware log</span>
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
