#!/usr/bin/env python3
"""Split manifest.json into balanced translation chunks (by context groups)."""
import json
from pathlib import Path

HERE = Path(__file__).parent
manifest = json.loads((HERE / "manifest.json").read_text(encoding="utf-8"))

by_ctx = {}
for key, v in manifest.items():
    by_ctx.setdefault(v["ctx"], []).append((key, v["src"]))

# hand-balanced groups (~60-110 keys each), big files split in halves
GROUPS = [
    ["SettingsDialog:H0"],
    ["SettingsDialog:H1"],
    ["PlaceboSettingsDialog"],
    ["PlaceboColorMappingDialog", "ConsolePinDialog", "ConfirmDialog"],
    ["DisplaySettingsDialog", "StreamView"],
    ["MainView", "StreamMenuWindow", "RegistDialog"],
    [
        "PsnView", "SteamShortcutDialog", "PSNLoginDialog", "PSNTokenDialog",
        "Main", "ManualHostDialog", "ProfileDialog", "RemindDialog",
        "ControllerMappingDialog", "AutoConnectView",
    ],
]

total = 0
for gi, group in enumerate(GROUPS):
    chunk = {}
    for entry in group:
        ctx, half = (entry.split(":") + [None])[:2]
        items = sorted(by_ctx[ctx])
        if half == "H0":
            items = items[: len(items) // 2]
        elif half == "H1":
            items = items[len(items) // 2 :]
        for key, src in items:
            chunk[key] = {"ctx": ctx, "src": src}
    (HERE / f"chunk_{gi}.json").write_text(
        json.dumps(chunk, ensure_ascii=False, indent=1), encoding="utf-8"
    )
    total += len(chunk)
    print(f"chunk_{gi}.json: {len(chunk)} strings")

covered = set()
for gi in range(len(GROUPS)):
    covered |= json.loads((HERE / f"chunk_{gi}.json").read_text(encoding="utf-8")).keys()
assert covered == set(manifest), "chunks do not cover manifest exactly!"
print(f"total {total} == manifest {len(manifest)}, coverage OK")
