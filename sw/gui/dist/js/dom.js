// Shared DOM helpers — one home for the idioms every module pasted.

export const $ = (sel) => document.querySelector(sel);

export const esc = (s) =>
  String(s).replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));

export const shortName = (path) => path.split(".").pop();
