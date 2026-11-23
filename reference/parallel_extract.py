#!/usr/bin/env python3
"""
parallel_extract.py -- parallel extraction engine for a custom Inno Setup loader.

This module decodes the LZMA1 payload chunks stored across the disk-span
``.bin`` slices of an Inno Setup installer, in parallel, and writes the
resulting files into an output directory while verifying checksums.

Layout facts (validated against op-2.2-1a.bin .. op-2.2-2d.bin)
---------------------------------------------------------------
* Each slice file begins with a 12-byte header::

      8 bytes  magic  b'idska32\\x1a'
      4 bytes  uint32 slice size (in this build it equals the file size)

  followed by ``filesize - 12`` bytes of raw payload data.
* The payload of all slices, concatenated with the 12-byte headers removed,
  forms one logical stream.  Payload chunks are laid out back-to-back in that
  logical stream.  A chunk may cross a slice boundary.
* A chunk begins with a 9-byte header::

      4 bytes  magic  b'zlb\\x1a'
      1 byte   LZMA1 properties (0x5d => lc=3, lp=0, pb=2)
      4 bytes  uint32 dictionary size (0x04000000 => 64 MiB)

  followed by a raw LZMA1 stream (no end-of-stream size field).

Metadata interface (real schema of ``op22_meta.json``)
-------------------------------------------------------
* ``data_entries[i].first_slice`` is 0-based; ``offset`` is the ABSOLUTE byte
  offset of the chunk's ``zlb`` magic inside that slice's file.  The logical
  stream position is therefore ``slice_data_start[first_slice] + offset - 12``
  (the 12-byte slice header is skipped by SliceReader).  Offset bases are
  auto-detected by peeking for the magic, so data-relative offsets also work.
* ``data_entries[i].size`` is the length of the RAW LZMA1 stream: it excludes
  both the 9-byte ``zlb`` chunk header and the 10-byte LZMA end marker that
  follows the stream.  Consequently ``dec.eof`` is False after decoding all
  ``size`` bytes; correctness is checked via ``uncompressed_size`` and the
  SHA-1 checksum instead.
* ``files[i].size`` is usually 0; the real size is
  ``data_entries[location].uncompressed_size``.  Files sharing a location are
  concatenated in header order inside that chunk.  ``files[0]`` is the
  generated uninstaller (location 0xFFFFFFFF, type 'uninstaller exe') and is
  skipped.
* ``checksum_type`` is 'sha1' for all data entries in this installer.

Usage
-----
    python parallel_extract.py --metadata op22_meta.json \
        --source-dir <dir-with-bin-files> --output-dir <out> --workers 8 \
        [--components '["main"]'] [--list-only] [--verify-only]

Stdlib only.  Python 3.15.
"""

from __future__ import annotations

import argparse
import bisect
import concurrent.futures as cf
import hashlib
import json
import os
import re
import struct
import sys
import time
import zlib
from typing import Any, Dict, Iterable, List, Optional, Sequence, Tuple

# --------------------------------------------------------------------------- #
# Format constants
# --------------------------------------------------------------------------- #

SLICE_MAGIC = b"idska32\x1a"
SLICE_HEADER_SIZE = 12
CHUNK_MAGIC = b"zlb\x1a"
CHUNK_HEADER_SIZE = 9  # magic(4) + prop(1) + dict_size(4)

DEFAULT_PROP = 0x5D
DEFAULT_DICT_SIZE = 64 << 20

# Inno Setup's call-instruction preprocessing works on fixed 64 KiB blocks.
# A data entry flagged "CallInstructionOptimized" stores the transformed
# bytes; the installer reverses the transform on the way out.  Both checksum
# correctness and executable validity depend on reproducing it exactly.
CALL_OPT_BLOCK = 65536


def transform_call_instructions(buf: bytearray, size: int, addr_offset: int) -> None:
    """Reverse Inno Setup's relative->offset CALL/JMP rewrite (decode path).

    Ported verbatim from Inno Setup's Compression.Base.pas,
    ``TransformCallInstructions(..., Encode=False)``.  ``buf`` is one 64 KiB
    block; ``addr_offset`` is the number of uncompressed bytes emitted before
    this block.
    """
    if size < 5:
        return
    limit = size - 4
    i = 0
    while i < limit:
        if buf[i] in (0xE8, 0xE9):
            i += 1
            if buf[i + 3] in (0x00, 0xFF):
                addr = (addr_offset + i + 4) & 0xFFFFFF
                rel = buf[i] | (buf[i + 1] << 8) | (buf[i + 2] << 16)
                rel = (rel - addr) & 0xFFFFFFFF
                if rel & 0x800000:
                    buf[i + 3] = (~buf[i + 3]) & 0xFF
                buf[i] = rel & 0xFF
                buf[i + 1] = (rel >> 8) & 0xFF
                buf[i + 2] = (rel >> 16) & 0xFF
            i += 4
        else:
            i += 1


class CallOptimizer:
    """Streaming 64 KiB block rewriter for CallInstructionOptimized chunks."""

    def __init__(self, enabled: bool) -> None:
        self.enabled = enabled
        self._buf = bytearray()
        self._offset = 0

    def feed(self, piece: bytes) -> bytes:
        if not self.enabled or not piece:
            return piece
        self._buf.extend(piece)
        out = bytearray()
        while len(self._buf) >= CALL_OPT_BLOCK:
            block = self._buf[:CALL_OPT_BLOCK]
            del self._buf[:CALL_OPT_BLOCK]
            transform_call_instructions(block, CALL_OPT_BLOCK, self._offset)
            out.extend(block)
            self._offset = (self._offset + CALL_OPT_BLOCK) & 0xFFFFFFFF
        return bytes(out)

    def finish(self) -> bytes:
        if not self.enabled or not self._buf:
            return b""
        block = bytes(self._buf)
        self._buf.clear()
        ba = bytearray(block)
        transform_call_instructions(ba, len(ba), self._offset)
        self._offset = (self._offset + len(ba)) & 0xFFFFFFFF
        return bytes(ba)


# Block sizes used for bounded-memory streaming.
COMPRESSED_BLOCK = 4 << 20  # read 4 MiB of compressed input at a time
OUTPUT_BLOCK = 16 << 20  # cap each decompress() result at 16 MiB

# Worker configuration is stored in module globals so that ProcessPoolExecutor
# children can initialise their own SliceReader without pickling file handles.
_WORKER_CFG: Dict[str, Any] = {}
_WORKER_READER: Optional["SliceReader"] = None


# --------------------------------------------------------------------------- #
# SliceReader
# --------------------------------------------------------------------------- #


class SliceReader:
    """A seekable, read-only view over the concatenated slice payloads.

    The 12-byte header of each slice is transparently skipped, so logical
    position 0 is the first byte of the first chunk's magic.  Seeking and
    reads use Python ints, so offsets well beyond 2 GiB are safe.
    """

    def __init__(
        self,
        source_dir: str,
        slices_per_disk: int = 6,
        prefix: str = "op-2.2",
        slice_names: Optional[Sequence[str]] = None,
    ) -> None:
        self.source_dir = source_dir
        self.slices_per_disk = slices_per_disk
        self.prefix = prefix
        self.names = list(slice_names) if slice_names else self._discover()

        self.paths = [os.path.join(source_dir, n) for n in self.names]
        self.starts: List[int] = []
        self.lengths: List[int] = []
        self.total = 0
        for path in self.paths:
            fsize = os.path.getsize(path)
            with open(path, "rb") as fh:
                head = fh.read(SLICE_HEADER_SIZE)
            if len(head) != SLICE_HEADER_SIZE or head[:8] != SLICE_MAGIC:
                raise ValueError("bad slice header in %s: %r" % (path, head[:8]))
            stored = struct.unpack_from("<I", head, 8)[0]
            # This build stores the whole file size in the header; other
            # Inno Setup variants store the payload length.  Accept both.
            if stored not in (fsize, fsize - SLICE_HEADER_SIZE):
                raise ValueError(
                    "slice size mismatch in %s: header=%d file=%d"
                    % (path, stored, fsize)
                )
            data_len = fsize - SLICE_HEADER_SIZE
            self.starts.append(self.total)
            self.lengths.append(data_len)
            self.total += data_len

        self._pos = 0
        self._idx = -1
        self._within = 0
        self._fh = None
        self._seek(0)

    # -- discovery ---------------------------------------------------------- #

    def _discover(self) -> List[str]:
        found = sorted(
            f for f in os.listdir(self.source_dir)
            if f.lower().endswith(".bin") and re.match(r"^op-2\.2-\d+[a-z]\.bin$", f)
        )
        if not found:
            found = sorted(f for f in os.listdir(self.source_dir) if f.lower().endswith(".bin"))
        if not found:
            raise FileNotFoundError("no .bin slice files in %s" % self.source_dir)
        return found

    @classmethod
    def name_for_index(cls, index: int, slices_per_disk: int, prefix: str = "op-2.2") -> str:
        """Return the canonical slice filename for a 0-based slice index."""
        major = index // slices_per_disk + 1
        minor = index % slices_per_disk
        return "%s-%d%s.bin" % (prefix, major, chr(ord("a") + minor))

    # -- file handling ------------------------------------------------------ #

    def _open_index(self, idx: int) -> None:
        if idx == self._idx and self._fh is not None:
            return
        if self._fh is not None:
            self._fh.close()
            self._fh = None
        if 0 <= idx < len(self.paths):
            self._fh = open(self.paths[idx], "rb", buffering=0)
            self._idx = idx
        else:
            self._idx = -1

    def _seek(self, pos: int) -> None:
        if pos < 0:
            raise ValueError("negative seek position")
        if pos > self.total:
            pos = self.total
        if not self.lengths:
            self._idx = -1
            self._within = 0
            self._pos = pos
            return
        # Which slice does ``pos`` fall in?  Clamp the last slice so that
        # pos == total maps to idx == len-1, within == length.
        idx = bisect.bisect_right(self.starts, pos) - 1
        if idx >= len(self.lengths):
            idx = len(self.lengths) - 1
        elif idx < 0:
            idx = 0
        within = pos - self.starts[idx]
        self._open_index(idx)
        if self._fh is not None:
            self._fh.seek(SLICE_HEADER_SIZE + within)
        self._pos = pos
        self._idx = idx
        self._within = within

    # -- public API --------------------------------------------------------- #

    def seek(self, pos: int, whence: int = os.SEEK_SET) -> int:
        if whence == os.SEEK_SET:
            new = pos
        elif whence == os.SEEK_CUR:
            new = self._pos + pos
        elif whence == os.SEEK_END:
            new = self.total + pos
        else:
            raise ValueError("invalid whence")
        self._seek(new)
        return self._pos

    def tell(self) -> int:
        return self._pos

    def read(self, n: int = -1) -> bytes:
        if n < 0:
            n = self.total - self._pos
        if n == 0 or self._pos >= self.total:
            return b""
        chunks: List[bytes] = []
        remaining = n
        while remaining > 0 and self._pos < self.total:
            if self._idx < 0 or self._within >= self.lengths[self._idx]:
                self._seek(self._pos)
            avail = self.lengths[self._idx] - self._within
            take = min(remaining, avail)
            if self._fh is None:
                break
            data = self._fh.read(take)
            if not data:
                break
            chunks.append(data)
            got = len(data)
            remaining -= got
            self._pos += got
            self._within += got
        return b"".join(chunks)

    def read_exact(self, n: int) -> bytes:
        data = self.read(n)
        if len(data) != n:
            raise EOFError("wanted %d bytes at %d, got %d" % (n, self._pos - len(data), len(data)))
        return data

    def close(self) -> None:
        if self._fh is not None:
            self._fh.close()
            self._fh = None
            self._idx = -1

    def __enter__(self) -> "SliceReader":
        return self

    def __exit__(self, *exc: Any) -> None:
        self.close()


# --------------------------------------------------------------------------- #
# Worker initialisation / decode
# --------------------------------------------------------------------------- #


def init_worker(cfg: Dict[str, Any], shared: bool = True) -> None:
    """Set up worker state.

    ProcessPoolExecutor children call this with ``shared=True`` so each
    process owns one reader.  ThreadPoolExecutor threads cannot share a
    seekable reader (seeks would race), so they get ``shared=False`` and
    build a private reader inside each decode_chunk call.
    """
    global _WORKER_CFG, _WORKER_READER
    _WORKER_CFG = cfg
    if shared:
        _WORKER_READER = SliceReader(
            cfg["source_dir"],
            cfg.get("slices_per_disk", 6),
            cfg.get("prefix", "op-2.2"),
            cfg.get("slice_names"),
        )
    else:
        _WORKER_READER = None


def _new_digests() -> Dict[str, Any]:
    return {
        "crc32": 0,
        "md5": hashlib.md5(),
        "sha1": hashlib.sha1(),
        "sha256": hashlib.sha256(),
    }


def _feed_digests(digests: Dict[str, Any], data: bytes) -> None:
    digests["crc32"] = zlib.crc32(data, digests["crc32"])
    digests["md5"].update(data)
    digests["sha1"].update(data)
    digests["sha256"].update(data)


def _digest_hex(digests: Dict[str, Any]) -> Dict[str, str]:
    return {
        "crc32": "%08x" % (digests["crc32"] & 0xFFFFFFFF),
        "md5": digests["md5"].hexdigest(),
        "sha1": digests["sha1"].hexdigest(),
        "sha256": digests["sha256"].hexdigest(),
    }


def _filters_from_header(header: bytes) -> List[Dict[str, Any]]:
    if len(header) < CHUNK_HEADER_SIZE or header[:4] != CHUNK_MAGIC:
        raise ValueError("not a zlb chunk: %r" % header[:4])
    prop = header[4]
    lc = prop % 9
    rem = prop // 9
    lp = rem % 5
    pb = rem // 5
    dict_size = struct.unpack_from("<I", header, 5)[0]
    if lc > 4 or lp > 4 or pb > 4 or dict_size == 0:
        raise ValueError("invalid LZMA1 header prop=0x%02x dict=%d" % (prop, dict_size))
    return [{
        "id": lzma_id(),
        "dict_size": dict_size,
        "lc": lc,
        "lp": lp,
        "pb": pb,
    }]


def lzma_id() -> int:
    import lzma
    return lzma.FILTER_LZMA1


def decode_chunk(task: Dict[str, Any]) -> Dict[str, Any]:
    """Decode one payload chunk into a temp file; return a small result dict.

    The compressed bytes are streamed from the logical slice stream in bounded
    blocks and the decompressed bytes are written to ``tmp_path`` on disk, so
    RAM use stays bounded regardless of chunk size (largest chunks expand to
    several GiB).  CRC32/MD5/SHA1/SHA256 are computed over the output as it is
    written.
    """
    global _WORKER_CFG, _WORKER_READER
    import lzma

    idx = task["index"]
    t0 = time.perf_counter()
    reader = _WORKER_READER
    owns_reader = False
    try:
        if reader is None:  # thread mode / direct call: private reader
            cfg = _WORKER_CFG or task
            reader = SliceReader(
                cfg["source_dir"],
                cfg.get("slices_per_disk", 6),
                cfg.get("prefix", "op-2.2"),
                cfg.get("slice_names"),
            )
            owns_reader = True

        # Locate the chunk magic.  Real metadata gives an absolute file
        # offset (logical = slice_data_start + offset - 12); some test
        # metadata uses a data-relative offset.  Peek both candidates.
        base = task["slice_start"]
        offset = task["offset"]
        candidates = [base + offset - SLICE_HEADER_SIZE, base + offset]
        pos = None
        for cand in candidates:
            if cand < 0 or cand + CHUNK_HEADER_SIZE > reader.total:
                continue
            reader.seek(cand)
            if reader.read(4) == CHUNK_MAGIC:
                pos = cand
                break
        if pos is None:
            raise ValueError("no zlb magic at %s for chunk %d" % (candidates, idx))

        reader.seek(pos)
        header = reader.read_exact(CHUNK_HEADER_SIZE)
        filters = _filters_from_header(header)
        # ``size`` is the raw LZMA stream length (no zlb header, no end
        # marker).  Honor an explicit override for alternate metadata.
        rest = task["size"] if task.get("size_includes_header") is not True else task["size"] - CHUNK_HEADER_SIZE
        reader.seek(pos + CHUNK_HEADER_SIZE)

        dec = lzma.LZMADecompressor(format=lzma.FORMAT_RAW, filters=filters)
        digests = _new_digests()
        tmp_path = task["tmp_path"]
        in_bytes = 0
        out_bytes = 0
        eof = False
        call_opt = CallOptimizer(bool(task.get("call_optimize")))

        def emit(piece: bytes) -> None:
            nonlocal out_bytes
            if not piece:
                return
            if call_opt.enabled:
                piece = call_opt.feed(piece)
            if piece:
                out.write(piece)
                _feed_digests(digests, piece)
                out_bytes += len(piece)

        def drain(first: bytes) -> None:
            """Write ``first`` plus any output still buffered in the decoder.

            Python 3.15's LZMADecompressor consumes all supplied input and
            exposes ``needs_input`` (False means more output is buffered),
            replacing the old ``unconsumed_tail`` attribute.
            """
            nonlocal eof
            piece = first
            while True:
                emit(piece)
                if dec.eof or dec.needs_input:
                    eof = dec.eof
                    break
                piece = dec.decompress(b"", max_length=OUTPUT_BLOCK)

        with open(tmp_path, "wb", buffering=0) as out:
            remaining = rest
            while remaining > 0 and not dec.eof:
                block = reader.read(min(COMPRESSED_BLOCK, remaining))
                if not block:
                    break
                remaining -= len(block)
                in_bytes += len(block)
                drain(dec.decompress(block, max_length=OUTPUT_BLOCK))
            # Final drain in case output is still buffered after last input.
            if not dec.eof:
                drain(dec.decompress(b"", max_length=OUTPUT_BLOCK))
            final = call_opt.finish()
            if final:
                out.write(final)
                _feed_digests(digests, final)
                out_bytes += len(final)
            eof = dec.eof

        dt = time.perf_counter() - t0
        return {
            "index": idx,
            "tmp_path": tmp_path,
            "in_bytes": in_bytes,
            "out_bytes": out_bytes,
            "elapsed": dt,
            "in_speed": in_bytes / dt / 1048576 if dt else 0.0,
            "out_speed": out_bytes / dt / 1048576 if dt else 0.0,
            "digests": _digest_hex(digests),
            "eof": eof,
            "expected_size": task.get("uncompressed_size"),
            "expected_checksum": task.get("checksum"),
            "error": None,
        }
    except Exception as exc:  # noqa: BLE001 - report any worker failure
        return {
            "index": idx,
            "tmp_path": task.get("tmp_path"),
            "in_bytes": 0,
            "out_bytes": 0,
            "elapsed": time.perf_counter() - t0,
            "in_speed": 0.0,
            "out_speed": 0.0,
            "digests": {},
            "eof": False,
            "expected_size": task.get("uncompressed_size"),
            "expected_checksum": task.get("checksum"),
            "error": "%s: %s" % (type(exc).__name__, exc),
        }
    finally:
        if owns_reader and reader is not None:
            reader.close()


# --------------------------------------------------------------------------- #
# Checksum helpers
# --------------------------------------------------------------------------- #


def normalise_hex(value: Optional[str]) -> Optional[str]:
    if value is None:
        return None
    s = str(value).lower().strip()
    if s.startswith("0x"):
        s = s[2:]
    s = s.replace("-", "").replace(":", "")
    return s


def match_checksum(digests: Dict[str, str], expected: Optional[str],
                   preferred: Optional[str] = None) -> Optional[str]:
    """Return the digest name that matched ``expected``, or None."""
    exp = normalise_hex(expected)
    if exp is None:
        return None
    # Try the declared algorithm first, then the natural one by length, then
    # every supported algorithm (CRC32 / MD5 / SHA1 / SHA256).
    order: List[str] = []
    if preferred:
        p = str(preferred).strip().lower().replace("-", "")
        if p in ("sha1", "sha256", "md5", "crc32"):
            order.append(p)
    order += {8: ["crc32"], 32: ["md5"], 40: ["sha1"], 64: ["sha256"]}.get(len(exp), [])
    order += [k for k in ("crc32", "md5", "sha1", "sha256") if k not in order]
    for name in order:
        if digests.get(name) == exp:
            return name
    return None


# --------------------------------------------------------------------------- #
# Metadata -> task planning
# --------------------------------------------------------------------------- #


def build_tasks(
    meta: Dict[str, Any],
    source_dir: str,
    work_dir: str,
    logical_starts: Sequence[int],
    slice_names: Sequence[str],
    only: Optional[Iterable[int]] = None,
) -> List[Dict[str, Any]]:
    tasks: List[Dict[str, Any]] = []
    only_set = set(only) if only is not None else None
    for i, ent in enumerate(meta["data_entries"]):
        if only_set is not None and i not in only_set:
            continue
        first = ent["first_slice"]
        offset = ent["offset"]
        tasks.append({
            "index": i,
            "source_dir": source_dir,
            "slices_per_disk": meta.get("slices_per_disk", 6),
            "prefix": "op-2.2",
            "slice_names": list(slice_names),
            "first_slice": first,
            "offset": offset,
            "slice_start": logical_starts[first],
            "size": ent["size"],
            "size_includes_header": bool(ent.get("size_includes_header", False)),
            "uncompressed_size": ent.get("uncompressed_size"),
            "checksum": ent.get("checksum"),
            "call_optimize": "CallInstructionOptimized" in (ent.get("flags") or []),
            "prop": ent.get("prop", DEFAULT_PROP),
            "dict_size": ent.get("dict_size", DEFAULT_DICT_SIZE),
            "tmp_path": os.path.join(work_dir, "chunk_%05d.dec" % i),
        })
    return tasks


def files_by_location(meta: Dict[str, Any]) -> Dict[int, List[Tuple[int, Dict[str, Any]]]]:
    groups: Dict[int, List[Tuple[int, Dict[str, Any]]]] = {}
    for order, f in enumerate(meta.get("files", [])):
        loc = f["location"]
        groups.setdefault(loc, []).append((order, f))
    return groups


def parse_components(value: Optional[str]) -> Optional[set]:
    """Parse ``--components`` as a JSON list, comma list, or '*' (all)."""
    if value is None or not str(value).strip() or str(value).strip() == "*":
        return None
    text = str(value).strip()
    names: List[str] = []
    if text.startswith("["):
        try:
            obj = json.loads(text)
            if isinstance(obj, list):
                names = [str(x) for x in obj]
            else:
                names = [str(obj)]
        except json.JSONDecodeError:
            # Be forgiving of shells that collapse JSON's doubled backslashes
            # (e.g. '["addons\\sounds"]' arriving as '["addons\sounds"]').
            names = [s.replace("\\\\", "\\") for s in re.findall(r'"([^"]*)"', text)]
            if not names:
                names = [s.strip() for s in text.strip("[]").split(",") if s.strip()]
    else:
        names = [x for x in text.split(",") if x.strip()]
    return {n.strip().lower() for n in names if n.strip()}


def component_filter(meta: Dict[str, Any], selected: Optional[str]) -> Tuple[set, int, int]:
    """Return (included file indices, skipped-uninstaller count, extra-loc count)."""
    files = meta.get("files", [])
    uninstaller = sum(
        1 for f in files
        if f.get("location") == 0xFFFFFFFF or f.get("type") == "uninstaller exe"
    )
    extra = sum(1 for f in files if f.get("additional_locations"))
    wanted = parse_components(selected)
    if wanted is None:
        return set(range(len(files))), uninstaller, extra
    included: set = set()
    for i, f in enumerate(files):
        if f.get("location") == 0xFFFFFFFF or f.get("type") == "uninstaller exe":
            continue  # generated uninstaller has no stored chunk
        comps = str(f.get("components", "") or "").lower()
        # Inno component expressions use +/& for boolean combinations; for
        # selection purposes treat each token as an independent component.
        parts = {c.strip() for c in re.split(r"[,&+!]", comps) if c.strip()}
        if not parts or parts & wanted:
            included.add(i)
        else:
            # Selecting a parent component implies its children.
            if any(p.startswith(w + "\\") or p.startswith(w + "/") for w in wanted for p in parts):
                included.add(i)
    return included, uninstaller, extra


def resolve_destination(raw: str) -> Optional[str]:
    """Turn an Inno destination such as ``{app}\\bin\\xrCore.dll`` into a
    relative output path with native separators.  Returns None for empty or
    unusable destinations."""
    if not raw or not raw.strip():
        return None
    path = raw.strip().replace("/", "\\")
    while True:
        m = re.match(r"^\{[^}]+\}\\", path)
        if not m:
            break
        path = path[m.end():]
    # Drop any remaining {...} constants rather than writing literal braces.
    path = re.sub(r"\{[^}]+\}", "", path)
    parts = [p for p in path.split("\\") if p and p not in (".", "..")]
    return os.path.join(*parts) if parts else None


def file_size(f: Dict[str, Any], ent: Dict[str, Any]) -> int:
    """Inno ``files[].size`` is 0; the stored chunk length is authoritative."""
    sz = int(f.get("size", 0) or 0)
    return sz if sz > 0 else int(ent.get("uncompressed_size", 0) or 0)


# --------------------------------------------------------------------------- #
# Orchestration
# --------------------------------------------------------------------------- #


def run_extraction(args: argparse.Namespace) -> int:
    t_start = time.perf_counter()
    with open(args.metadata, "r", encoding="utf-8") as fh:
        meta = json.load(fh)

    spd = int(meta.get("slices_per_disk", 6))
    sample = SliceReader(args.source_dir, spd)
    logical_starts = list(sample.starts)
    slice_names = list(sample.names)
    sample.close()
    print("slices: %d, logical payload: %.2f GiB" % (
        len(slice_names), sum(sample.lengths) / (1 << 30)), flush=True)

    included, uninstaller, extra_locations = component_filter(meta, args.components)
    if uninstaller:
        print("skipping %d generated uninstaller file(s) (no stored chunk)" % uninstaller)
    if extra_locations:
        print("NOTE: %d file(s) have 'additional_locations' (ignored in v1)" % extra_locations)

    groups = files_by_location(meta)
    selected_locations = sorted({
        meta["files"][i]["location"] for i in included
        if meta["files"][i]["location"] < len(meta["data_entries"])
    })

    if args.list_only:
        print("\n%-6s %-8s %-14s %-12s %s" % ("loc", "idx", "size", "component", "destination"))
        for loc in sorted(groups):
            for order, f in groups[loc]:
                mark = " " if order in included else "x"
                ent = meta["data_entries"][loc] if loc < len(meta["data_entries"]) else {}
                print("%-6s %-8s %-14s %-12s %s%s" % (
                    loc, order, file_size(f, ent), str(f.get("components", ""))[:12],
                    mark, resolve_destination(f.get("destination", "")) or "<skip>"))
        print("\n%d data entries, %d files, %d included"
              % (len(meta["data_entries"]), len(meta.get("files", [])), len(included)))
        return 0

    if not args.work_dir:
        # Keep temp chunks on the same volume as the destination so the
        # assembly step can rename them into place instead of copying.
        out_abs = os.path.abspath(args.output_dir)
        args.work_dir = os.path.join(os.path.dirname(out_abs), "_op22_work")
    os.makedirs(args.work_dir, exist_ok=True)
    only = selected_locations if parse_components(args.components) is not None else None
    tasks = build_tasks(meta, args.source_dir, args.work_dir, logical_starts, slice_names, only)
    if not tasks:
        print("no chunks to decode")
        return 0
    print("decoding %d chunk(s) with %d %s worker(s)" % (
        len(tasks), args.workers, args.executor), flush=True)

    cfg = {
        "source_dir": args.source_dir,
        "slices_per_disk": spd,
        "prefix": "op-2.2",
        "slice_names": slice_names,
    }
    results: Dict[int, Dict[str, Any]] = {}
    done_in = done_out = 0
    t_decode = time.perf_counter()

    if args.executor == "process":
        pool: cf.Executor = cf.ProcessPoolExecutor(
            max_workers=args.workers, initializer=init_worker, initargs=(cfg,))
    else:
        pool = cf.ThreadPoolExecutor(max_workers=args.workers)
        init_worker(cfg, shared=False)

    try:
        futures = {pool.submit(decode_chunk, t): t for t in tasks}
        for fut in cf.as_completed(futures):
            r = fut.result()
            results[r["index"]] = r
            done_in += r["in_bytes"]
            done_out += r["out_bytes"]
            if r["error"]:
                print("  chunk %d: ERROR %s" % (r["index"], r["error"]), flush=True)
            else:
                elapsed_total = time.perf_counter() - t_decode
                print("  chunk %2d/%d: %6.1f MiB in / %7.1f MiB out  "
                      "elapsed %.1fs  agg %5.1f MiB/s in, %5.1f MiB/s out"
                      % (r["index"] + 1, len(tasks), r["in_bytes"] / 1048576,
                         r["out_bytes"] / 1048576, elapsed_total,
                         done_in / elapsed_total / 1048576,
                         done_out / elapsed_total / 1048576), flush=True)
    finally:
        pool.shutdown(wait=True)

    t_decode_done = time.perf_counter()
    td = t_decode_done - t_decode
    print("\ndecode: %.1f MiB in, %.1f MiB out in %.2fs = %.1f MiB/s in, %.1f MiB/s out"
          % (done_in / 1048576, done_out / 1048576, td,
             done_in / td / 1048576, done_out / td / 1048576), flush=True)

    # -- checksum / size verification ------------------------------------- #
    mismatches = 0
    matched_algs: Dict[str, int] = {}
    for i, ent in enumerate(meta["data_entries"]):
        r = results.get(i)
        if not r:
            continue
        if r["error"]:
            mismatches += 1
            continue
        exp_size = ent.get("uncompressed_size")
        if exp_size and r["out_bytes"] != exp_size:
            print("SIZE MISMATCH chunk %d: expected %d, got %d" % (
                i, exp_size, r["out_bytes"]))
            mismatches += 1
        # The parser's ``size`` excludes the 10-byte LZMA end marker, so
        # dec.eof is intentionally False; size + checksum are authoritative.
        alg = match_checksum(r["digests"], ent.get("checksum"),
                             ent.get("checksum_type"))
        if ent.get("checksum"):
            if alg:
                matched_algs[alg] = matched_algs.get(alg, 0) + 1
                print("chunk %d checksum OK (%s)" % (i, alg))
            else:
                print("CHECKSUM MISMATCH chunk %d: expected %s, got %s" % (
                    i, ent.get("checksum"), r["digests"]))
                mismatches += 1
    if matched_algs:
        print("checksum algorithms matched: %s" % matched_algs)

    if args.verify_only:
        print("verify-only: skipping file assembly")
        for r in results.values():
            try:
                if r.get("tmp_path") and os.path.exists(r["tmp_path"]):
                    os.remove(r["tmp_path"])
            except OSError:
                pass
    else:
        # -- assemble files ------------------------------------------------- #
        os.makedirs(args.output_dir, exist_ok=True)
        written = 0
        skipped_no_dest = 0
        renamed_count = 0
        for loc, members in sorted(groups.items()):
            if loc >= len(meta["data_entries"]):
                continue  # generated uninstaller
            r = results.get(loc)
            if not r or r["error"] or not os.path.exists(r["tmp_path"]):
                continue
            ent = meta["data_entries"][loc]
            chunk_size = file_size(members[0][1], ent)
            multi = len(members) > 1
            if multi and all(not int(f.get("size", 0) or 0) for _, f in members):
                print("WARN chunk %d has %d files with size=0; cannot split reliably"
                      % (loc, len(members)))
            cursor = 0
            renamed = False
            for order, f in sorted(members, key=lambda x: x[0]):
                if order not in included:
                    cursor += file_size(f, ent) if not multi else int(f.get("size", 0) or 0)
                    continue
                rel = resolve_destination(f.get("destination", ""))
                if not rel:
                    skipped_no_dest += 1
                    continue
                dest = os.path.join(args.output_dir, rel)
                os.makedirs(os.path.dirname(dest) or args.output_dir, exist_ok=True)
                size = file_size(f, ent) if not multi else int(f.get("size", 0) or 0)

                # A chunk that maps 1:1 to a whole file can be installed with
                # an atomic rename instead of copying 35 GiB.  Only safe when
                # no per-file checksum is pending and both paths share a
                # volume; the chunk-level SHA-1 was already verified.
                same_volume = (os.path.splitdrive(r["tmp_path"])[0].lower() ==
                               os.path.splitdrive(dest)[0].lower())
                if (not multi and cursor == 0 and same_volume
                        and not f.get("checksum")
                        and size == int(ent.get("uncompressed_size") or 0)):
                    try:
                        os.replace(r["tmp_path"], dest)
                        renamed = True
                        renamed_count += 1
                        cursor += size
                        written += 1
                        continue
                    except OSError:
                        pass  # fall back to a plain copy
                _copy_range(r["tmp_path"], dest, cursor, size, f.get("checksum"))
                cursor += size
                written += 1
            # Temp file no longer needed once every member of this chunk is
            # written; drop it to bound disk usage.
            if not renamed:
                try:
                    os.remove(r["tmp_path"])
                except OSError:
                    pass
        print("wrote %d file(s) to %s%s%s" % (
            written, args.output_dir,
            " (%d skipped: no destination)" % skipped_no_dest if skipped_no_dest else "",
            " [%d installed by atomic rename]" % renamed_count if renamed_count else ""))

    try:
        os.rmdir(args.work_dir)  # only succeeds when all temp chunks were consumed
    except OSError:
        pass
    print("total elapsed %.2fs; %d mismatch(es)" % (time.perf_counter() - t_start, mismatches))
    return 1 if mismatches else 0


def _copy_range(src: str, dest: str, offset: int, size: int,
                expected_checksum: Optional[str] = None) -> None:
    """Copy ``size`` bytes at ``offset`` from ``src`` into ``dest`` and verify.

    Hashing is only performed when a per-file checksum actually exists.  The
    chunk-level SHA-1 was already verified during decode; this installer has
    no per-file checksums, so hashing here would be four wasted passes over
    35 GiB and was the bottleneck of the assembly phase.
    """
    digests = _new_digests() if expected_checksum else None
    with open(src, "rb") as fi, open(dest, "wb") as fo:
        fi.seek(offset)
        remaining = size
        while remaining > 0:
            block = fi.read(min(8 << 20, remaining))
            if not block:
                break
            fo.write(block)
            if digests is not None:
                _feed_digests(digests, block)
            remaining -= len(block)
    if expected_checksum and digests is not None:
        alg = match_checksum(_digest_hex(digests), expected_checksum)
        if not alg:
            print("FILE CHECKSUM MISMATCH %s: expected %s" % (dest, expected_checksum))


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--metadata", required=True, help="JSON metadata from inno_header.py")
    p.add_argument("--source-dir", default=os.getcwd(),
                   help="directory holding the .bin slices")
    p.add_argument("--output-dir", default=os.path.join(os.getcwd(), "_out"),
                   help="where extracted files go")
    p.add_argument("--work-dir", default=None,
                   help="temp directory for decoded chunks (default: sibling of output dir, "
                        "so 1:1 chunks can be installed with an atomic rename)")
    p.add_argument("--workers", type=int, default=8, help="parallel worker count")
    p.add_argument("--executor", choices=("thread", "process"), default="thread",
                   help="ThreadPoolExecutor (lzma releases the GIL) or ProcessPoolExecutor")
    p.add_argument("--components", default=None,
                   help='JSON list of component names, e.g. \'["main"]\' '
                        '(default: all; "*" = all)')
    p.add_argument("--list-only", action="store_true", help="list files, do not decode")
    p.add_argument("--verify-only", action="store_true", help="decode and verify, do not write files")
    return p


def main(argv: Optional[Sequence[str]] = None) -> int:
    return run_extraction(build_parser().parse_args(argv))


if __name__ == "__main__":
    sys.exit(main())
