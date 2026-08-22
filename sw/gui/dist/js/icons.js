// Lucide icon paths (inlined — no runtime fetch), drawn with the theme's
// --icon-stroke / --icon-cap voice. One set of paths, three voices.
const PATHS = {
  search: '<circle cx="11" cy="11" r="8"/><path d="m21 21-4.3-4.3"/>',
  crosshair: '<circle cx="12" cy="12" r="10"/><path d="M22 12h-4M6 12H2M12 6V2M12 22v-4"/>',
  activity: '<path d="M22 12h-4l-3 9L9 3l-3 9H2"/>',
  "more-horizontal": '<path d="M5 12h.01M12 12h.01M19 12h.01"/>',
  plus: '<path d="M5 12h14M12 5v14"/>',
  "grip-vertical": '<path d="M9 5h.01M9 12h.01M9 19h.01M15 5h.01M15 12h.01M15 19h.01"/>',
  "alert-triangle":
    '<path d="m10.29 3.86-8.19 14.2A2 2 0 0 0 3.83 21h16.34a2 2 0 0 0 1.73-2.94l-8.19-14.2a2 2 0 0 0-3.42 0z"/><path d="M12 9v4"/><path d="M12 17h.01"/>',
  "chevron-left": '<path d="m15 18-6-6 6-6"/>',
  "chevron-right": '<path d="m9 18 6-6-6-6"/>',
  "chevron-down": '<path d="m6 9 6 6 6-6"/>',
  terminal: '<path d="m4 17 6-6-6-6"/><path d="M12 19h8"/>',
};

/** Inline SVG markup for a named Lucide icon. */
export function icon(name, cls = "icon") {
  const paths = PATHS[name] || "";
  return `<svg class="${cls}" viewBox="0 0 24 24" aria-hidden="true">${paths}</svg>`;
}
