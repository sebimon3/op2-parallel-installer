#!/usr/bin/env python3
"""Verify every installed file against the original installer's SHA-1 list.

Usage: python tools/verify_install.py <install-dir> [path/to/op22_meta.json]
"""
import hashlib
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, os.pardir))

if len(sys.argv) < 2:
    raise SystemExit(__doc__)
install_dir = sys.argv[1]
meta_path = (sys.argv[2] if len(sys.argv) > 2
             else os.path.join(ROOT, "reference", "op22_meta.json"))
meta = json.load(open(meta_path, encoding="utf-8"))


def resolve(dest):
    for prefix in ("{app}", "{tmp}"):
        if dest.startswith(prefix):
            return dest[len(prefix):].lstrip("\\/")
    return dest


ok = bad = skip = 0
for f in meta["files"]:
    loc = f["location"]
    if loc >= len(meta["data_entries"]):
        skip += 1
        continue
    rel = resolve(f.get("destination", ""))
    # {tmp} files are deleted by Inno Setup after installation.
    if not rel or (f.get("destination") or "").lower().startswith("{tmp}"):
        skip += 1
        continue
    path = os.path.join(install_dir, rel)
    if not os.path.exists(path):
        print("MISSING", rel)
        bad += 1
        continue
    h = hashlib.sha1()
    with open(path, "rb") as fh:
        for block in iter(lambda: fh.read(8 << 20), b""):
            h.update(block)
    expected = meta["data_entries"][loc]["checksum"]
    if h.hexdigest() == expected:
        ok += 1
    else:
        print("MISMATCH", rel, h.hexdigest(), expected)
        bad += 1

total = sum(os.path.getsize(os.path.join(r, n))
            for r, _, fs in os.walk(install_dir) for n in fs)
print("files verified: %d OK, %d bad, %d skipped" % (ok, bad, skip))
print("total bytes: %d" % total)
raise SystemExit(1 if bad else 0)
