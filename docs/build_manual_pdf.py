from __future__ import annotations

import html
import re
from pathlib import Path

from reportlab.lib import colors
from reportlab.lib.enums import TA_CENTER
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle, getSampleStyleSheet
from reportlab.lib.units import mm
from reportlab.platypus import (
    Image,
    KeepTogether,
    PageBreak,
    Paragraph,
    Preformatted,
    SimpleDocTemplate,
    Spacer,
    Table,
    TableStyle,
)

ROOT = Path(__file__).resolve().parent
SOURCE = ROOT / "CAD-Converter-2-V0.31-User-Manual.md"
OUTPUT = ROOT / "CAD-Converter-2-V0.31-User-Manual.pdf"

styles = getSampleStyleSheet()
styles.add(ParagraphStyle(
    name="ManualTitle", parent=styles["Title"], fontName="Helvetica-Bold",
    fontSize=22, leading=27, textColor=colors.HexColor("#12365A"), spaceAfter=12,
))
styles.add(ParagraphStyle(
    name="ManualSubtitle", parent=styles["Normal"], fontName="Helvetica",
    fontSize=10, leading=14, alignment=TA_CENTER, textColor=colors.HexColor("#52687E"), spaceAfter=14,
))
styles.add(ParagraphStyle(
    name="ManualH1", parent=styles["Heading1"], fontName="Helvetica-Bold",
    fontSize=16, leading=20, textColor=colors.HexColor("#174F82"), spaceBefore=12, spaceAfter=7,
    keepWithNext=True,
))
styles.add(ParagraphStyle(
    name="ManualH2", parent=styles["Heading2"], fontName="Helvetica-Bold",
    fontSize=12.5, leading=16, textColor=colors.HexColor("#1D6098"), spaceBefore=9, spaceAfter=5,
    keepWithNext=True,
))
styles.add(ParagraphStyle(
    name="ManualBody", parent=styles["BodyText"], fontName="Helvetica",
    fontSize=9.5, leading=13.5, textColor=colors.HexColor("#243444"), spaceAfter=6,
))
styles.add(ParagraphStyle(
    name="ManualBullet", parent=styles["ManualBody"], leftIndent=14, firstLineIndent=-8,
    bulletIndent=3, spaceAfter=3,
))
styles.add(ParagraphStyle(
    name="ManualCode", parent=styles["Code"], fontName="Courier",
    fontSize=8.3, leading=11, leftIndent=8, rightIndent=8, borderPadding=6,
    borderColor=colors.HexColor("#CFDCE8"), borderWidth=0.5,
    backColor=colors.HexColor("#F3F7FA"), spaceBefore=3, spaceAfter=8,
))
styles.add(ParagraphStyle(
    name="ManualCaption", parent=styles["ManualBody"], alignment=TA_CENTER,
    fontSize=8.2, leading=11, textColor=colors.HexColor("#617589"), spaceBefore=3, spaceAfter=10,
))


def inline_markup(text: str) -> str:
    escaped = html.escape(text)
    escaped = re.sub(r"\*\*(.+?)\*\*", r"<b>\1</b>", escaped)
    escaped = re.sub(r"`(.+?)`", r'<font name="Courier">\1</font>', escaped)
    return escaped


def page_footer(canvas, doc):
    canvas.saveState()
    canvas.setStrokeColor(colors.HexColor("#D9E3EC"))
    canvas.line(18 * mm, 13 * mm, A4[0] - 18 * mm, 13 * mm)
    canvas.setFont("Helvetica", 8)
    canvas.setFillColor(colors.HexColor("#6A7D8F"))
    canvas.drawString(18 * mm, 8.5 * mm, "© 2026 R Innovation · CAD Converter 2 V0.31 · Chan Lap Chi")
    canvas.drawRightString(A4[0] - 18 * mm, 8.5 * mm, f"Page {doc.page}")
    canvas.restoreState()


def parse_table(lines: list[str]) -> Table:
    rows = []
    for line in lines:
        cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
        if all(re.fullmatch(r":?-{3,}:?", cell or "") for cell in cells):
            continue
        rows.append([Paragraph(inline_markup(cell), styles["ManualBody"]) for cell in cells])
    table = Table(rows, repeatRows=1, hAlign="LEFT")
    table.setStyle(TableStyle([
        ("BACKGROUND", (0, 0), (-1, 0), colors.HexColor("#DCECF8")),
        ("TEXTCOLOR", (0, 0), (-1, 0), colors.HexColor("#153E61")),
        ("FONTNAME", (0, 0), (-1, 0), "Helvetica-Bold"),
        ("GRID", (0, 0), (-1, -1), 0.45, colors.HexColor("#C6D4E0")),
        ("VALIGN", (0, 0), (-1, -1), "TOP"),
        ("ROWBACKGROUNDS", (0, 1), (-1, -1), [colors.white, colors.HexColor("#F7FAFC")]),
        ("LEFTPADDING", (0, 0), (-1, -1), 5),
        ("RIGHTPADDING", (0, 0), (-1, -1), 5),
        ("TOPPADDING", (0, 0), (-1, -1), 4),
        ("BOTTOMPADDING", (0, 0), (-1, -1), 4),
    ]))
    return table


def build_story(markdown: str):
    lines = markdown.splitlines()
    story = []
    paragraph: list[str] = []
    in_code = False
    code_lines: list[str] = []
    first_title = True

    def flush_paragraph():
        nonlocal paragraph
        if paragraph:
            story.append(Paragraph(inline_markup(" ".join(part.strip() for part in paragraph)), styles["ManualBody"]))
            paragraph = []

    i = 0
    while i < len(lines):
        line = lines[i]
        stripped = line.strip()
        if stripped.startswith("```"):
            flush_paragraph()
            if in_code:
                story.append(Preformatted("\n".join(code_lines), styles["ManualCode"]))
                code_lines = []
                in_code = False
            else:
                in_code = True
            i += 1
            continue
        if in_code:
            code_lines.append(line)
            i += 1
            continue
        if stripped.startswith("!["):
            flush_paragraph()
            match = re.match(r"!\[(.*?)\]\((.*?)\)", stripped)
            if match:
                caption, relative = match.groups()
                image_path = ROOT / relative
                image = Image(str(image_path))
                max_width = 168 * mm
                max_height = 190 * mm
                scale = min(max_width / image.imageWidth, max_height / image.imageHeight)
                image.drawWidth = image.imageWidth * scale
                image.drawHeight = image.imageHeight * scale
                story.append(KeepTogether([image, Paragraph(inline_markup(caption), styles["ManualCaption"])]))
            i += 1
            continue
        if stripped.startswith("|") and i + 1 < len(lines) and lines[i + 1].strip().startswith("|"):
            flush_paragraph()
            table_lines = []
            while i < len(lines) and lines[i].strip().startswith("|"):
                table_lines.append(lines[i])
                i += 1
            story.append(parse_table(table_lines))
            story.append(Spacer(1, 7))
            continue
        if stripped.startswith("# "):
            flush_paragraph()
            if first_title:
                story.append(Paragraph(inline_markup(stripped[2:]), styles["ManualTitle"]))
                story.append(Paragraph("Native STEP, IGES, and STL conversion to GLB/GLTF with hardware 3D preview", styles["ManualSubtitle"]))
                first_title = False
            else:
                story.append(PageBreak())
                story.append(Paragraph(inline_markup(stripped[2:]), styles["ManualH1"]))
        elif stripped.startswith("## "):
            flush_paragraph()
            story.append(Paragraph(inline_markup(stripped[3:]), styles["ManualH1"]))
        elif stripped.startswith("### "):
            flush_paragraph()
            story.append(Paragraph(inline_markup(stripped[4:]), styles["ManualH2"]))
        elif re.match(r"^[-*] ", stripped):
            flush_paragraph()
            story.append(Paragraph(inline_markup(stripped[2:]), styles["ManualBullet"], bulletText="•"))
        elif re.match(r"^\d+\. ", stripped):
            flush_paragraph()
            number, text = stripped.split(". ", 1)
            story.append(Paragraph(inline_markup(text), styles["ManualBullet"], bulletText=f"{number}."))
        elif stripped == "---":
            flush_paragraph()
            story.append(Spacer(1, 6))
        elif not stripped:
            flush_paragraph()
        else:
            paragraph.append(stripped)
        i += 1
    flush_paragraph()
    return story


def main():
    document = SimpleDocTemplate(
        str(OUTPUT), pagesize=A4,
        rightMargin=18 * mm, leftMargin=18 * mm,
        topMargin=17 * mm, bottomMargin=18 * mm,
        title="CAD Converter 2 V0.31 User Manual",
        author="Chan Lap Chi, R Innovation",
        subject="User guide for CAD Converter 2 V0.31",
    )
    story = build_story(SOURCE.read_text(encoding="utf-8"))
    document.build(story, onFirstPage=page_footer, onLaterPages=page_footer)
    print(OUTPUT)


if __name__ == "__main__":
    main()
