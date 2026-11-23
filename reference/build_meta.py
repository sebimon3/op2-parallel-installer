#!/usr/bin/env python3
"""Regenerate op22_meta.json from the original installer, encoding-safe.

Usage: python reference/build_meta.py [path/to/op-2.2.exe]

The original installer is not included in this repository; supply its path.
Do not generate the JSON with shell redirection: PowerShell's pipeline
re-decodes Python's UTF-8 output as the OEM code page, which corrupts the
Russian message strings.
"""

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import inno_header

HERE = os.path.dirname(os.path.abspath(__file__))


def scrub(obj):
    if isinstance(obj, bytes):
        return obj.hex()
    if isinstance(obj, list):
        return [scrub(x) for x in obj]
    if isinstance(obj, dict):
        return {k: scrub(v) for k, v in obj.items()}
    return obj


def main() -> int:
    exe = (sys.argv[1] if len(sys.argv) > 1
           else os.path.join(HERE, "op-2.2.exe"))
    data = scrub(inno_header.parse_installer(exe))
    out = os.path.join(HERE, "op22_meta.json")
    with open(out, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=1)
    print("wrote %s" % out)
    print("integrity:", data["integrity"])
    print("counts: %d components, %d files, %d data entries"
          % (len(data["components"]), len(data["files"]), len(data["data_entries"])))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
