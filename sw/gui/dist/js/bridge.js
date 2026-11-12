// The one place the frontend meets Tauri. In a plain browser (visual QA)
// the clearly-marked dev mock stands in; it can never load under Tauri.
const tauri = window.__TAURI__;
export const isTauri = Boolean(tauri);

let mock = null;
async function devmock() {
  if (!mock) mock = (await import("./devmock.js")).mock;
  return mock;
}

export async function invoke(cmd, args = {}) {
  if (isTauri) return tauri.core.invoke(cmd, args);
  return (await devmock()).invoke(cmd, args);
}

export async function listen(event, handler) {
  if (isTauri) return tauri.event.listen(event, (e) => handler(e.payload));
  return (await devmock()).listen(event, handler);
}

/** Native file-open dialog via the dialog plugin; returns a path or null. */
export async function pickFile(filters) {
  if (isTauri) {
    return tauri.core.invoke("plugin:dialog|open", {
      options: { multiple: false, directory: false, filters },
    });
  }
  return (await devmock()).pickFile();
}
