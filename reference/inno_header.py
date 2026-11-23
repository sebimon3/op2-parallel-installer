#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
inno_header.py -- parse the header of an Inno Setup 6.1.0 (Unicode) installer.

This is a self-contained, stdlib-only reimplementation of the parts of
InnoExtract (https://github.com/dscharrer/innoextract, ZLIB license) that are
needed to read the *setup header* of an Inno Setup installer.  It was written
for `op-2.2.exe` and its 10 external data slices, but it follows the documented
on-disk format and should work for other modern (4.x/5.x/6.x) installers too.

Format overview
---------------
The .exe contains a "setup loader" table.  InnoExtract finds it either via a
pointer at file offset 0x30 (old versions) or via a PE resource named 11111
(new versions).  For 5.1.5+ the loader header begins with a 12-byte magic:

    r D l P t S CD E6 D7 { 0B *            (loader format version 5.1.5)
    n S 5 W 7 d T 83 AA 1B 0F j            (also 5.1.5)

Following the magic are version-dependent fields; a rolling CRC32 is kept over
every field and compared with a stored CRC at the end.

`header_offset` in that table points at the *setup data header*.  The first 64
bytes there are an ASCII version string, e.g.:

    "Inno Setup Setup Data (6.1.0) (u)"

After it come one or more "blocks".  A block starts with a 9-byte header:

    uint32 expected_crc32      # CRC32 of the next 5 bytes
    uint32 stored_size         # number of bytes in the block body
    uint8  compressed          # 1 => LZMA1 (for setup >= 4.1.6), else stored

The block body is a sequence of CRC-framed sub-blocks:

    [uint32 crc32][up to 4096 data bytes] ...

The concatenated data is an LZMA1 stream whose first 5 bytes are
[prop byte][uint32 dict_size] (prop = (pb*5+lp)*9+lc).  Decompressing it yields
the actual header byte stream from which the structures below are parsed.

The primary header stream contains the main header followed by languages,
messages, permissions, types, components, tasks, directories, files, icons,
INI entries, registry entries, delete entries, run entries, wizard images and
the (optional) decompressor DLL.  A *second* block immediately follows and
contains the "data entries" (one per compressed file chunk), which tell us how
to locate each chunk in the external .bin slices.

Strings are stored as a uint32 byte-length followed by that many bytes.  For a
Unicode installer the bytes are UTF-16LE; ANSI strings are Windows-1252.  A
length of 0xFFFFFFFF denotes a null string (handled defensively here).

The module never writes to the installer or to the .bin files.
"""

import hashlib
import json
import lzma
import os
import struct
import sys
import zlib


# ---------------------------------------------------------------------------
# Version handling
# ---------------------------------------------------------------------------

def V(a, b, c, d=0):
    """Encode a version as a comparable tuple."""
    return (a, b, c, d)


# Known setup-data version strings -> (version tuple, is_unicode).
# (Only the entries relevant to modern installers are listed; the generic
#  parser below handles any string of the form "Inno Setup ... (a.b.c)".)
KNOWN_VERSIONS = {
    b"Inno Setup Setup Data (6.1.0) (u)": (V(6, 1, 0), True),
    b"Inno Setup Setup Data (6.0.0) (u)": (V(6, 0, 0), True),
    b"Inno Setup Setup Data (6.3.0)":     (V(6, 3, 0), True),
    b"Inno Setup Setup Data (5.6.1) (u)": (V(5, 6, 1), True),
    b"Inno Setup Setup Data (5.6.0) (u)": (V(5, 6, 0), True),
    b"Inno Setup Setup Data (5.5.7) (u)": (V(5, 5, 7), True),
    b"Inno Setup Setup Data (5.5.6) (u)": (V(5, 5, 6), True),
}


def parse_version_string(raw64):
    """Return (version_tuple, is_unicode, version_string) from 64 raw bytes."""
    raw = bytes(raw64[:64])
    text = raw.split(b"\x00", 1)[0].decode("ascii", "replace")
    if raw in KNOWN_VERSIONS:
        ver, uni = KNOWN_VERSIONS[raw]
        return ver, uni, text
    # Generic fallback: pull the first a.b.c out of parentheses.
    import re
    m = re.search(r"\((\d+)\.(\d+)\.(\d+)(?:\.(\d+))?\)", text)
    if not m or "Inno Setup" not in text:
        raise ValueError("Unrecognised setup data version: %r" % text)
    ver = V(int(m.group(1)), int(m.group(2)), int(m.group(3)),
            int(m.group(4) or 0))
    uni = ("(u)" in text.lower()) or ver >= V(6, 3, 0)
    return ver, uni, text


# ---------------------------------------------------------------------------
# Compression method enum values (from stream::compression_method in
# InnoExtract; index into stored_compression_method_3 for >= 5.3.9).
# ---------------------------------------------------------------------------
# The on-disk compression values are Inno Setup's own enum: 0=Stored, 1=Zlib,
# 2=BZip2, 3=LZMA1, 4=LZMA2 (InnoExtract's tables include an extra leading
# UnknownCompression sentinel, which would shift these by one).
COMPRESSION_NAMES = ["stored", "zlib", "bzip2", "lzma1", "lzma2"]

# Flag-name tables.  Order matters: it mirrors the exact order in which
# InnoExtract's `stored_flag_reader.add()` calls are made for version 6.1.0.
HEADER_FLAG_NAMES = [
    "DisableStartupPrompt", "CreateAppDir", "AllowNoIcons", "AlwaysRestart",
    "AlwaysUsePersonalGroup", "WindowVisible", "WindowShowCaption",
    "WindowResizable", "WindowStartMaximized", "EnableDirDoesntExistWarning",
    "Password", "AllowRootDirectory", "DisableFinishedPage",
    "UsePreviousAppDir", "BackColorHorizontal", "UsePreviousGroup",
    "UpdateUninstallLogAppName", "UsePreviousSetupType", "DisableReadyMemo",
    "AlwaysShowComponentsList", "FlatComponentsList", "ShowComponentSizes",
    "UsePreviousTasks", "DisableReadyPage", "AlwaysShowDirOnReadyPage",
    "AlwaysShowGroupOnReadyPage", "AllowUNCPath", "UserInfoPage",
    "UsePreviousUserInfo", "UninstallRestartComputer", "RestartIfNeededByRun",
    "ShowTasksTreeLines", "AllowCancelDuringInstall", "WizardImageStretch",
    "AppendDefaultDirName", "AppendDefaultGroupName", "EncryptionUsed",
    "SetupLogging", "SignedUninstaller", "UsePreviousLanguage",
    "DisableWelcomePage", "CloseApplications", "RestartApplications",
    "AllowNetworkDrive", "ForceCloseApplications", "AppNameHasConsts",
    "UsePreviousPrivileges", "WizardResizable",
]
COMPONENT_FLAG_NAMES = [
    "Fixed", "Restart", "DisableNoUninstallWarning", "Exclusive",
    "DontInheritCheck",
]
TYPE_FLAG_NAMES = ["CustomSetupType"]
TASK_FLAG_NAMES = [
    "Exclusive", "Unchecked", "Restart", "CheckedOnce", "DontInheritCheck",
]
DIRECTORY_FLAG_NAMES = [
    "NeverUninstall", "DeleteAfterInstall", "AlwaysUninstall",
    "SetNtfsCompression", "UnsetNtfsCompression",
]
FILE_FLAG_NAMES = [
    "ConfirmOverwrite", "NeverUninstall", "RestartReplace",
    "DeleteAfterInstall", "RegisterServer", "RegisterTypeLib", "SharedFile",
    "CompareTimeStamp", "FontIsNotTrueType", "SkipIfSourceDoesntExist",
    "OverwriteReadOnly", "OverwriteSameVersion", "CustomDestName",
    "OnlyIfDestFileExists", "NoRegError", "UninsRestartDelete",
    "OnlyIfDoesntExist", "IgnoreVersion", "PromptIfOlder", "DontCopy",
    "UninsRemoveReadOnly", "RecurseSubDirsExternal",
    "ReplaceSameVersionIfContentsDiffer", "DontVerifyChecksum",
    "UninsNoSharedFilePrompt", "CreateAllSubDirs", "Bits32", "Bits64",
    "ExternalSizePreset", "SetNtfsCompression", "UnsetNtfsCompression",
    "GacInstall",
]
ICON_FLAG_NAMES = [
    "NeverUninstall", "CreateOnlyIfFileExists", "UseAppPaths",
    "FolderShortcut", "ExcludeFromShowInNewInstall", "PreventPinning",
    "HasAppUserModelToastActivatorCLSID",
]
INI_FLAG_NAMES = [
    "CreateKeyIfDoesntExist", "UninsDeleteEntry", "UninsDeleteEntireSection",
    "UninsDeleteSectionIfEmpty", "HasValue",
]
REGISTRY_FLAG_NAMES = [
    "CreateValueIfDoesntExist", "UninsDeleteValue", "UninsClearValue",
    "UninsDeleteEntireKey", "UninsDeleteEntireKeyIfEmpty",
    "PreserveStringType", "DeleteKey", "DeleteValue", "NoError",
    "DontCreateKey", "Bits32", "Bits64",
]
DATA_FLAG_NAMES = [
    "VersionInfoValid", "VersionInfoNotValid", "TimeStampInUTC",
    "IsUninstallerExe", "CallInstructionOptimized", "Touch",
    "ChunkEncrypted", "ChunkCompressed", "SolidBreak", "Sign", "SignOnce",
]
ARCH_FLAG_NAMES = ["ArchitectureUnknown", "X86", "Amd64", "IA64", "ARM32", "ARM64"]
RUN_FLAG_NAMES = [
    "ShellExec", "SkipIfDoesntExist", "PostInstall", "Unchecked",
    "SkipIfSilent", "SkipIfNotSilent", "HideWizard", "Bits32", "Bits64",
    "RunAsOriginalUser", "DontLogParameters",
]


# ---------------------------------------------------------------------------
# Low-level reader
# ---------------------------------------------------------------------------

class Reader(object):
    """A little-endian cursor over a bytes object with Inno-specific helpers."""

    def __init__(self, data, pos=0):
        self.data = bytes(data)
        self.pos = pos

    @property
    def remaining(self):
        return len(self.data) - self.pos

    def take(self, n):
        if n < 0 or self.pos + n > len(self.data):
            raise EOFError("want %d bytes at %d, have %d" %
                           (n, self.pos, self.remaining))
        out = self.data[self.pos:self.pos + n]
        self.pos += n
        return out

    def u8(self):
        return self.take(1)[0]

    def u16(self):
        return struct.unpack("<H", self.take(2))[0]

    def u32(self):
        return struct.unpack("<I", self.take(4))[0]

    def u64(self):
        return struct.unpack("<Q", self.take(8))[0]

    def i16(self):
        return struct.unpack("<h", self.take(2))[0]

    def i32(self):
        return struct.unpack("<i", self.take(4))[0]

    def i64(self):
        return struct.unpack("<q", self.take(8))[0]

    def string(self, unicode=True, cp="cp1252", raw=False):
        """Read a length-prefixed string.  Length is in *bytes*.

        0xFFFFFFFF denotes a null string; we return None for it.  Unicode
        installers store UTF-16LE; ANSI fields (license text, etc.) are
        Windows-1252.
        """
        n = self.u32()
        if n == 0xFFFFFFFF:
            return None
        blob = self.take(n)
        if raw:
            return blob
        if unicode:
            return blob.decode("utf-16le", "replace")
        return blob.decode(cp, "replace")

    def flags(self, names):
        """Read a packed bit-field with one bit per name.

        3-byte sets are padded to 4 bytes on 32-bit installers.  Returns
        (raw_int, [set flag names]).
        """
        nbytes = (len(names) + 7) // 8
        blob = self.take(nbytes)
        if nbytes == 3:
            self.take(1)  # padding byte
        value = int.from_bytes(blob, "little")
        active = [name for i, name in enumerate(names) if value & (1 << i)]
        return value, active

    def enum(self, names, default=None):
        v = self.u8()
        if v < len(names):
            return names[v]
        return default if default is not None else "%s?(%d)" % (names[0], v)


def read_winver(r):
    """Read a windows_version_range: two windows_versions (begin, end).

    Each windows_version is 10 bytes for setup >= 1.3.19:
        win: uint16 build, uint8 minor, uint8 major
        nt : uint16 build, uint8 minor, uint8 major
        uint8 nt_service_pack.minor, uint8 nt_service_pack.major
    """
    def one_version():
        def one_data():
            build = r.u16()
            minor = r.u8()
            major = r.u8()
            return {"major": major, "minor": minor, "build": build}
        win = one_data()
        nt = one_data()
        sp_minor = r.u8()
        sp_major = r.u8()
        return {"win": win, "nt": nt,
                "nt_service_pack": "%d.%d" % (sp_major, sp_minor)}
    return {"begin": one_version(), "end": one_version()}


def condition_data(r):
    """Read the common `item` condition fields (all present in 6.1.0)."""
    return {
        "components": r.string(),
        "tasks": r.string(),
        "languages": r.string(),
        "check": r.string(),
        "after_install": r.string(),
        "before_install": r.string(),
    }


# ---------------------------------------------------------------------------
# CRC32 / block handling
# ---------------------------------------------------------------------------

def crc32(data):
    return zlib.crc32(data) & 0xFFFFFFFF


# 12-byte loader header magics (InnoExtract offsets.cpp).
LOADER_MAGICS = [
    b"rDlPtS02\x87eVx",
    b"rDlPtS04\x87eVx",
    b"rDlPtS05\x87eVx",
    b"rDlPtS06\x87eVx",
    b"rDlPtS07\x87eVx",
    b"rDlPtS\xcd\xe6\xd7{\x0b*",
    b"nS5W7dT\x83\xaa\x1b\x0fj",
]


def _find_loader_offsets_raw(blob):
    """Locate and decode the setup loader table inside an in-memory .exe image."""
    loader_pos = None
    loader_magic = None
    for m in LOADER_MAGICS:
        i = blob.find(m)
        if i >= 0:
            loader_pos, loader_magic = i, m
            break
    if loader_pos is None:
        # Fall back to the old pointer at 0x30.
        if blob[:4] == b"Inno" and len(blob) >= 0x38:
            off = struct.unpack_from("<I", blob, 0x34)[0]
            inv = struct.unpack_from("<I", blob, 0x38)[0]
            if off == (~inv & 0xFFFFFFFF) and off + 12 <= len(blob):
                loader_pos = off
                loader_magic = blob[off:off + 12]
        if loader_pos is None:
            raise ValueError("No Inno Setup loader header found in %s" % exe_path)

    r = Reader(blob, loader_pos)
    magic = r.take(12)
    running = crc32(magic)

    def crc_field(fmt):
        nonlocal running
        size = struct.calcsize(fmt)
        raw = r.take(size)
        running = zlib.crc32(raw, running) & 0xFFFFFFFF
        return struct.unpack(fmt, raw)[0]

    # >= 5.1.5 has a `revision` field; the magic we found dictates that.
    is_515 = magic in LOADER_MAGICS[5:]
    revision = crc_field("<I") if is_515 else None
    unknown = crc_field("<I")
    exe_offset = crc_field("<I")
    exe_compressed_size = 0 if is_515 else crc_field("<I")
    exe_uncompressed_size = crc_field("<I")
    exe_checksum = crc_field("<I")
    # message_offset only exists for < 4.0.0
    message_offset = 0 if is_515 else crc_field("<I")
    header_offset = crc_field("<I")
    data_offset = crc_field("<I")
    stored_crc = r.u32() if is_515 else None

    result = {
        "loader_offset": loader_pos,
        "loader_magic": magic.hex(),
        "revision": revision,
        "exe_offset": exe_offset,
        "exe_compressed_size": exe_compressed_size,
        "exe_uncompressed_size": exe_uncompressed_size,
        "exe_checksum": "%08x" % exe_checksum,
        "message_offset": message_offset,
        "header_offset": header_offset,
        "data_offset": data_offset,
        "loader_crc_stored": ("%08x" % stored_crc) if stored_crc is not None else None,
        "loader_crc_computed": "%08x" % running,
        "loader_crc_ok": (stored_crc == running) if stored_crc is not None else None,
        "slices_per_disk": None,
    }
    return result


def find_loader_offsets(exe_path):
    """Locate and decode the setup loader table inside the .exe.

    Returns a dict with `header_offset`, `data_offset` and `slices_per_disk`
    (the latter is read from the main header, since the loader table itself
    does not carry it), plus diagnostic/integrity fields.
    """
    with open(exe_path, "rb") as f:
        blob = f.read()
    result = _find_loader_offsets_raw(blob)
    # slices_per_disk lives in the main setup header, not in the loader table.
    header_offset = result["header_offset"]
    ver, uni, _ = parse_version_string(blob[header_offset:header_offset + 64])
    primary, _, _ = _read_block(blob, header_offset + 64)
    hr = Reader(primary)
    result["slices_per_disk"] = _parse_main_header(hr, ver, uni)["slices_per_disk"]
    return result


def _read_block(blob, offset):
    """Decode one CRC-framed, optionally-LZMA-compressed header block.

    Returns (decompressed_bytes, offset_just_after_block, info_dict).
    """
    if offset + 9 > len(blob):
        raise EOFError("block header past end of file at %#x" % offset)
    expected = struct.unpack_from("<I", blob, offset)[0]
    stored_size = struct.unpack_from("<I", blob, offset + 4)[0]
    compressed = blob[offset + 8]
    body = blob[offset + 9:offset + 9 + stored_size]
    if len(body) != stored_size:
        raise EOFError("truncated block at %#x" % offset)

    actual = crc32(blob[offset + 4:offset + 9])
    block_info = {
        "offset": offset,
        "stored_size": stored_size,
        "compressed": bool(compressed),
        "header_crc_ok": (expected == actual),
    }
    if expected != actual:
        raise ValueError("block header CRC32 mismatch at %#x" % offset)

    # Body = sequence of [uint32 crc][<=4096 bytes].
    unframed = bytearray()
    p = 0
    bad = 0
    while p < len(body):
        if p + 4 > len(body):
            raise ValueError("truncated sub-block header")
        want = struct.unpack_from("<I", body, p)[0]
        p += 4
        n = min(4096, len(body) - p)
        payload = body[p:p + n]
        p += n
        unframed += payload
        if crc32(payload) != want:
            bad += 1
    block_info["subblock_crc_failures"] = bad
    block_info["subblocks_ok"] = (bad == 0)
    if bad:
        raise ValueError("%d sub-block CRC32 mismatches" % bad)

    if not compressed:
        data = bytes(unframed)
    else:
        if len(unframed) < 5:
            raise ValueError("LZMA stream too short")
        prop = unframed[0]
        lc, rem = prop % 9, prop // 9
        lp, pb = rem % 5, rem // 5
        dict_size = struct.unpack_from("<I", unframed, 1)[0]
        if prop > 9 * 5 * 5:
            raise ValueError("invalid LZMA property byte %#x" % prop)
        filters = [{"id": lzma.FILTER_LZMA1, "dict_size": dict_size,
                    "lc": lc, "lp": lp, "pb": pb}]
        data = lzma.LZMADecompressor(
            format=lzma.FORMAT_RAW, filters=filters
        ).decompress(bytes(unframed[5:]))
        block_info.update({"lzma_prop": prop, "lc": lc, "lp": lp, "pb": pb,
                           "dict_size": dict_size})
    block_info["decompressed_size"] = len(data)
    return data, offset + 9 + stored_size, block_info


# ---------------------------------------------------------------------------
# Header and entry parsers (targeting 6.1.0; version guards included)
# ---------------------------------------------------------------------------

def _parse_main_header(r, ver, unicode_installer):
    """Parse the main setup header.  `r` is positioned at its start."""
    h = {}

    # ---- strings ----
    h["app_name"] = r.string()
    h["app_versioned_name"] = r.string()
    if ver >= V(1, 3, 0):
        h["app_id"] = r.string()
    h["app_copyright"] = r.string()
    if ver >= V(1, 3, 0):
        h["app_publisher"] = r.string()
        h["app_publisher_url"] = r.string()
    if ver >= V(5, 1, 13):
        h["app_support_phone"] = r.string()
    if ver >= V(1, 3, 0):
        h["app_support_url"] = r.string()
        h["app_updates_url"] = r.string()
        h["app_version"] = r.string()
    h["default_dir_name"] = r.string()
    h["default_group_name"] = r.string()
    if ver < V(3, 0, 0):
        h["uninstall_icon_name"] = r.string(unicode=False)
    h["base_filename"] = r.string()
    if V(1, 3, 0) <= ver < V(5, 2, 5):
        h["license_text"] = r.string(unicode=False)
        h["info_before"] = r.string(unicode=False)
        h["info_after"] = r.string(unicode=False)
    if ver >= V(1, 3, 3):
        h["uninstall_files_dir"] = r.string()
    if ver >= V(1, 3, 6):
        h["uninstall_name"] = r.string()
        h["uninstall_icon"] = r.string()
    if ver >= V(1, 3, 14):
        h["app_mutex"] = r.string()
    if ver >= V(3, 0, 0):
        h["default_user_name"] = r.string()
        h["default_user_organisation"] = r.string()
    if ver >= V(4, 0, 0):
        h["default_serial"] = r.string()
    if V(4, 0, 0) <= ver < V(5, 2, 5):
        h["compiled_code"] = r.string(raw=True)
    if ver >= V(4, 2, 4):
        h["app_readme_file"] = r.string()
        h["app_contact"] = r.string()
        h["app_comments"] = r.string()
        h["app_modify_path"] = r.string()
    if ver >= V(5, 3, 8):
        h["create_uninstall_registry_key"] = r.string()
    if ver >= V(5, 3, 10):
        h["uninstallable"] = r.string()
    if ver >= V(5, 5, 0):
        h["close_applications_filter"] = r.string()
    if ver >= V(5, 5, 6):
        h["setup_mutex"] = r.string()
    if ver >= V(5, 6, 1):
        h["changes_environment"] = r.string()
        h["changes_associations"] = r.string()
    if ver >= V(6, 3, 0):
        h["architectures_allowed_expr"] = r.string()
        h["architectures_installed_in_64bit_mode_expr"] = r.string()
    if ver >= V(5, 2, 5):
        h["license_text"] = r.string(unicode=False)
        h["info_before"] = r.string(unicode=False)
        h["info_after"] = r.string(unicode=False)
    if V(5, 2, 1) <= ver < V(5, 3, 10):
        h["uninstaller_signature"] = r.string(raw=True)
    if ver >= V(5, 2, 5):
        h["compiled_code"] = r.string(raw=True)

    # ---- counts ----
    if ver >= V(4, 0, 0):
        h["language_count"] = r.u32()
    if ver >= V(4, 2, 1):
        h["message_count"] = r.u32()
    if ver >= V(4, 1, 0):
        h["permission_count"] = r.u32()
    if ver >= V(2, 0, 0):
        h["type_count"] = r.u32()
        h["component_count"] = r.u32()
    if ver >= V(2, 0, 0):
        h["task_count"] = r.u32()
    count_fields = [
        "directory_count", "file_count", "data_entry_count", "icon_count",
        "ini_entry_count", "registry_entry_count", "delete_entry_count",
        "uninstall_delete_entry_count", "run_entry_count",
        "uninstall_run_entry_count",
    ]
    for name in count_fields:
        h[name] = r.u32()

    h["winver"] = read_winver(r)
    h["back_color"] = r.u32() if ver < V(6, 4, 0, 1) else 0
    h["back_color2"] = (r.u32() if V(1, 3, 3) <= ver < V(6, 4, 0, 1) else 0)
    h["image_back_color"] = r.u32() if ver < V(5, 5, 7) else 0
    h["small_image_back_color"] = (
        r.u32() if (V(2, 0, 0) <= ver < V(5, 0, 4)) else 0
    )
    if ver >= V(6, 0, 0):
        h["wizard_style"] = r.enum(["classic", "modern"])
        h["wizard_resize_percent_x"] = r.u32()
        h["wizard_resize_percent_y"] = r.u32()
    if ver >= V(5, 5, 7):
        h["image_alpha_format"] = r.enum(
            ["ignored", "defined", "premultiplied"])
    # password hash
    if ver >= V(6, 4, 0):
        h["password_sha256"] = r.take(4).hex()
    elif ver >= V(5, 3, 9):
        h["password_sha1"] = r.take(20).hex()
    elif ver >= V(4, 2, 0):
        h["password_md5"] = r.take(16).hex()
    else:
        h["password_crc32"] = "%08x" % r.u32()
    if ver >= V(6, 4, 0):
        h["password_salt"] = r.take(44).hex()
    elif ver >= V(4, 2, 2):
        h["password_salt"] = r.take(8).hex()

    h["extra_disk_space_required"] = (
        r.i64() if ver >= V(4, 0, 0) else r.i32()
    )
    h["slices_per_disk"] = r.u32() if ver >= V(4, 0, 0) else 1
    if V(2, 0, 0) <= ver < V(5, 0, 0):
        h["install_mode"] = r.enum(["normal", "silent", "very silent"])
    if ver >= V(1, 3, 0):
        h["uninstall_log_mode"] = r.enum(["append", "new log", "overwrite"])
    if ver < V(5, 0, 0):
        h["uninstall_style"] = r.enum(["classic", "modern"])
    if ver >= V(1, 3, 6):
        h["dir_exists_warning"] = r.enum(["auto", "no", "yes"])
    if ver >= V(5, 3, 7):
        h["privileges_required"] = r.enum(
            ["none", "power user", "admin", "lowest"])
    elif ver >= V(3, 0, 4):
        h["privileges_required"] = r.enum(
            ["none", "power user", "admin"])
    if ver >= V(5, 7, 0):
        h["privileges_required_override_allowed"] = r.flags(
            ["Commandline", "Dialog"])[1]
    if ver >= V(4, 0, 10):
        h["show_language_dialog"] = r.enum(["yes", "no", "auto"])
        h["language_detection"] = r.enum(
            ["ui language", "locale", "none"])
    if ver >= V(5, 3, 9):
        h["compression"] = r.enum(COMPRESSION_NAMES, "unknown")
    elif ver >= V(4, 2, 6):
        h["compression"] = r.enum(COMPRESSION_NAMES[:5], "unknown")
    # architectures (for 6.1.0: two 1-byte flag sets)
    if ver >= V(6, 3, 0):
        h["architectures_allowed"] = h.get("architectures_allowed_expr")
        h["architectures_installed_in_64bit_mode"] = h.get(
            "architectures_installed_in_64bit_mode_expr")
    elif ver >= V(5, 6, 0):
        h["architectures_allowed"] = r.flags(ARCH_FLAG_NAMES)[1]
        h["architectures_installed_in_64bit_mode"] = r.flags(ARCH_FLAG_NAMES)[1]
    if V(5, 2, 1) <= ver < V(5, 3, 10):
        h["signed_uninstaller_original_size"] = r.u32()
        h["signed_uninstaller_header_checksum"] = r.u32()
    if ver >= V(5, 3, 3):
        h["disable_dir_page"] = r.enum(["auto", "no", "yes"])
        h["disable_program_group_page"] = r.enum(["auto", "no", "yes"])
    if ver >= V(5, 5, 0):
        h["uninstall_display_size"] = r.u64()

    raw_flags, active = r.flags(HEADER_FLAG_NAMES)
    h["options_raw"] = raw_flags
    h["options"] = active
    h["encryption_used"] = "EncryptionUsed" in active
    return h


def _parse_language(r, ver):
    e = {}
    if ver >= V(4, 0, 0):
        e["name"] = r.string()
    e["language_name"] = r.string()
    if ver == V(5, 5, 7, 1):
        r.string(raw=True)
    for k in ("dialog_font", "title_font", "welcome_font", "copyright_font"):
        e[k] = r.string()
    if ver >= V(4, 0, 0):
        e["data"] = r.string(raw=True)
    if ver >= V(4, 0, 1):
        e["license_text"] = r.string()
        e["info_before"] = r.string()
        e["info_after"] = r.string()
    e["language_id"] = r.u32()
    if ver < V(4, 2, 2):
        pass  # codepage derived from language_id (not needed for Unicode)
    elif ver >= V(4, 2, 2) and ver >= V(5, 3, 0):
        pass  # Unicode: codepage fixed, no field read
    elif ver >= V(4, 2, 2) and ver < V(5, 3, 0):
        r.u32()  # obsolete codepage field
    e["dialog_font_size"] = r.u32()
    if ver < V(4, 1, 0):
        e["dialog_font_standard_height"] = r.u32()
    e["title_font_size"] = r.u32()
    e["welcome_font_size"] = r.u32()
    e["copyright_font_size"] = r.u32()
    if ver == V(5, 5, 7, 1):
        r.u32()
    if ver >= V(5, 2, 3):
        e["right_to_left"] = bool(r.u8())
    return e


def _parse_message(r):
    return {"name": r.string(), "value": r.string(), "language": r.i32()}


def _parse_permission(r):
    return {"permissions": r.string(raw=True)}


def _parse_type(r, ver):
    e = {
        "name": r.string(),
        "description": r.string(),
    }
    if ver >= V(4, 0, 1):
        e["languages"] = r.string()
    e["check"] = r.string() if ver >= V(4, 0, 0) else None
    e["winver"] = read_winver(r)
    _, flags = r.flags(TYPE_FLAG_NAMES)
    e["flags"] = flags
    e["custom_type"] = "CustomSetupType" in flags
    e["type"] = r.enum(["user", "default full", "default compact",
                        "default custom"]) if ver >= V(4, 0, 3) else "user"
    e["size"] = r.u64() if ver >= V(4, 0, 0) else r.u32()
    return e


def _parse_component(r, ver):
    e = {
        "name": r.string(),
        "description": r.string(),
        "types": r.string(),
    }
    if ver >= V(4, 0, 1):
        e["languages"] = r.string()
    e["check"] = r.string() if ver >= V(4, 0, 0) else None
    e["extra_disk_space_required"] = r.u64() if ver >= V(4, 0, 0) else r.u32()
    e["level"] = r.i32() if ver >= V(4, 0, 0) else 0
    e["used"] = bool(r.u8()) if ver >= V(4, 0, 0) else True
    e["winver"] = read_winver(r)
    e["flags"] = r.flags(COMPONENT_FLAG_NAMES)[1]
    e["used_size"] = r.u64() if ver >= V(4, 0, 0) else (
        r.u32() if ver >= V(2, 0, 0) else 0)
    e["size"] = e["used_size"]
    return e


def _parse_task(r, ver):
    e = {
        "name": r.string(),
        "description": r.string(),
        "group_description": r.string(),
        "components": r.string(),
    }
    if ver >= V(4, 0, 1):
        e["languages"] = r.string()
    e["check"] = r.string() if ver >= V(4, 0, 0) else None
    e["level"] = r.i32() if ver >= V(4, 0, 0) else 0
    e["used"] = bool(r.u8()) if ver >= V(4, 0, 0) else True
    e["winver"] = read_winver(r)
    e["flags"] = r.flags(TASK_FLAG_NAMES)[1]
    return e


def _parse_directory(r, ver):
    e = {"name": r.string()}
    e.update(condition_data(r))
    if V(4, 0, 11) <= ver < V(4, 1, 0):
        e["permissions"] = r.string(raw=True)
    if ver >= V(2, 0, 11):
        e["attributes"] = r.u32()
    e["winver"] = read_winver(r)
    e["permission"] = r.i16() if ver >= V(4, 1, 0) else -1
    e["flags"] = r.flags(DIRECTORY_FLAG_NAMES)[1]
    return e


def _parse_file(r, ver):
    e = {
        "source": r.string(),
        "destination": r.string(),
        "install_font_name": r.string(),
    }
    if ver >= V(5, 2, 5):
        e["strong_assembly_name"] = r.string()
    e.update(condition_data(r))
    e["winver"] = read_winver(r)
    e["location"] = r.u32()
    e["attributes"] = r.u32()
    e["external_size"] = r.u64() if ver >= V(4, 0, 0) else r.u32()
    e["permission"] = r.i16() if ver >= V(4, 1, 0) else -1
    e["flags"] = r.flags(FILE_FLAG_NAMES)[1]
    e["type"] = r.enum(["user file", "uninstaller exe", "reg server exe"])
    # `size` and `checksum` are only filled for GOG multi-part files in
    # InnoExtract; normal installers leave them at zero/None.
    e["size"] = 0
    e["checksum"] = None
    return e


def _parse_icon(r, ver):
    e = {}
    for k in ("name", "filename", "parameters", "working_dir", "icon_file",
              "comment"):
        e[k] = r.string()
    e.update(condition_data(r))
    if ver >= V(5, 3, 5):
        e["app_user_model_id"] = r.string()
    if ver >= V(6, 1, 0):
        e["app_user_model_toast_activator_clsid"] = r.take(16).hex()
    e["winver"] = read_winver(r)
    e["icon_index"] = r.i32()
    e["show_command"] = r.i32() if ver >= V(1, 3, 24) else 1
    e["close_on_exit"] = (r.enum(["no setting", "close on exit",
                                  "don't close on exit"])
                          if ver >= V(1, 3, 15) else "no setting")
    e["hotkey"] = r.u16() if ver >= V(2, 0, 7) else 0
    e["flags"] = r.flags(ICON_FLAG_NAMES)[1]
    return e


def _parse_ini(r, ver):
    e = {
        "inifile": r.string(),
        "section": r.string(),
        "key": r.string(),
        "value": r.string(),
    }
    e.update(condition_data(r))
    e["winver"] = read_winver(r)
    e["flags"] = r.flags(INI_FLAG_NAMES)[1]
    return e


def _parse_registry(r, ver):
    e = {"key": r.string(), "name": r.string(), "value": r.string(raw=True)}
    e.update(condition_data(r))
    if V(4, 0, 11) <= ver < V(4, 1, 0):
        e["permissions"] = r.string(raw=True)
    e["winver"] = read_winver(r)
    e["hive_raw"] = r.u32() & ~0x80000000
    e["permission"] = r.i16() if ver >= V(4, 1, 0) else -1
    e["type"] = r.enum(["none", "string", "expand string", "dword", "binary",
                        "multi string", "qword"])
    e["flags"] = r.flags(REGISTRY_FLAG_NAMES)[1]
    return e


def _parse_delete(r, ver):
    e = {"name": r.string()}
    e.update(condition_data(r))
    e["winver"] = read_winver(r)
    e["type"] = r.enum(["files", "files and subdirs", "dir if empty"])
    return e


def _parse_run(r, ver):
    e = {"name": r.string(), "parameters": r.string(), "working_dir": r.string()}
    if ver >= V(1, 3, 9):
        e["run_once_id"] = r.string()
    if ver >= V(2, 0, 2):
        e["status_message"] = r.string()
    if ver >= V(5, 1, 13):
        e["verb"] = r.string()
    e["description"] = r.string() if ver >= V(2, 0, 0) else None
    e.update(condition_data(r))
    e["winver"] = read_winver(r)
    e["show_command"] = r.i32() if ver >= V(1, 3, 24) else 0
    e["wait"] = r.enum(["wait until terminated", "no wait", "wait until idle"])
    e["flags"] = r.flags(RUN_FLAG_NAMES)[1]
    return e


def _load_wizard_images(r, ver):
    count = r.u32() if ver >= V(5, 6, 0) else 1
    return [r.string() for _ in range(count)]


def _parse_data_entry(r, ver, header_compression):
    e = {}
    e["first_slice"] = r.u32()
    e["last_slice"] = r.u32()
    e["offset"] = r.u32()                  # chunk offset within the data stream
    if ver >= V(4, 0, 1):
        e["file_offset"] = r.u64()
    e["uncompressed_size"] = r.u64() if ver >= V(4, 0, 0) else r.u32()
    e["size"] = r.u64() if ver >= V(4, 0, 0) else r.u32()  # compressed chunk size

    # checksum
    if ver >= V(6, 4, 0):
        e["checksum"] = r.take(32).hex()
        e["checksum_type"] = "sha256"
    elif ver >= V(5, 3, 9):
        e["checksum"] = r.take(20).hex()
        e["checksum_type"] = "sha1"
    elif ver >= V(4, 2, 0):
        e["checksum"] = r.take(16).hex()
        e["checksum_type"] = "md5"
    elif ver >= V(4, 0, 1):
        e["checksum"] = "%08x" % r.u32()
        e["checksum_type"] = "crc32"
    else:
        e["checksum"] = "%08x" % r.u32()
        e["checksum_type"] = "adler32"

    # 32-bit installers store a Win32 FILETIME; 16-bit ones a FAT time/date.
    # (This module targets 32-bit Unicode installers such as 6.1.0.)
    e["filetime"] = r.i64()
    e["file_version_ms"] = r.u32()
    e["file_version_ls"] = r.u32()
    e["file_version"] = (e["file_version_ms"] << 32) | e["file_version_ls"]

    raw_flags, active = r.flags(DATA_FLAG_NAMES)
    e["flags_raw"] = raw_flags
    e["flags"] = active
    if ver >= V(6, 3, 0):
        e["sign"] = r.enum(["no setting", "yes", "once", "check"])
    else:
        if "SignOnce" in active:
            e["sign"] = "once"
        elif "Sign" in active:
            e["sign"] = "yes"
        else:
            e["sign"] = "no setting"

    if "ChunkCompressed" in active:
        e["compression"] = header_compression
    elif "BZipped" in active:
        e["compression"] = "bzip2"
    else:
        e["compression"] = "stored"
    if "ChunkEncrypted" in active:
        e["encryption"] = ("xchacha20" if ver >= V(6, 4, 0) else
                           "arc4_sha1" if ver >= V(5, 3, 9) else "arc4_md5")
    else:
        e["encryption"] = "plaintext"
    return e


# ---------------------------------------------------------------------------
# Top-level parse
# ---------------------------------------------------------------------------

def parse_installer(exe_path):
    """Fully parse the setup header of `exe_path`.

    Returns a dict with version, slices_per_disk, header, components, types,
    directories, files, data_entries and the remaining entry categories.
    """
    with open(exe_path, "rb") as f:
        blob = f.read()

    offsets = _find_loader_offsets_raw(blob)
    header_offset = offsets["header_offset"]

    ver, unicode_installer, version_text = parse_version_string(
        blob[header_offset:header_offset + 64])

    # Primary block begins right after the 64-byte version string.
    primary, pos, primary_info = _read_block(blob, header_offset + 64)
    r = Reader(primary)
    header = _parse_main_header(r, ver, unicode_installer)
    slices_per_disk = header["slices_per_disk"]
    hc = header.get("compression", "unknown")

    counts = header
    languages = [_parse_language(r, ver)
                 for _ in range(counts.get("language_count", 0))]
    messages = [_parse_message(r) for _ in range(counts.get("message_count", 0))]
    permissions = [_parse_permission(r)
                   for _ in range(counts.get("permission_count", 0))]
    types = [_parse_type(r, ver) for _ in range(counts.get("type_count", 0))]
    components = [_parse_component(r, ver)
                  for _ in range(counts.get("component_count", 0))]
    tasks = [_parse_task(r, ver) for _ in range(counts.get("task_count", 0))]
    directories = [_parse_directory(r, ver)
                   for _ in range(counts.get("directory_count", 0))]
    files = [_parse_file(r, ver) for _ in range(counts.get("file_count", 0))]
    icons = [_parse_icon(r, ver) for _ in range(counts.get("icon_count", 0))]
    ini_entries = [_parse_ini(r, ver)
                   for _ in range(counts.get("ini_entry_count", 0))]
    registry_entries = [_parse_registry(r, ver)
                        for _ in range(counts.get("registry_entry_count", 0))]
    delete_entries = [_parse_delete(r, ver)
                      for _ in range(counts.get("delete_entry_count", 0))]
    uninstall_delete_entries = [
        _parse_delete(r, ver)
        for _ in range(counts.get("uninstall_delete_entry_count", 0))
    ]
    run_entries = [_parse_run(r, ver)
                   for _ in range(counts.get("run_entry_count", 0))]
    uninstall_run_entries = [
        _parse_run(r, ver)
        for _ in range(counts.get("uninstall_run_entry_count", 0))
    ]

    wizard_images = _load_wizard_images(r, ver)
    wizard_images_small = (
        _load_wizard_images(r, ver) if ver >= V(2, 0, 0) else []
    )
    # Decompressor DLL: present for BZip2, LZMA1 on 4.1.5, or Zlib on >= 4.2.6.
    decompressor_dll = None
    if (hc == "bzip2" or (hc == "lzma1" and ver == V(4, 1, 5)) or
            (hc == "zlib" and ver >= V(4, 2, 6))):
        decompressor_dll = r.string()
    decrypt_dll = None
    if header.get("encryption_used") and ver < V(6, 4, 0):
        decrypt_dll = r.string()

    primary_end_ok = (r.remaining == 0)

    # Secondary block: data entries.
    secondary, pos2, secondary_info = _read_block(blob, pos)
    r2 = Reader(secondary)
    data_entries = [_parse_data_entry(r2, ver, hc)
                    for _ in range(counts.get("data_entry_count", 0))]
    secondary_end_ok = (r2.remaining == 0)

    return {
        "version": (ver[0], ver[1], ver[2]),
        "version_string": version_text,
        "version_tuple": ver,
        "unicode": unicode_installer,
        "slices_per_disk": slices_per_disk,
        "offsets": offsets,
        "blocks": {"primary": primary_info, "secondary": secondary_info},
        "integrity": {
            "loader_crc_ok": offsets.get("loader_crc_ok"),
            "primary_block_crc_ok": primary_info["header_crc_ok"] and
                                    primary_info["subblocks_ok"],
            "secondary_block_crc_ok": secondary_info["header_crc_ok"] and
                                      secondary_info["subblocks_ok"],
            "primary_stream_fully_consumed": primary_end_ok,
            "secondary_stream_fully_consumed": secondary_end_ok,
        },
        "header": header,
        "languages": languages,
        "messages": messages,
        "permissions": permissions,
        "types": types,
        "components": components,
        "tasks": tasks,
        "directories": directories,
        "files": files,
        "icons": icons,
        "ini_entries": ini_entries,
        "registry_entries": registry_entries,
        "delete_entries": delete_entries,
        "uninstall_delete_entries": uninstall_delete_entries,
        "run_entries": run_entries,
        "uninstall_run_entries": uninstall_run_entries,
        "wizard_images": wizard_images,
        "wizard_images_small": wizard_images_small,
        "decompressor_dll": decompressor_dll,
        "decrypt_dll": decrypt_dll,
        "data_entries": data_entries,
    }


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def _json_default(obj):
    if isinstance(obj, bytes):
        return obj.hex()
    raise TypeError("not JSON serializable: %r" % (obj,))


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    full = "--full" in argv
    exe = next((a for a in argv if not a.startswith("--")),
               os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            "op-2.2.exe"))

    info = parse_installer(exe)
    h = info["header"]

    if full:
        print(json.dumps(info, indent=2, ensure_ascii=False,
                         default=_json_default))
        return 0

    # Compact JSON summary.
    summary = {
        "exe": exe,
        "version": info["version_string"],
        "slices_per_disk": info["slices_per_disk"],
        "app_name": h.get("app_name"),
        "app_version": h.get("app_version"),
        "base_filename": h.get("base_filename"),
        "compression": h.get("compression"),
        "counts": {
            "languages": len(info["languages"]),
            "types": len(info["types"]),
            "components": len(info["components"]),
            "tasks": len(info["tasks"]),
            "directories": len(info["directories"]),
            "files": len(info["files"]),
            "data_entries": len(info["data_entries"]),
        },
        "integrity": info["integrity"],
    }
    print(json.dumps(summary, indent=2, ensure_ascii=False,
                     default=_json_default))

    print("\n--- Overview ---")
    print("App:        %s  %s" % (h.get("app_name"), h.get("app_version") or ""))
    print("Version:    %s (unicode=%s)" %
          (info["version_string"], info["unicode"]))
    print("Slices/disk:%d" % info["slices_per_disk"])
    print("Compression:%s" % h.get("compression"))
    print("Base file:  %s" % h.get("base_filename"))
    print("Types:      %d" % len(info["types"]))
    print("Components: %d" % len(info["components"]))
    for c in info["components"]:
        print("   - %s   (size=%s, flags=%s)" %
              (c["name"], c.get("used_size"), ", ".join(c.get("flags", []))))
    print("Files:      %d" % len(info["files"]))
    print("Data recs:  %d" % len(info["data_entries"]))
    integ = info["integrity"]
    print("Integrity:  loader=%s primary=%s secondary=%s streams=%s/%s" % (
        integ["loader_crc_ok"], integ["primary_block_crc_ok"],
        integ["secondary_block_crc_ok"],
        integ["primary_stream_fully_consumed"],
        integ["secondary_stream_fully_consumed"]))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as e:  # pragma: no cover
        import traceback
        traceback.print_exc()
        sys.exit(1)
