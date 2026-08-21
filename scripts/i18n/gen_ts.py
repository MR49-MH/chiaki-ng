#!/usr/bin/env python3
"""Extract qsTr() strings from QML files into a Qt Linguist .ts skeleton.

Usage:
    python gen_ts.py                 # writes gui/lang/chiaki_zh_CN.ts + manifest.json

The .ts skeleton contains every qsTr() literal found in gui/src/qml/*.qml,
grouped by QML document context (= file basename), translations left empty.
manifest.json carries the same strings with stable ids for offline translation.
"""
import html
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
QML_DIR = ROOT / "gui" / "src" / "qml"
OUT_TS = ROOT / "gui" / "lang" / "chiaki_zh_CN.ts"
OUT_MANIFEST = Path(__file__).parent / "manifest.json"

# qsTr("...") or qsTr('...'), allowing whitespace/newlines inside the call
QSTR_RE = re.compile(
    r"""qsTr\s*\(\s*(?:"((?:\\.|[^"\\])*)"|'((?:\\.|[^'\\])*)')\s*\)""",
    re.DOTALL,
)

ESCAPES = {
    "\\\\": "\\",
    '\\"': '"',
    "\\'": "'",
    "\\n": "\n",
    "\\t": "\t",
}


def unescape(lit: str) -> str:
    out, i = [], 0
    while i < len(lit):
        ch = lit[i]
        if ch == "\\" and i + 1 < len(lit):
            two = lit[i : i + 2]
            if two in ESCAPES:
                out.append(ESCAPES[two])
                i += 2
                continue
            # unknown escape: keep verbatim (Qt would warn at runtime anyway)
        out.append(ch)
        i += 1
    return "".join(out)


def extract():
    """Return {context: [source strings]} preserving first-seen order."""
    contexts = {}
    for qml in sorted(QML_DIR.glob("*.qml")):
        text = qml.read_text(encoding="utf-8")
        found = []
        seen = set()
        for m in QSTR_RE.finditer(text):
            lit = m.group(1) if m.group(1) is not None else m.group(2)
            src = unescape(lit)
            if not src.strip():
                continue  # qsTr("") is pointless to translate
            if src in seen:
                continue
            seen.add(src)
            found.append(src)
        if found:
            contexts[qml.stem] = found
    return contexts


def xml_escape(s: str) -> str:
    return html.escape(s, quote=False)


def write_ts(contexts):
    lines = [
        '<?xml version="1.0" encoding="utf-8"?>',
        "<!DOCTYPE TS>",
        '<TS version="2.1" language="zh_CN" sourcelanguage="en_US">',
    ]
    total = 0
    for ctx in sorted(contexts):
        lines.append("<context>")
        lines.append(f"    <name>{ctx}</name>")
        for src in contexts[ctx]:
            lines.append("    <message>")
            lines.append(f"        <source>{xml_escape(src)}</source>")
            lines.append("        <translation></translation>")
            lines.append("    </message>")
            total += 1
        lines.append("</context>")
    lines.append("</TS>")
    lines.append("")
    OUT_TS.parent.mkdir(parents=True, exist_ok=True)
    OUT_TS.write_text("\n".join(lines), encoding="utf-8")
    return total


def main():
    contexts = extract()
    total = write_ts(contexts)

    manifest = {}
    for ctx, srcs in sorted(contexts.items()):
        for idx, src in enumerate(srcs):
            manifest[f"{ctx}::{idx}"] = {"ctx": ctx, "src": src}
    OUT_MANIFEST.write_text(
        json.dumps(manifest, ensure_ascii=False, indent=1), encoding="utf-8"
    )

    print(f"{len(contexts)} contexts, {total} strings -> {OUT_TS.name}")
    # sanity: warn about strings that look like concatenations or non-literals
    for qml in sorted(QML_DIR.glob("*.qml")):
        text = qml.read_text(encoding="utf-8")
        for m in re.finditer(r'qsTr\s*\((?!\s*["\'])', text):
            line = text[: m.start()].count("\n") + 1
            print(f"WARN non-literal qsTr: {qml.name}:{line}", file=sys.stderr)


if __name__ == "__main__":
    main()
