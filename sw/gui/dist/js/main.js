// Boot: hydrate the persisted session context, reattach to the core session
// (a webview reload must not disturb it) or restore the saved one, wire the
// event stream, render the chrome.

import { attachEvents, subscribe, store, notify, set, api, prefs, initPrefs } from "./state.js";
import { initChrome } from "./chrome.js";
import { initPicker } from "./picker.js";
import { initLogPane } from "./logpane.js";
import { initWorkspace } from "./workspace/index.js";
import { initPerf } from "./perf.js";
import { icon } from "./icons.js";

function renderEmptyWorkspace() {
  const host = document.querySelector(".workspace-empty");
  const count = store.elf.signalCount;
  host.innerHTML = `
    <div class="empty-drop" data-drop="workspace">${icon("plus")}drop a signal here</div>
    <div class="empty-copy">
      <h2 class="empty-title display">An empty bench</h2>
      <p class="empty-body">
        Drag any of the ${count || ""} firmware variables from the left onto the canvas.
        A plot appears where you drop it; drop onto an existing plot to overlay.
      </p>
      <div class="empty-actions">
        <button class="btn btn-primary btn-lg" data-workspace="new-plot">${icon("plus")}New plot widget</button>
        <button class="btn btn-secondary btn-lg" data-workspace="new-table">${icon("plus")}New table widget</button>
        <button class="btn btn-secondary btn-lg" data-workspace="load-layout" disabled>Load layout…</button>
      </div>
    </div>`;
}

// [impl->app~arch_002~1] Session restore: re-enter the saved context at
// launch. A live core session (webview reload) wins over reconnecting;
// every failed step lands in its normal manual state (pre-connect picker,
// no-elf chip); the watch list reinstalls through watchflow's gate hook
// the moment the restored session derives to "matched".
async function restoreSession() {
  const status = await api.getStatus();
  if (status.connected) {
    set({
      connection: { state: "connected", port: status.port, buildId: status.device_build_id },
      lastPort: status.port,
    });
  }
  const ports = await api.listPorts();
  const savedElf = prefs.get("cockpit.session.elf");
  if (savedElf && !store.elf.buildId) {
    await api.loadElf(savedElf).catch(() => {}); // unreadable: the no-elf state stands
  }
  const savedPort = prefs.get("cockpit.session.port");
  if (!status.connected && savedPort) {
    if (ports.some((p) => p.name === savedPort)) {
      await api.connect(savedPort).catch(() => {}); // failure: the pre-connect state stands
    } else {
      set({ lastPort: savedPort }); // absent: prime the picker's selection only
    }
  }
}

async function boot() {
  await initPrefs(); // before any module reads a persisted preference
  initChrome();
  initPicker();
  initLogPane();
  renderEmptyWorkspace();
  initWorkspace();
  initPerf();
  subscribe("elf", renderEmptyWorkspace);

  // Workspace-creation intents: the workspace module subscribes to these.
  document.querySelector(".workspace").addEventListener("click", (ev) => {
    const btn = ev.target.closest("[data-workspace]");
    if (btn && !btn.disabled) notify("workspace-create", btn.dataset.workspace);
  });

  await attachEvents();
  await restoreSession();
}

boot();
