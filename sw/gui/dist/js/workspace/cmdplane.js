// The command-plane widgets' shared plumbing: one send that keeps the latest
// send's verdict (Stop never waits behind a Run or Send in flight, the
// client carrying concurrent requests), the send row it renders into, and
// the number format both widgets show.

import { store, api, notify } from "../state.js";
import { isStop } from "./setpoints.js";
import { esc } from "../dom.js";

export const fmt = (v) => (Number.isFinite(v) ? String(+v.toFixed(4)) : "");

export const isLive = () => store.connection.state === "connected";

/** The send row: the primary control (`data-${attr}`), Stop, the verdict. */
export function sendRowHtml(attr, label) {
  return `
    <div class="cmd-row cmd-send">
      <button class="btn btn-primary" data-${attr}>${esc(label)}</button>
      <button class="btn btn-secondary" data-stop title="MODE_OFF, in one press">Stop</button>
      <span class="cmd-verdict" data-verdict></span>
      <span class="cmd-offline" data-offline hidden>no session</span>
    </div>`;
}

/** Send `command` for `widget` ({ verdict, sending, refresh() }); an
 *  accepted one is announced, ending every earlier run's marker. */
export async function sendWithVerdict(widget, command, onAccepted, onRejected) {
  const stop = isStop(command);
  if ((widget.sending && !stop) || !isLive()) return;
  const id = (widget.sendId = (widget.sendId ?? 0) + 1);
  if (!stop) widget.sending = true;
  widget.verdict = { text: "sending…", pending: true };
  widget.refresh();
  let v;
  try {
    v = await api.sendCommand(command);
  } catch (e) {
    v = { accepted: false, cause: String(e) };
  }
  if (!stop) widget.sending = false;
  if (id === widget.sendId) {
    widget.verdict = v.accepted ? { text: "accepted" } : { text: v.cause || "rejected", rejected: true };
  }
  if (v.accepted) {
    notify("command-accepted", widget);
    onAccepted?.();
  } else {
    onRejected?.();
  }
  widget.refresh();
}

/** The send row's state: the primary control waits on its own send, Stop
 *  only on the session. */
export function renderSendRow(widget, primary) {
  const live = isLive();
  widget.el.querySelector(primary).disabled = !live || widget.sending;
  widget.el.querySelector("[data-stop]").disabled = !live;
  widget.el.querySelector("[data-offline]").hidden = live;
  const v = widget.verdict;
  const verdict = widget.el.querySelector("[data-verdict]");
  verdict.textContent = v?.text ?? "";
  verdict.classList.toggle("cmd-verdict--rejected", Boolean(v?.rejected));
  verdict.classList.toggle("cmd-verdict--accepted", Boolean(v) && !v.rejected && !v.pending);
}
