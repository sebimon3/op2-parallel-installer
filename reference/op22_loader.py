#!/usr/bin/env python3
"""
Custom parallel loader for the OP-2.2 Inno Setup payload.

Reads the original installer's metadata (produced by inno_header.py) and the
original, unmodified disk-span .bin files, presents a component-selection
wizard, then decodes the LZMA1 chunks in parallel and verifies every file.

Nothing in this file writes to the source archives.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import tkinter as tk
from tkinter import ttk, filedialog, messagebox

HERE = os.path.dirname(os.path.abspath(__file__))
FROZEN = getattr(sys, "frozen", False)


def app_dir() -> str:
    """Directory containing the .bin payloads (exe folder, or its parent)."""
    if not FROZEN:
        return HERE
    d = os.path.dirname(os.path.abspath(sys.executable))
    has_bins = lambda p: (os.path.isdir(p) and
                          any(f.lower().endswith(".bin") for f in os.listdir(p)))
    if not has_bins(d):
        parent = os.path.dirname(d)
        if has_bins(parent):
            return parent
    return d


def resource_path(name: str) -> str:
    """Path to a data file bundled inside a PyInstaller one-file build."""
    base = getattr(sys, "_MEIPASS", HERE)
    return os.path.join(base, name)


# ---------------------------------------------------------------------------
# Metadata helpers
# ---------------------------------------------------------------------------

def load_metadata(path: str) -> dict:
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


_MSG_CACHE: dict[str, str] = {}
_COMPONENT_SIZES: dict[str, int] = {}


def build_message_map(meta: dict) -> None:
    _MSG_CACHE.clear()
    for m in meta.get("messages", []):
        name, value = m.get("name"), m.get("value")
        if name and value:
            _MSG_CACHE.setdefault(name, value)
    # {code:AppName} is a compiled script constant; the installer's own
    # MainTitle message holds the same display string.
    if "AppName" not in _MSG_CACHE and "MainTitle" in _MSG_CACHE:
        _MSG_CACHE["AppName"] = _MSG_CACHE["MainTitle"]


def compute_component_sizes(meta: dict) -> dict[str, int]:
    sizes: dict[str, int] = {}
    entries = meta.get("data_entries", [])
    for f in meta.get("files", []):
        comp = (f.get("components") or "").strip().lower()
        if not comp or comp.startswith("{"):
            continue
        loc = f.get("location")
        if not isinstance(loc, int) or loc >= len(entries):
            continue
        n = int(entries[loc].get("uncompressed_size") or 0)
        sizes[comp] = sizes.get(comp, 0) + n
    # Roll child sizes into parents for the tree display.
    for name in list(sizes):
        parts = name.split("\\")
        for i in range(1, len(parts)):
            parent = "\\".join(parts[:i])
            sizes[parent] = sizes.get(parent, 0) + sizes[name]
    return sizes


def resolve(text: str) -> str:
    if not isinstance(text, str):
        return str(text or "")
    for prefix in ("{cm:", "{code:"):
        while prefix in text:
            start = text.index(prefix)
            end = text.find("}", start)
            if end < 0:
                break
            key = text[start + len(prefix):end]
            text = text[:start] + _MSG_CACHE.get(key, key) + text[end + 1:]
    return text


def component_label(c: dict) -> str:
    desc = resolve(c.get("description") or c.get("name") or "(unnamed)")
    return desc


def component_is_radio(c: dict) -> bool:
    return "Exclusive" in (c.get("flags") or [])


def component_is_fixed(c: dict) -> bool:
    return "Fixed" in (c.get("flags") or [])


def component_size(c: dict) -> int:
    for key in ("used_size", "size", "extra_size"):
        v = c.get(key)
        if isinstance(v, int) and v:
            return v
    return int(_COMPONENT_SIZES.get(str(c.get("name") or "").lower(), 0))


def human_bytes(n: int) -> str:
    try:
        n = int(n)
    except (TypeError, ValueError):
        return "?"
    for unit in ("B", "KiB", "MiB", "GiB", "TiB"):
        if abs(n) < 1024:
            return f"{n:.1f} {unit}" if unit != "B" else f"{n} B"
        n /= 1024.0
    return f"{n:.1f} PiB"


def free_space(path: str) -> int:
    try:
        import shutil
        return shutil.disk_usage(path).free
    except Exception:
        return 0


# ---------------------------------------------------------------------------
# Wizard
# ---------------------------------------------------------------------------

class CheckTree(ttk.Frame):
    """A Treeview with a tri-state checkbox column and hierarchical rows."""

    def __init__(self, master, components: list[dict], **kw):
        super().__init__(master, **kw)
        self.items: dict[str, str] = {}
        self.selected: set[str] = set()

        self.tree = ttk.Treeview(self, columns=("check", "size"), displaycolumns=("check", "size"),
                                 selectmode="none", show="tree headings", height=16)
        self.tree.heading("#0", text="Component")
        self.tree.heading("check", text="Install")
        self.tree.heading("size", text="Size")
        self.tree.column("#0", width=360, stretch=True)
        self.tree.column("check", width=64, stretch=False, anchor="center")
        self.tree.column("size", width=100, stretch=False, anchor="e")

        vsb = ttk.Scrollbar(self, orient="vertical", command=self.tree.yview)
        self.tree.configure(yscrollcommand=vsb.set)
        self.tree.pack(side="left", fill="both", expand=True)
        vsb.pack(side="right", fill="y")

        self.tree.tag_configure("on", image="")
        self._build(components)
        self.tree.bind("<Button-1>", self._on_click)
        self.tree.bind("<space>", self._on_space)

    def _build(self, components: list[dict]) -> None:
        # Inno Setup encodes hierarchy with backslash-separated names.
        by_name = {str(c.get("name") or ""): c for c in components}
        children: dict[str, list[dict]] = {}
        roots: list[dict] = []
        for c in components:
            name = str(c.get("name") or "")
            parent = name.rsplit("\\", 1)[0] if "\\" in name else ""
            if parent and parent in by_name:
                children.setdefault(parent, []).append(c)
            else:
                roots.append(c)

        def add(c: dict, parent: str = "", radio_group: list[str] | None = None) -> None:
            name = str(c.get("name") or component_label(c))
            iid = self.tree.insert(parent, "end", text=component_label(c),
                                   values=("☑" if name in self.selected else "☐",
                                           human_bytes(component_size(c))))
            self.items[iid] = name
            if not (c.get("fixed_off") or str(c.get("flags", "")).lower() == "off"):
                self.selected.add(iid)
                self.tree.set(iid, "check", "☑")
            self.tree.item(iid, open=True)
            if radio_group is not None:
                radio_group.append(iid)
            kids = children.get(name, [])
            radio = [k for k in kids if component_is_radio(k)]
            normal = [k for k in kids if not component_is_radio(k)]
            group: list[str] = []
            for k in normal:
                add(k, iid)
            for k in radio:
                add(k, iid, group)
            if len(group) > 1:
                self.radio_groups.append(group)
                # keep only the first selected in an exclusive group
                chosen = [g for g in group if g in self.selected]
                for g in chosen[1:]:
                    self.selected.discard(g)
                    self.tree.set(g, "check", "☐")

        self.radio_groups: list[list[str]] = []
        for c in roots:
            add(c)

    def _toggle(self, iid: str) -> None:
        if iid in self.selected:
            self.selected.discard(iid)
            self.tree.set(iid, "check", "☐")
        else:
            self.selected.add(iid)
            self.tree.set(iid, "check", "☑")
            for group in self.radio_groups:
                if iid in group:
                    for other in group:
                        if other != iid:
                            self.selected.discard(other)
                            self.tree.set(other, "check", "☐")

    def _on_click(self, event) -> None:
        row = self.tree.identify_row(event.y)
        col = self.tree.identify_column(event.x)
        if row and col in ("#1", "#2"):
            self._toggle(row)

    def _on_space(self, event) -> None:
        for row in self.tree.selection():
            self._toggle(row)

    def selection(self) -> list[str]:
        return sorted({self.items[i] for i in self.selected})


class Wizard(tk.Tk):
    def __init__(self, meta: dict, source_dir: str, metadata_path: str):
        super().__init__()
        self.meta = meta
        self.source_dir = source_dir
        self.metadata_path = metadata_path
        build_message_map(meta)
        _COMPONENT_SIZES.clear()
        _COMPONENT_SIZES.update(compute_component_sizes(meta))
        self.comp_page = None
        self.check_tree = None
        self.dest = tk.StringVar(value=os.path.join(os.environ.get("ProgramFiles(x86)", "C:\\Program Files (x86)"), "OP-2.2"))
        self.title(resolve(self.meta.get("header", {}).get("app_name", "OP-2.2")) + " Setup")
        self.geometry("760x560")
        self.resizable(True, True)
        self._frame = ttk.Frame(self)
        self._frame.pack(fill="both", expand=True, padx=12, pady=12)
        self.show_components() if getattr(self, "_start_on_components", False) else self.show_destination()

    def _clear(self):
        for w in list(self._frame.winfo_children()):
            w.destroy()

    def show_destination(self):
        self._clear()
        ttk.Label(self._frame, text="Select Destination Location", font=("Segoe UI", 14, "bold")).pack(anchor="w", pady=(6, 4))
        ttk.Label(self._frame, text="Where should OP-2.2 be installed?").pack(anchor="w")
        row = ttk.Frame(self._frame)
        row.pack(fill="x", pady=10)
        ttk.Entry(row, textvariable=self.dest, width=60).pack(side="left", fill="x", expand=True)
        ttk.Button(row, text="Browse...", command=self._browse).pack(side="left", padx=6)
        required = sum(int(e.get("uncompressed_size") or 0)
                       for e in self.meta.get("data_entries", []))
        ttk.Label(self._frame, text=f"At least {human_bytes(required)} of free disk space is required.").pack(anchor="w", pady=4)
        f = free_space(os.path.dirname(self.dest.get()) or ".")
        ttk.Label(self._frame, text=f"Free on target volume: {human_bytes(f)}").pack(anchor="w")
        btns = ttk.Frame(self._frame)
        btns.pack(fill="x", side="bottom", pady=8)
        ttk.Button(btns, text="Cancel", command=self.destroy).pack(side="right", padx=6)
        ttk.Button(btns, text="Next", command=self.show_components).pack(side="right")

    def _browse(self):
        p = filedialog.askdirectory(initialdir=self.dest.get() or "C:\\")
        if p:
            self.dest.set(p)

    def show_components(self):
        self._clear()
        ttk.Label(self._frame, text="Select Components", font=("Segoe UI", 14, "bold")).pack(anchor="w", pady=(6, 4))
        ttk.Label(self._frame, text="Select the components you want to install.").pack(anchor="w", pady=(0, 8))
        self.check_tree = CheckTree(self._frame, self.meta.get("components", []))
        self.check_tree.pack(fill="both", expand=True)
        btns = ttk.Frame(self._frame)
        btns.pack(fill="x", side="bottom", pady=8)
        ttk.Button(btns, text="Cancel", command=self.destroy).pack(side="right", padx=6)
        ttk.Button(btns, text="Install", command=self._start).pack(side="right")

    def _start(self):
        selection = self.check_tree.selection()
        self.destroy()
        main_extract(self.metadata_path, self.source_dir, self.dest.get(), selection)


# ---------------------------------------------------------------------------
# Extraction driver
# ---------------------------------------------------------------------------

def main_extract(metadata_path: str, source_dir: str, output_dir: str,
                 selection: list[str] | None = None,
                 extra: list[str] | None = None) -> int:
    """Run the extractor in-process (works both as a script and frozen)."""
    import parallel_extract

    args = ["--metadata", metadata_path,
            "--source-dir", source_dir,
            "--output-dir", output_dir,
            "--workers", str(max(1, (os.cpu_count() or 4)))]
    if selection:
        # parallel_extract.py expects a comma-separated list of component names.
        args += ["--components", ",".join(selection)]
    if extra:
        args += extra
    rc = parallel_extract.main(args)
    try:
        if rc != 0:
            messagebox.showerror("Installation failed", f"Extractor exited with code {rc}")
        elif not (extra and ("--verify-only" in extra or "--list-only" in extra)):
            messagebox.showinfo("Installation complete",
                                f"Files were installed to:\n{output_dir}")
    except tk.TclError:
        pass  # no display (headless test run)
    return rc


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Custom OP-2.2 parallel loader")
    ap.add_argument("--metadata", default=resource_path("op22_meta.json"))
    ap.add_argument("--source-dir", default=app_dir())
    ap.add_argument("--output-dir", default=None)
    ap.add_argument("--no-wizard", action="store_true")
    ap.add_argument("--show-components", action="store_true")
    ap.add_argument("--verify-only", action="store_true")
    ap.add_argument("--list-only", action="store_true")
    ap.add_argument("--components", default=None)
    args = ap.parse_args(argv)
    meta = load_metadata(args.metadata)
    extra: list[str] = []
    if args.verify_only:
        extra.append("--verify-only")
    if args.list_only:
        extra.append("--list-only")
    if args.components:
        extra += ["--components", args.components]
    if args.no_wizard:
        return main_extract(args.metadata, args.source_dir,
                            args.output_dir or os.path.join(app_dir(), "_out"),
                            None, extra)
    else:
        w = Wizard(meta, args.source_dir, args.metadata)
        w._start_on_components = args.show_components
        if args.show_components:
            w.show_components()
        w.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
