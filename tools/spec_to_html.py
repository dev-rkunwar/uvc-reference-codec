#!/usr/bin/env python3
"""Convert UVC_Specification.md -> docs/index.html (gh-pages) with stdlib only.

Minimal, faithful markdown->HTML: headings, fenced code blocks, paragraphs,
bold/code spans, and a generated table of contents. No external deps.
"""
import re
import html
import sys
import os

SRC = os.path.join(os.path.dirname(__file__), "..", "UVC_Specification.md")
OUT = os.path.join(os.path.dirname(__file__), "..", "docs", "index.html")


def inline(text):
    # order: escape first, then code spans, then bold, then italic
    text = html.escape(text, quote=False)
    text = re.sub(r"`([^`]+)`", lambda m: "<code>" + m.group(1) + "</code>", text)
    text = re.sub(r"\*\*([^*]+)\*\*", lambda m: "<strong>" + m.group(1) + "</strong>", text)
    text = re.sub(r"\*([^*]+)\*", lambda m: "<em>" + m.group(1) + "</em>", text)
    return text


def main():
    with open(SRC, "r", encoding="utf-8") as f:
        lines = f.read().split("\n")

    out = []
    toc = []
    in_code = False
    code_buf = []

    out.append("""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>UVC — Universal Video Codec Specification</title>
<style>
  :root { color-scheme: light dark; }
  body { font: 15px/1.6 -apple-system, Segoe UI, Roboto, Helvetica, Arial, sans-serif;
         max-width: 880px; margin: 2rem auto; padding: 0 1rem; color: #1b1f24; }
  @media (prefers-color-scheme: dark) { body { color: #d6dde6; background: #0d1117; } a { color: #79c0ff; } code,pre { background:#161b22; } }
  h1,h2,h3 { line-height: 1.25; }
  h1 { border-bottom: 2px solid #8883; padding-bottom: .3rem; }
  h2 { border-bottom: 1px solid #8882; padding-bottom: .2rem; margin-top: 2.2rem; }
  h3 { margin-top: 1.6rem; }
  code { background: #8881; padding: .1em .35em; border-radius: 4px; font-size: 90%; }
  pre { background: #8881; padding: 1rem; border-radius: 8px; overflow:auto; }
  pre code { background: none; padding: 0; }
  a { color: #0969da; text-decoration: none; }
  a:hover { text-decoration: underline; }
  nav.toc { background:#8881; padding:.8rem 1.2rem; border-radius:8px; margin:1.5rem 0; }
  nav.toc ul { margin: 0; padding-left: 1.2rem; }
  nav.toc li { margin: .15rem 0; }
</style>
</head>
<body>
<h1>UVC — Universal Video Codec Specification</h1>
<nav class="toc"><strong>Contents</strong><ul>
""")

    for ln in lines:
        if ln.strip().startswith("```"):
            if not in_code:
                in_code = True
                code_buf = []
            else:
                in_code = False
                out.append("<pre><code>" + html.escape("\n".join(code_buf), quote=False) + "</code></pre>")
            continue
        if in_code:
            code_buf.append(ln)
            continue
        m = re.match(r"^(#{1,3})\s+(.*)$", ln)
        if m:
            level = len(m.group(1))
            title = m.group(2).strip()
            anchor = re.sub(r"[^a-z0-9]+", "-", title.lower()).strip("-")
            tag = "h%d" % level
            out.append('<a id="%s"></a><%s>%s</%s>' % (anchor, tag, inline(title), tag))
            if level <= 3:
                toc.append('  <li><a href="#%s">%s</a></li>' % (anchor, html.escape(title)))
            continue
        if ln.strip() == "":
            continue
        out.append("<p>" + inline(ln) + "</p>")

    # Header closes the Contents <ul> by appending the collected TOC items, then </nav>
    out.append("</ul></nav>" if False else "")
    out.append("\n".join(toc))
    out.append("</ul></nav>")
    out.append("""
<hr>
<p><em>Generated from <code>UVC_Specification.md</code> by <code>tools/spec_to_html.py</code>.
This document is the Tier-1 reference scaffold specification.</em></p>
</body>
</html>""")

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8") as f:
        f.write("\n".join(out))
    print("wrote", os.path.abspath(OUT))


if __name__ == "__main__":
    main()
