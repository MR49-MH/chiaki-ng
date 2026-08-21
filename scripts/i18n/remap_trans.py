#!/usr/bin/env python3
"""Re-key old translations (old manifest) onto the regenerated manifest by
matching (context, source). Unmatched = newly added strings."""
import json
from pathlib import Path

HERE = Path(__file__).parent

old_man = json.loads((HERE / "manifest_old.json").read_text(encoding="utf-8"))
trans = {}
for i in range(10):
    p = HERE / "old" / f"trans_{i}.json"
    if p.exists():
        trans.update(json.loads(p.read_text(encoding="utf-8")))

old_map = {(v["ctx"], v["src"]): trans[k] for k, v in old_man.items()}
new_man = json.loads((HERE / "manifest.json").read_text(encoding="utf-8"))

out, missing = {}, []
for k, v in new_man.items():
    t = old_map.get((v["ctx"], v["src"]))
    if t is None:
        missing.append((k, v["ctx"], repr(v["src"])))
    else:
        out[k] = t

(HERE / "trans_0.json").write_text(
    json.dumps(out, ensure_ascii=False, indent=1), encoding="utf-8"
)
print(f"rekeyed {len(out)} translations; {len(missing)} new strings need manual translation:")
for m in missing:
    print("  NEW:", *m)
