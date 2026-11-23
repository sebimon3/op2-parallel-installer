#!/usr/bin/env python3
"""Generate src/manifest_data.h from the installer metadata JSON.

Run from anywhere:  python src/gen_manifest.py [path/to/op22_meta.json]
Output is always written next to this script, and the two wizard BMPs are
written to src/assets/.
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, os.pardir))
META = (sys.argv[1] if len(sys.argv) > 1
        else os.path.join(ROOT, "reference", "op22_meta.json"))
OUT = os.path.join(HERE, "manifest_data.h")
ASSETS = os.path.join(HERE, "assets")

# The wizard artwork is stored as real .bmp files in assets/ and embedded by
# resource.rc as custom resources 200/201.  The generated header therefore
# contains no binary blobs at all.
RES_WIZARD = 210
RES_WIZARD_SMALL = 211


def cstr(text):
    """Readable UTF-8 C string literal (source is compiled with /utf-8)."""
    out = ['"']
    for ch in text:
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\r":
            out.append("\\r")
        elif ch == "\t":
            out.append("\\t")
        elif ord(ch) < 32:
            out.append("\\x%02X" % ord(ch))
        else:
            out.append(ch)
    out.append('"')
    return "".join(out)


def cbytes(data, indent="    ", per_line=20):
    lines = []
    for i in range(0, len(data), per_line):
        chunk = data[i:i + per_line]
        lines.append(indent + ", ".join("0x%02X" % b for b in chunk))
    return ",\n".join(lines)


def main():
    with open(META, "r", encoding="utf-8") as f:
        m = json.load(f)

    header = m["header"]
    datas = m["data_entries"]

    # English (language 0) message table; {cm:X} and {code:X} both resolve here.
    msgs = {}
    for msg in m["messages"]:
        if msg.get("language") == 0:
            msgs[msg["name"]] = msg["value"]

    # Inno Setup's {code:AppName} is provided by the compiled_code function
    # AppName, which in this installer returns the MainTitle custom message.
    CODE_ALIASES = {"AppName": "MainTitle", "DefaultGroupName": "MainTitle"}

    def resolve(expr):
        if not isinstance(expr, str):
            return ""
        if expr.startswith("{cm:") or expr.startswith("{code:"):
            name = expr[expr.index(":") + 1:expr.rindex("}")]
            if expr.startswith("{code:"):
                name = CODE_ALIASES.get(name, name)
            if name in msgs:
                return msgs[name]
        return expr

    app_name = resolve(header["app_name"])
    app_version = header.get("app_version", "")
    publisher = header.get("app_publisher", "")
    copyright = header.get("app_copyright", "")

    default_dir = header.get("default_dir_name", "")
    if "\\" in default_dir:
        default_dir = default_dir.rsplit("\\", 1)[-1]
    elif "/" in default_dir:
        default_dir = default_dir.rsplit("/", 1)[-1]

    slices = max(e["last_slice"] for e in datas) + 1
    slices_per_disk = int(m["slices_per_disk"])

    # ---- components -------------------------------------------------------
    comps = []
    for c in m["components"]:
        name = c["name"]
        parent = name.rsplit("\\", 1)[0] if "\\" in name else ""
        flags = (1 if "Fixed" in c.get("flags", []) else 0) | \
                (2 if "Exclusive" in c.get("flags", []) else 0)
        comps.append({"name": name, "desc": resolve(c["description"]),
                      "flags": flags, "parent": parent})

    # ---- files ------------------------------------------------------------
    files = []
    for f in m["files"]:
        loc = f.get("location")
        if not isinstance(loc, int) or loc < 0 or loc >= len(datas):
            continue  # sentinel entry (empty destination, no data chunk)
        dest = f.get("destination", "")
        is_tmp = 0
        if dest.startswith("{app}\\"):
            dest = dest[len("{app}\\"):]
        elif dest.startswith("{tmp}\\"):
            dest = dest[len("{tmp}\\"):]
            is_tmp = 1
        elif dest.startswith("{app}/"):
            dest = dest[len("{app}/"):]
        elif dest.startswith("{tmp}/"):
            dest = dest[len("{tmp}/"):]
            is_tmp = 1
        files.append({"dest": dest, "location": loc,
                      "components": f.get("components", ""),
                      "tasks": f.get("tasks", ""),
                      "size": int(datas[loc]["uncompressed_size"]),
                      "is_tmp": is_tmp})

    # Component sizes: files belonging exactly to a component plus all of its
    # descendants, rolled up.
    for c in comps:
        prefix = c["name"] + "\\"
        total = 0
        for f in files:
            fc = f["components"]
            if fc == c["name"] or fc.startswith(prefix):
                total += f["size"]
        c["size"] = total

    # ---- data entries -----------------------------------------------------
    entries = []
    for e in datas:
        sha = bytes.fromhex(e["checksum"])
        if len(sha) != 20:
            raise SystemExit("bad checksum at data entry %d" % len(entries))
        entries.append({
            "first_slice": int(e["first_slice"]),
            "offset": int(e["offset"]),
            "compressed_size": int(e["size"]),
            "uncompressed_size": int(e["uncompressed_size"]),
            "sha1": sha,
            "call_optimize": 1 if "CallInstructionOptimized" in e.get("flags", []) else 0,
        })

    # ---- wizard bitmaps ---------------------------------------------------
    def bmp(key):
        raw = m[key][0].encode("utf-16le")
        if not raw.startswith(b"BM"):
            raise SystemExit("%s does not start with BM" % key)
        return raw

    wizard = bmp("wizard_images")
    wizard_small = bmp("wizard_images_small")

    # Write real BMP files so the artwork can be viewed/edited directly.
    os.makedirs(ASSETS, exist_ok=True)
    for name, blob in (("wizard.bmp", wizard), ("wizard_small.bmp", wizard_small)):
        with open(os.path.join(ASSETS, name), "wb") as f:
            f.write(blob)

    # ---- strings ----------------------------------------------------------
    S = {
        "s_title": "Cumulative Pack 2.2 Setup",
        "s_lang_title": "Select Language",
        "s_lang_label": "Select the language to be used for the installation.",
        "s_dest_title": "Select Destination Location",
        "s_dest_label": "Where should Cumulative Pack 2.2 be installed?",
        "s_space_required": "Space required: %1",
        "s_comp_title": "Select Components",
        "s_comp_label": "Select the components you want to install.",
        "s_install_col": "Install",
        "s_size_col": "Size",
        "s_tasks_title": "Select Tasks",
        "s_tasks_label": "Select tasks to be performed:",
        "s_desktop_task": "Create a desktop shortcut",
        "s_common": "For all users",
        "s_current": "For the current user",
        "s_ready_title": "Ready to Install",
        "s_ready_label": "Setup is now ready to begin installing Cumulative Pack 2.2 on your computer.",
        "s_installing": "Installing",
        "s_finish_title": "Installation Complete",
        "s_finish_label": "Setup has finished installing Cumulative Pack 2.2 on your computer.",
        "s_next": "&Next >",
        "s_back": "< &Back",
        "s_cancel": "Cancel",
        "s_install": "&Install",
        "s_browse": "Browse...",
        "s_finish_btn": "&Finish",
    }

    # ---- emit -------------------------------------------------------------
    w = []
    a = w.append
    a("/* Generated by gen_manifest.py from op22_meta.json -- do not edit by hand.")
    a(" * All strings are plain UTF-8.  The wizard artwork is NOT embedded here:")
    a(" * it lives in assets/wizard.bmp and assets/wizard_small.bmp and is")
    a(" * embedded as RCDATA resources %d/%d by resource.rc, then loaded at" %
      (RES_WIZARD, RES_WIZARD_SMALL))
    a(" * startup by ui.c.  This keeps this file human-readable. */")
    a("#ifndef OP22_MANIFEST_DATA_H")
    a("#define OP22_MANIFEST_DATA_H")
    a('#include "app.h"')
    a("")
    a("static const op22_comp g_comps[] = {")
    for c in comps:
        a("    { %s, %s, %u, %uULL, %s }," % (
            cstr(c["name"]), cstr(c["desc"]), c["flags"], c["size"],
            cstr(c["parent"])))
    a("};")
    a("static const op22_file g_files[] = {")
    for f in files:
        a("    { %s, %d, %s, %s, %uULL, %d }," % (
            cstr(f["dest"]), f["location"], cstr(f["components"]),
            cstr(f["tasks"]), f["size"], f["is_tmp"]))
    a("};")
    a("static const op22_data g_data[] = {")
    for e in entries:
        sha = ", ".join("0x%02X" % b for b in e["sha1"])
        a("    { %d, %uULL, %uULL, %uULL, { %s }, %d }," % (
            e["first_slice"], e["offset"], e["compressed_size"],
            e["uncompressed_size"], sha, e["call_optimize"]))
    a("};")
    a("")
    a("op22_manifest g_manifest = {")
    a("    %s, %s, %s, %s, %s," % (
        cstr(app_name), cstr(app_version), cstr(publisher), cstr(copyright),
        cstr(default_dir)))
    a("    %d, %d," % (slices, slices_per_disk))
    a("    g_comps, %d, g_files, %d, g_data, %d," % (
        len(comps), len(files), len(entries)))
    a("    NULL, 0,  /* wizard.bmp -- loaded from RCDATA %d in ui.c */" % RES_WIZARD)
    a("    NULL, 0,  /* wizard_small.bmp -- loaded from RCDATA %d in ui.c */" % RES_WIZARD_SMALL)
    for key in ["s_title", "s_lang_title", "s_lang_label", "s_dest_title",
                "s_dest_label", "s_space_required", "s_comp_title",
                "s_comp_label", "s_install_col", "s_size_col", "s_tasks_title",
                "s_tasks_label", "s_desktop_task", "s_common", "s_current",
                "s_ready_title", "s_ready_label", "s_installing",
                "s_finish_title", "s_finish_label", "s_next", "s_back",
                "s_cancel", "s_install", "s_browse", "s_finish_btn"]:
        a("    %s," % cstr(S[key]))
    a("};")
    a("")
    a("#endif /* OP22_MANIFEST_DATA_H */")
    a("")

    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(w))

    print("wrote %s" % OUT)
    print("  app=%r version=%r dir=%r" % (app_name, app_version, default_dir))
    print("  comps=%d files=%d data=%d slices=%d spd=%d" % (
        len(comps), len(files), len(entries), slices, slices_per_disk))
    print("  bmp=%d bytes, small bmp=%d bytes" % (len(wizard), len(wizard_small)))


if __name__ == "__main__":
    sys.exit(main())
