// Platform-aware input mapping. macOS reserves Ctrl+click as the secondary
// click (the OS synthesizes a context menu — WKWebView popped "Reload /
// Inspect Element" over the gesture on the bench), so the comparison-anchor
// modifier is Command there and Ctrl everywhere else. The UI zoom hotkeys
// stay Ctrl on every platform.

/** "Meta" | "Control" for a platform string — pure, so the suite can pin
 *  the mapping for both families regardless of the host it runs on. */
export function anchorModifierKeyFor(platform) {
  return /mac/i.test(platform || "") ? "Meta" : "Control";
}

// Chromium exposes userAgentData.platform ("macOS"/"Windows"); WKWebView
// has no userAgentData and reports navigator.platform ("MacIntel").
export const PLATFORM = navigator.userAgentData?.platform ?? navigator.platform ?? "";

/** The KeyboardEvent.key of the anchor modifier on this host. */
export const ANCHOR_KEY = anchorModifierKeyFor(PLATFORM);

/** True while an event carries the anchor modifier. */
export function isAnchorModifier(ev) {
  return ANCHOR_KEY === "Meta" ? ev.metaKey : ev.ctrlKey;
}
