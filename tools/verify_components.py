#!/usr/bin/env python3
"""Verify that component selection installs exactly the stock file set.

Runs the native installer with different --components selections into fresh
directories and compares both the file set and every SHA-1 against the
component mappings in op22_meta.json.

The original 10 .bin payload files are required but never modified.

Usage:
  python tools/verify_components.py --source-dir <dir-with-bin-files>
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, os.pardir))


def rel_path(dest):
    for prefix in ("{app}\\", "{tmp}\\"):
        if dest.lower().startswith(prefix.lower()):
            return dest[len(prefix):].lstrip("\\/")
    return dest


def expected_files(meta, selected):
    out = {}
    for f in meta["files"]:
        dest = f.get("destination") or ""
        loc = f.get("location")
        if (not dest or dest.lower().startswith("{tmp}")
                or not isinstance(loc, int) or loc >= len(meta["data_entries"])):
            continue
        expr = (f.get("components") or "").strip()
        if expr and expr not in selected:
            continue
        out[rel_path(dest)] = meta["data_entries"][loc]["checksum"]
    return out


def sha1(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(8 << 20), b""):
            h.update(b)
    return h.hexdigest()


def run_scenario(args, meta, label, names, expect_names):
    out = os.path.join(args.temp_root, "_scenario_" + label)
    if os.path.exists(out):
        shutil.rmtree(out)
    cmd = [args.exe, "--no-wizard", "--output-dir", out,
           "--source-dir", args.source_dir,
           "--components", ",".join(names)]
    t = time.perf_counter()
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=1800)
    dt = time.perf_counter() - t
    print("\n=== %s (%s)" % (label, ",".join(names)))
    if p.returncode != 0:
        print("INSTALL FAILED rc=%d\n%s" % (p.returncode, p.stdout[-2000:]))
        return False
    exp = expected_files(meta, expect_names)
    actual = {}
    for base, _, files in os.walk(out):
        for n in files:
            actual[os.path.relpath(os.path.join(base, n), out)] = None
    missing = sorted(set(exp) - set(actual))
    extra = sorted(set(actual) - set(exp))
    bad = [r for r in sorted(set(exp) & set(actual))
           if sha1(os.path.join(out, r)) != exp[r]]
    ok = not (missing or extra or bad)
    print("elapsed %.1fs | expected %d, got %d | missing %d, extra %d, bad %d"
          % (dt, len(exp), len(actual), len(missing), len(extra), len(bad)))
    if missing: print("  MISSING:", missing[:10])
    if extra: print("  EXTRA:", extra[:10])
    if bad: print("  BAD SHA1:", bad[:10])
    shutil.rmtree(out, ignore_errors=True)
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--source-dir", required=True,
                    help="directory containing op-2.2-1a.bin ... 2d.bin")
    ap.add_argument("--exe", default=os.path.join(ROOT, "bin", "op-2.2-mt.exe"))
    ap.add_argument("--meta", default=os.path.join(ROOT, "reference", "op22_meta.json"))
    ap.add_argument("--temp-root", default=os.getcwd())
    args = ap.parse_args()

    meta = json.load(open(args.meta, encoding="utf-8"))
    cases = [
        ("main_only", ["main"], {"main"}),
        ("look2", ["main", "addons", "addons\\arms", "addons\\arms\\arm2"],
         {"main", "addons", "addons\\arms", "addons\\arms\\arm2"}),
        ("sounds", ["main", "addons", "addons\\sounds",
                    "addons\\sounds\\streamers", "addons\\sounds\\streamers\\more_strict",
                    "addons\\sounds\\cont", "addons\\sounds\\cont\\cont3"],
         {"main", "addons", "addons\\sounds",
          "addons\\sounds\\streamers", "addons\\sounds\\streamers\\more_strict",
          "addons\\sounds\\cont", "addons\\sounds\\cont\\cont3"}),
        ("exclusive_conflict", ["main", "addons", "addons\\arms",
                                "addons\\arms\\arm1", "addons\\arms\\arm2"],
         {"main", "addons", "addons\\arms", "addons\\arms\\arm2"}),
    ]
    results = [run_scenario(args, meta, *c) for c in cases]
    print("\nRESULT: %d/%d scenarios passed" % (sum(results), len(results)))
    return 0 if all(results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
