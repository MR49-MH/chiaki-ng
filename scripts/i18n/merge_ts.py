#!/usr/bin/env python3
"""Merge trans_N.json files into gui/lang/chiaki_zh_CN.ts.

Validates full coverage against manifest.json and placeholder integrity,
then writes the final .ts for lrelease.
"""
import html
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).parent
ROOT = Path(__file__).resolve().parents[2]
OUT_TS = ROOT / "gui" / "lang" / "chiaki_zh_CN.ts"

manifest = json.loads((HERE / "manifest.json").read_text(encoding="utf-8"))

translations = {}
for i in range(10):
    p = HERE / f"trans_{i}.json"
    if not p.exists():
        continue
    part = json.loads(p.read_text(encoding="utf-8"))
    dup = set(part) & set(translations)
    if dup:
        sys.exit(f"FATAL: duplicate keys across chunks: {sorted(dup)[:5]}")
    translations.update(part)

missing = set(manifest) - set(translations)
extra = set(translations) - set(manifest)
if missing:
    sys.exit(f"FATAL: {len(missing)} untranslated keys, e.g. {sorted(missing)[:5]}")
if extra:
    sys.exit(f"FATAL: {len(extra)} unknown keys, e.g. {sorted(extra)[:5]}")

PH_RE = re.compile(r"%[1-9]")
bad = []
for key, m in manifest.items():
    src, dst = m["src"], translations[key]
    if sorted(PH_RE.findall(src)) != sorted(PH_RE.findall(dst)):
        bad.append((key, "placeholder mismatch", src, dst))
    elif src.count("\n") != dst.count("\n"):
        bad.append((key, "newline mismatch", repr(src), repr(dst)))
if bad:
    for b in bad[:10]:
        print("BAD:", *b, sep="  ", file=sys.stderr)
    sys.exit(f"FATAL: {len(bad)} placeholder/newline mismatches")


def xml_escape(s: str) -> str:
    return html.escape(s, quote=False)


# (ctx, src) -> translation ; manifest is deduped per context by gen_ts.py
tmap = {(v["ctx"], v["src"]): translations[k] for k, v in manifest.items()}

by_ctx = {}
for key, v in manifest.items():
    by_ctx.setdefault(v["ctx"], []).append(v["src"])

lines = [
    '<?xml version="1.0" encoding="utf-8"?>',
    "<!DOCTYPE TS>",
    '<TS version="2.1" language="zh_CN" sourcelanguage="en_US">',
]
count = 0
for ctx in sorted(by_ctx):
    lines.append("<context>")
    lines.append(f"    <name>{ctx}</name>")
    for src in by_ctx[ctx]:
        dst = tmap[(ctx, src)]
        lines.append("    <message>")
        lines.append(f"        <source>{xml_escape(src)}</source>")
        lines.append(f"        <translation>{xml_escape(dst)}</translation>")
        lines.append("    </message>")
        count += 1
    lines.append("</context>")
lines.append("</TS>")

OUT_TS.parent.mkdir(parents=True, exist_ok=True)
OUT_TS.write_text("\n".join(lines) + "\n", encoding="utf-8")
print(f"OK: wrote {count} translations ({len(by_ctx)} contexts) -> {OUT_TS}")
