#!/usr/bin/env python3
"""Build the import index that peflink.py uses to find shared libraries.

Each Mac OS shared library (InterfaceLib, MathLib, ...) exports a list of
names. Retro68's ImportLibraries/lib*.a archives hold just those names as
XCOFF symbols: ".Name" (text) for a function's code and "Name" (data) for
its descriptor or for exported data. We only read the names with llvm-nm,
so no code is used from them.

    python3 gen-imports.py LLVM_NM IMPORTLIB_DIR imports.json

The output maps each library to {"func": [...], "data": [...], "weak": bool}.
"""
import json
import os
import subprocess
import sys

nm, src, out = sys.argv[1:4]
libs = {}
for fn in sorted(os.listdir(src)):
    if not (fn.startswith("lib") and fn.endswith(".a")):
        continue
    text = subprocess.run([nm, os.path.join(src, fn)], check=True,
                          capture_output=True, encoding="mac_roman").stdout
    member, funcs, data = None, set(), set()
    for line in text.splitlines():
        if line.endswith(".o:"):
            member = line[:-3]
            continue
        parts = line.split()
        if len(parts) != 3:
            continue
        kind, name = parts[1], parts[2]
        if kind == "T" and name.startswith("."):
            funcs.add(name[1:])
        elif kind == "D":
            data.add(name)
    # MakeImport marks a library to import weakly with a "__weak" suffix.
    name = member or fn[3:-2]
    weak = name.endswith("__weak")
    if weak:
        name = name[:-6]
    libs[name] = {"func": sorted(funcs), "data": sorted(data - funcs),
                  "weak": weak}

with open(out, "w", encoding="utf-8") as f:
    json.dump(libs, f, separators=(",", ":"), sort_keys=True)
print(f"{len(libs)} libraries, "
      f"{sum(len(v['func']) + len(v['data']) for v in libs.values())} names")
