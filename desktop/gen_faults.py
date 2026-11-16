#!/usr/bin/env python3
# Generates the pump fault tables for the desktop monitor and the proxy's
# display page from the firmware's faults.cpp, which is the one source.
#
#   python3 desktop/gen_faults.py           write both files
#   python3 desktop/gen_faults.py --check   exit 1 if either is out of date
#
# Run it after every change to faults.cpp and commit the results with it.
#
# Copyright 2026, Frederik. GPL-3.0; see LICENSE.md in the root.

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "firmware" / "minimed-coreink" / "faults.cpp"
PY_OUT = ROOT / "desktop" / "faults.py"
JS_OUT = ROOT / "proxy" / "display" / "faults.js"

HEADER = ("Generated from firmware/minimed-coreink/faults.cpp by "
          "desktop/gen_faults.py. Do not edit: change faults.cpp and rerun.")


def block(src, name):
    """The text between `NAME[] = {` and the matching `};`."""
    m = re.search(r"\b%s\[\]\s*=\s*\{(.*?)\};" % re.escape(name), src, re.S)
    if not m:
        sys.exit("gen_faults: %s not found in %s" % (name, SOURCE))
    return m.group(1)


def pairs(text):
    return re.findall(r'\{\s*"([^"]*)"\s*,\s*"([^"]*)"\s*\}', text)


def ids(text):
    return re.findall(r'"([^"]*)"', text)


def load():
    src = SOURCE.read_text(encoding="utf-8")
    tables = {
        "FAULT_MAP": dict(pairs(block(src, "FAULT_MAP"))),
        "FAULT_TEXT": dict(pairs(block(src, "FAULT_TEXT"))),
        "LOW_GLUCOSE_IDS": ids(block(src, "LOW_GLUCOSE_IDS")),
        "HIGH_GLUCOSE_IDS": ids(block(src, "HIGH_GLUCOSE_IDS")),
    }
    # A parse that silently came back short would drop alarm texts.
    if len(tables["FAULT_MAP"]) < 100 or len(tables["FAULT_TEXT"]) < 50:
        sys.exit("gen_faults: suspiciously few entries parsed, refusing to write")
    return tables


def render_py(t):
    out = ["# " + HEADER, ""]
    for name, value in t.items():
        out.append("%s = %s" % (name, json.dumps(value, indent=1, sort_keys=isinstance(value, dict))))
        out.append("")
    return "\n".join(out)


def render_js(t):
    out = ["// " + HEADER, '"use strict";', ""]
    for name, value in t.items():
        out.append("const %s = %s;" % (name, json.dumps(value, indent=1, sort_keys=isinstance(value, dict))))
        out.append("")
    return "\n".join(out)


def main():
    t = load()
    wanted = {PY_OUT: render_py(t), JS_OUT: render_js(t)}
    if "--check" in sys.argv[1:]:
        stale = [p for p, text in wanted.items()
                 if not p.exists() or p.read_text(encoding="utf-8") != text]
        for p in stale:
            print("out of date: %s" % p.relative_to(ROOT))
        sys.exit(1 if stale else 0)
    for p, text in wanted.items():
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text, encoding="utf-8")
        print("wrote %s" % p.relative_to(ROOT))


if __name__ == "__main__":
    main()
