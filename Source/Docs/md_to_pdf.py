"""Renders a project Markdown document as a PDF in the module-guide style.

    python Source\\Docs\\md_to_pdf.py Source\\Docs\\literature-review.md

Writes the PDF next to the Markdown file. Supports the subset of Markdown
used in this project's docs: headings, paragraphs, bullet and numbered
lists (one nesting level), tables, block quotes, horizontal rules, fenced
code, **bold**, *italic*, `code` and [links](url).
"""

import os
import re
import sys

from reportlab.lib import colors
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle, getSampleStyleSheet
from reportlab.lib.units import mm
from reportlab.platypus import (HRFlowable, ListFlowable, ListItem, Paragraph, Preformatted,
                                SimpleDocTemplate, Spacer, Table, TableStyle)

NAVY = colors.HexColor("#1F3A5F")
styles = getSampleStyleSheet()
H = {
    1: ParagraphStyle("h1", parent=styles["Heading1"], fontSize=18, textColor=NAVY, spaceAfter=6),
    2: ParagraphStyle("h2", parent=styles["Heading2"], fontSize=13.5, textColor=NAVY,
                      spaceBefore=12, spaceAfter=4),
    3: ParagraphStyle("h3", parent=styles["Heading3"], fontSize=11, textColor=NAVY,
                      spaceBefore=8, spaceAfter=3),
}
BODY = ParagraphStyle("body", parent=styles["BodyText"], fontSize=9.6, leading=13.2, spaceAfter=4)
QUOTE = ParagraphStyle("quote", parent=BODY, leftIndent=8, textColor=colors.HexColor("#333333"),
                       backColor=colors.HexColor("#F3F5F8"), borderPadding=5)
CELL = ParagraphStyle("cell", parent=BODY, fontSize=8.2, leading=10.4, spaceAfter=0)
CELLB = ParagraphStyle("cellb", parent=CELL, fontName="Helvetica-Bold")
CODE = ParagraphStyle("code", parent=styles["Code"], fontSize=7.8, leading=9.6,
                      backColor=colors.HexColor("#F3F5F8"), borderPadding=4)

# Glyphs missing from the built-in Helvetica font.
REPLACE = {"→": "->", "←": "<-", "≤": "<=", "≥": ">=", "†": "†"}


def inline(text):
    for k, v in REPLACE.items():
        text = text.replace(k, v)
    text = text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
    # Pull code spans out first so * and _ inside them are not read as emphasis.
    codes = []

    def stash(m):
        codes.append(m.group(1))
        return f"\x00{len(codes) - 1}\x00"

    text = re.sub(r"`([^`]+)`", stash, text)
    text = re.sub(r"\*\*([^*]+)\*\*", r"<b>\1</b>", text)
    text = re.sub(r"(?<![*\w])\*([^*]+)\*(?!\w)", r"<i>\1</i>", text)
    text = re.sub(r"\[([^\]]+)\]\(([^)]+)\)", r"<u>\1</u>", text)
    return re.sub(r"\x00(\d+)\x00",
                  lambda m: f"<font face='Courier'>{codes[int(m.group(1))]}</font>", text)


def table(rows):
    header, body = rows[0], rows[1:]
    ncol = len(header)
    width = A4[0] - 36 * mm
    # Width proportional to the longest text in each column, within limits.
    lengths = [max(len(r[i]) if i < len(r) else 0 for r in rows) for i in range(ncol)]
    lengths = [min(max(l, 8), 90) for l in lengths]
    total = sum(lengths)
    widths = [width * l / total for l in lengths]
    data = [[Paragraph(inline(c), CELLB) for c in header]]
    for r in body:
        r = r + [""] * (ncol - len(r))
        data.append([Paragraph(inline(c), CELL) for c in r[:ncol]])
    t = Table(data, colWidths=widths, repeatRows=1)
    t.setStyle(TableStyle([
        ("BACKGROUND", (0, 0), (-1, 0), colors.HexColor("#DCE4EE")),
        ("GRID", (0, 0), (-1, -1), 0.4, colors.HexColor("#9AA8B8")),
        ("VALIGN", (0, 0), (-1, -1), "TOP"),
        ("ROWBACKGROUNDS", (0, 1), (-1, -1), [colors.white, colors.HexColor("#F7F9FB")]),
        ("LEFTPADDING", (0, 0), (-1, -1), 4),
        ("RIGHTPADDING", (0, 0), (-1, -1), 4),
    ]))
    return t


def split_row(line):
    return [c.strip() for c in line.strip().strip("|").split("|")]


def make_list(items, ordered):
    flow = []
    for text, children in items:
        parts = [Paragraph(inline(text), BODY)]
        if children:
            parts.append(make_list([(c, []) for c in children], False))
        flow.append(ListItem(parts, leftIndent=12))
    return ListFlowable(flow, bulletType="1" if ordered else "bullet", start="1" if ordered else None,
                        leftIndent=14, bulletFontSize=8 if not ordered else 9.6)


def parse(md):
    story = []
    lines = md.replace("\r\n", "\n").split("\n")
    i = 0
    para = []

    def flush():
        if para:
            story.append(Paragraph(inline(" ".join(para)), BODY))
            para.clear()

    while i < len(lines):
        line = lines[i]
        s = line.strip()
        if s.startswith("```"):
            flush()
            j = i + 1
            block = []
            while j < len(lines) and not lines[j].strip().startswith("```"):
                block.append(lines[j])
                j += 1
            story.append(Preformatted("\n".join(block), CODE))
            i = j + 1
            continue
        m = re.match(r"^(#{1,3})\s+(.*)", s)
        if m:
            flush()
            story.append(Paragraph(inline(m.group(2)), H[len(m.group(1))]))
            i += 1
            continue
        if s == "---":
            flush()
            story.append(HRFlowable(width="100%", color=colors.HexColor("#C5CED9"), spaceBefore=4,
                                    spaceAfter=6))
            i += 1
            continue
        if s.startswith("|"):
            flush()
            rows = []
            while i < len(lines) and lines[i].strip().startswith("|"):
                if not re.match(r"^\|[\s:|-]+\|$", lines[i].strip()):
                    rows.append(split_row(lines[i]))
                i += 1
            story.append(table(rows))
            story.append(Spacer(1, 6))
            continue
        if s.startswith(">"):
            flush()
            quote = []
            while i < len(lines) and lines[i].strip().startswith(">"):
                quote.append(lines[i].strip()[1:].strip())
                i += 1
            story.append(Paragraph(inline(" ".join(quote)), QUOTE))
            story.append(Spacer(1, 4))
            continue
        lm = re.match(r"^(\d+\.|[-*])\s+(.*)", s)
        if lm and not line.startswith("  "):
            flush()
            ordered = lm.group(1)[0].isdigit()
            items = []
            while i < len(lines):
                cur = lines[i]
                top = re.match(r"^(\d+\.|[-*])\s+(.*)", cur)
                sub = re.match(r"^\s{2,}(?:\d+\.|[-*])\s+(.*)", cur)
                if top and not cur.startswith(" "):
                    items.append([top.group(2), []])
                elif sub and items:
                    items[-1][1].append(sub.group(1))
                elif cur.startswith("   ") and cur.strip() and items:
                    target = items[-1][1]
                    if target:
                        target[-1] += " " + cur.strip()
                    else:
                        items[-1][0] += " " + cur.strip()
                else:
                    break
                i += 1
            story.append(make_list([(t, c) for t, c in items], ordered))
            story.append(Spacer(1, 4))
            continue
        if not s:
            flush()
            i += 1
            continue
        if s.startswith("**"):
            flush()  # a line opening with a bold label starts its own paragraph
        para.append(s)
        i += 1
    flush()
    return story


def render(md_path):
    text = open(md_path, encoding="utf-8").read()
    title = re.search(r"^#\s+(.*)", text, re.M).group(1)
    out = os.path.splitext(md_path)[0] + ".pdf"

    def footer(canvas, doc):
        canvas.saveState()
        canvas.setFont("Helvetica", 7.5)
        canvas.setFillColor(colors.HexColor("#777777"))
        canvas.drawString(18 * mm, 10 * mm, "ThreadHaul-Daemon - " + title.replace("–", "-"))
        canvas.drawRightString(A4[0] - 18 * mm, 10 * mm, f"Page {doc.page}")
        canvas.restoreState()

    doc = SimpleDocTemplate(out, pagesize=A4, leftMargin=18 * mm, rightMargin=18 * mm,
                            topMargin=16 * mm, bottomMargin=18 * mm, title=title,
                            author="ThreadHaul-Daemon team")
    doc.build(parse(text), onFirstPage=footer, onLaterPages=footer)
    return out


if __name__ == "__main__":
    for path in sys.argv[1:] or [os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                              "literature-review.md")]:
        print("wrote", render(path))
