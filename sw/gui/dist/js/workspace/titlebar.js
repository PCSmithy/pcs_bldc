// Widget title editing: the header title is click-to-edit — Enter or blur
// commits, Escape cancels; a committed empty name unsets the title so the
// widget's derived name returns. The editor swaps in for the title span at
// the same type size, so the head row keeps its layout.
// [impl->app~views_016~1]

/** Re-derive the shown title from the widget's current state. */
export function updateTitle(widget) {
  const span = widget.el.querySelector(".widget-title");
  if (span) span.textContent = widget.title();
}

export function wireTitleEditor(widget) {
  const span = widget.el.querySelector(".widget-title");
  span.addEventListener("click", (ev) => {
    ev.stopPropagation();
    if (widget.el.querySelector(".widget-title-edit")) return;
    const input = document.createElement("input");
    input.className = "widget-title-edit display";
    input.value = widget.cfg.title ?? "";
    input.placeholder = widget.title();
    span.hidden = true;
    span.after(input);
    const close = (commit) => {
      input.removeEventListener("blur", onBlur);
      if (commit) {
        const name = input.value.trim();
        if (name) widget.cfg.title = name;
        else delete widget.cfg.title;
        widget.hooks.onChange();
      }
      input.remove();
      span.hidden = false;
      updateTitle(widget);
    };
    const onBlur = () => close(true);
    input.addEventListener("blur", onBlur);
    input.addEventListener("keydown", (kev) => {
      if (kev.key === "Enter") close(true);
      else if (kev.key === "Escape") close(false);
      kev.stopPropagation();
    });
    // The head is the drag handle — an editing gesture must never move
    // the widget.
    input.addEventListener("pointerdown", (pev) => pev.stopPropagation());
    input.focus();
    input.select();
  });
}
