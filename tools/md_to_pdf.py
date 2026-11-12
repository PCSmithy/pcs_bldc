#!/usr/bin/env python
"""Render a Markdown note with $...$ / $$...$$ math to PDF (and optionally HTML).

Uses the venv's Playwright Chromium with marked + MathJax (SVG output) from
cdnjs, loaded on every run, so it needs network. Usage:

    .venv/Scripts/python tools/md_to_pdf.py docs/references/note.md [--html out.html] [--eyebrow "text"]

The PDF lands beside the Markdown with the same stem unless --pdf is given.
"""
import argparse
import pathlib
import sys

TEMPLATE = r"""<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>/*TITLE*/</title>
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Source+Serif+4:ital,opsz,wght@0,8..60,400;0,8..60,600;1,8..60,400&family=IBM+Plex+Sans:wght@500;600&family=IBM+Plex+Mono:wght@400&display=swap">
<style>
  :root {
    --bg: #f6f7f7; --fg: #1b2227; --muted: #5b6871; --accent: #0e6a70;
    --rule: #d6dcdf; --code-bg: #e9eeef;
    --font-body: "Source Serif 4", Georgia, "Times New Roman", serif;
    --font-head: "IBM Plex Sans", "Segoe UI", Helvetica, Arial, sans-serif;
    --font-mono: "IBM Plex Mono", Consolas, "Courier New", monospace;
  }
  body { margin: 0; background: var(--bg); color: var(--fg); font-family: var(--font-body);
         font-size: 1.0625rem; line-height: 1.55; padding: 2.5rem 16px 4rem; }
  article { max-width: 42rem; margin: 0 auto; min-width: 0; }
  .eyebrow { font-family: var(--font-head); font-size: 0.75rem; font-weight: 600; letter-spacing: 0.08em;
             text-transform: uppercase; color: var(--accent); display: flex; flex-wrap: wrap;
             gap: 0.25rem 1.25rem; margin-bottom: 0.75rem; }
  .eyebrow span + span { color: var(--muted); }
  h1, h2, h3 { font-family: var(--font-head); line-height: 1.2; }
  h1 { font-size: 2rem; font-weight: 600; margin: 0 0 1rem; }
  h2 { font-size: 1.25rem; font-weight: 600; margin: 2.5rem 0 0.75rem; padding-top: 1rem; border-top: 1px solid var(--rule); }
  h3 { font-size: 1.05rem; font-weight: 600; margin: 1.5rem 0 0.5rem; }
  p { margin: 0 0 1rem; }
  article > p:first-of-type { font-size: 1.125rem; color: var(--muted); }
  code { font-family: var(--font-mono); font-size: 0.85em; background: var(--code-bg); padding: 0.05em 0.3em; border-radius: 3px; }
  pre { background: var(--code-bg); padding: 0.75rem 1rem; overflow-x: auto; border-radius: 4px; }
  pre code { background: none; padding: 0; }
  mjx-container[display="true"] { display: block; overflow-x: auto; overflow-y: hidden; margin: 1.1rem 0 1.25rem; padding: 0.15rem 0; }
  .table-wrap { overflow-x: auto; margin: 1rem 0 1.25rem; }
  table { border-collapse: collapse; width: 100%; font-size: 0.9375rem; font-variant-numeric: tabular-nums; }
  th, td { text-align: left; padding: 0.4rem 0.6rem; border-bottom: 1px solid var(--rule); vertical-align: top; }
  th { font-family: var(--font-head); font-size: 0.75rem; font-weight: 600; letter-spacing: 0.04em; text-transform: uppercase; color: var(--muted); }
  td[align="right"], th[align="right"] { text-align: right; }
  .colophon { margin-top: 3rem; padding-top: 1rem; border-top: 1px solid var(--rule); font-family: var(--font-head); font-size: 0.8125rem; color: var(--muted); }
  @media print {
    :root { --bg: #ffffff; --fg: #111111; --muted: #555555; --rule: #cccccc; --code-bg: #f0f0f0; }
    body { font-size: 10.5pt; padding: 0; }
    article { max-width: none; }
    h2, h3 { break-after: avoid; }
    mjx-container[display="true"], table, p, pre { break-inside: avoid; }
    @page { size: Letter; margin: 20mm 18mm; }
  }
</style></head><body>
<article>
  <div class="eyebrow">/*EYEBROW*/</div>
  <div id="content"></div>
  <p class="colophon">Source: <code>/*SOURCE*/</code>, rendered by <code>tools/md_to_pdf.py</code> (marked + MathJax).</p>
</article>
<script type="text/plain" id="md">/*MD*/</script>
<script>
  window.MathJax = { tex: { inlineMath: [["$", "$"]], displayMath: [["$$", "$$"]], processEscapes: false },
                     svg: { fontCache: "none" }, startup: { typeset: false } };
</script>
<script src="https://cdnjs.cloudflare.com/ajax/libs/marked/9.1.6/marked.min.js"></script>
<script src="https://cdnjs.cloudflare.com/ajax/libs/mathjax/3.2.2/es5/tex-svg.js"></script>
<script>
  (function () {
    var src = document.getElementById("md").textContent.split("<\\/script>").join("<" + "/script>");
    var math = [];
    function shield(s) { math.push(s); return "@@MATH" + (math.length - 1) + "@@"; }
    src = src.replace(/\$\$[\s\S]*?\$\$/g, shield).replace(/\$(?!\$)[^$\n]+?\$/g, shield);
    var html = marked.parse(src, { gfm: true, mangle: false, headerIds: false });
    html = html.replace(/@@MATH(\d+)@@/g, function (_, i) { return math[+i]; });
    html = html.replace(/<table>/g, '<div class="table-wrap"><table>').replace(/<\/table>/g, "</table></div>");
    document.getElementById("content").innerHTML = html;
    function done() { document.documentElement.setAttribute("data-ready", "1"); }
    MathJax.startup.promise.then(function () { return MathJax.typesetPromise(); }).then(done, done);
  })();
</script>
</body></html>
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("markdown", type=pathlib.Path)
    ap.add_argument("--pdf", type=pathlib.Path, help="output PDF (default: beside the .md)")
    ap.add_argument("--html", type=pathlib.Path, help="also keep the rendered HTML here")
    ap.add_argument("--eyebrow", default="", help="small caps line above the title; '|' splits segments")
    args = ap.parse_args()

    md_path = args.markdown.resolve()
    pdf_path = (args.pdf or md_path.with_suffix(".pdf")).resolve()
    md = md_path.read_text(encoding="utf-8")
    title = next((l[2:].strip() for l in md.splitlines() if l.startswith("# ")), md_path.stem)
    eyebrow = "".join(f"<span>{seg.strip()}</span>" for seg in args.eyebrow.split("|") if seg.strip())
    try:
        source = md_path.relative_to(pathlib.Path(__file__).resolve().parent.parent).as_posix()
    except ValueError:
        source = md_path.name
    html = (TEMPLATE.replace("/*TITLE*/", title).replace("/*EYEBROW*/", eyebrow)
            .replace("/*SOURCE*/", source).replace("/*MD*/", md.replace("</script>", "<\\/script>")))

    html_path = (args.html or pdf_path.with_suffix(".render.html")).resolve()
    html_path.write_text(html, encoding="utf-8")

    from playwright.sync_api import sync_playwright
    errors = []
    with sync_playwright() as p:
        browser = p.chromium.launch()
        page = browser.new_page()
        page.on("pageerror", lambda e: errors.append(str(e)))
        page.goto(html_path.as_uri())
        page.wait_for_selector("html[data-ready='1']", state="attached", timeout=90000)
        page.emulate_media(media="print")
        page.wait_for_timeout(300)
        n_math = page.evaluate("document.querySelectorAll('mjx-container').length")
        n_err = page.evaluate("document.querySelectorAll('mjx-merror').length")
        page.pdf(path=str(pdf_path), format="Letter", print_background=True, prefer_css_page_size=True)
        browser.close()
    if not args.html:
        html_path.unlink()

    print(f"{pdf_path.name}: {pdf_path.stat().st_size} bytes, {n_math} math blocks, {n_err} MathJax errors")
    for e in errors:
        print("page error:", e, file=sys.stderr)
    return 1 if (errors or n_err) else 0


if __name__ == "__main__":
    sys.exit(main())
