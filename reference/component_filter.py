"""Map a component selection to the files that Inno Setup would install."""

from __future__ import annotations

import re


_TOKEN = re.compile(r"\s*([()!&|]|[^\s()!&|]+)\s*")


def _tokenize(expr: str) -> list[str]:
    out, pos = [], 0
    while pos < len(expr):
        m = _TOKEN.match(expr, pos)
        if not m:
            break
        out.append(m.group(1))
        pos = m.end()
    return out


def expand_selection(selected: set[str], components: list[dict]) -> set[str]:
    """Selecting a parent implies descendants, except exclusive (radio) children."""
    explicit = {s.lower() for s in selected}
    result = set(explicit)
    exclusive = {
        str(c.get("name") or "").lower()
        for c in components
        if "Exclusive" in (c.get("flags") or [])
    }
    for c in components:
        name = str(c.get("name") or "").lower()
        if not name:
            continue
        parent_selected = any(name.startswith(p + "\\") for p in result)
        if parent_selected and (name not in exclusive or name in explicit):
            result.add(name)
    # A parent is considered selected if any descendant is selected.
    for c in components:
        name = str(c.get("name") or "").lower()
        if any(s.startswith(name + "\\") for s in result):
            result.add(name)
    return result


def file_selected(expr: str, selected_lower: set[str]) -> bool:
    if not expr or not expr.strip():
        return True  # no component requirement -> always installed
    toks = _tokenize(expr)
    names = {t.lower() for t in toks if t not in ("(", ")", "!", "&", "|")}
    if len(names) == 1 and names <= selected_lower:
        return True

    # Generic evaluation with name resolution.
    pos = 0

    def parse_atom():
        nonlocal pos
        if pos >= len(toks):
            return False
        t = toks[pos]
        if t == "(":
            pos += 1
            v = parse_or()
            if pos < len(toks) and toks[pos] == ")":
                pos += 1
            return v
        if t == "!":
            pos += 1
            return not parse_atom()
        pos += 1
        return t.lower() in selected_lower

    def parse_and():
        nonlocal pos
        v = parse_atom()
        while pos < len(toks) and toks[pos] == "&":
            pos += 1
            v = parse_atom() and v
        return v

    def parse_or():
        nonlocal pos
        v = parse_and()
        while pos < len(toks) and toks[pos] == "|":
            pos += 1
            v = parse_and() or v
        return v

    return parse_or()


def files_for_selection(meta: dict, selected: list[str]) -> list[dict]:
    sel = expand_selection(set(selected), meta.get("components", []))
    return [f for f in meta.get("files", []) if file_selected(f.get("components", ""), sel)]
