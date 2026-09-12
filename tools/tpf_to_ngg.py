#!/usr/bin/env python3
"""
tpf_to_ngg.py - Convert a Texmod .tpf texture pack into a native
NextGenGraphics (NGG) texture pack for NFS Most Wanted.

TPF format:
  1. Whole file XOR-encrypted with key 0x3FA43FA4 (little-endian, repeating)
  2. Result is a standard ZIP whose entries may be ZipCrypto-encrypted with a
     fixed 42-byte "password" (Texmod convention)
  3. Optional texmod.def maps game texture hashes (hex) to entry filenames

Native NGG pack layout produced:
  <output>/
    TexturePackInfo.json   { "rootDirectory": "textures", "textureMappings": [...] }
    textures/              extracted .dds files

gameId resolution order for each DDS entry:
  1. texmod.def mapping (filename -> game hash) -> gameId = "0x<gameHash>"
  2. CRC32 parsed from filename, looked up in the static CRC32<->gameHash
     cache (HashMaps/MW_CRC32Cache_Static.h) -> one mapping per game hash
  3. Normalized base filename as plain-string gameId (the NGG loader DJB-
     hashes it with the game's own hash function, matching TPFLoader's
     fallback behavior)

Usage:
  python tpf_to_ngg.py <pack.tpf> [-o output_dir] [--crc32-cache path]

Drop the result into  <game>/NextGenGraphics/TexturePacks/
"""

import argparse
import io
import json
import re
import struct
import sys
import zipfile
from pathlib import Path

# Fixed ZipCrypto password used by Texmod TPF files (42 bytes)
TPF_ZIP_KEY = bytes([
    0x73, 0x2A, 0x63, 0x7D, 0x5F, 0x0A, 0xA6, 0xBD,
    0x7D, 0x65, 0x7E, 0x67, 0x61, 0x2A, 0x7F, 0x7F,
    0x74, 0x61, 0x67, 0x5B, 0x60, 0x70, 0x45, 0x74,
    0x5C, 0x22, 0x74, 0x5D, 0x6E, 0x6A, 0x73, 0x41,
    0x77, 0x6E, 0x46, 0x47, 0x77, 0x49, 0x0C, 0x4B,
    0x46, 0x6F,
])

XOR_KEY = struct.pack("<I", 0x3FA43FA4)

# Pack prefixes stripped when normalizing names (mirrors TPFLoader)
NAME_PREFIXES = ("SPEED.EXE_", "speed_t_", "SPEED_t_")


def xor_decrypt(data: bytes) -> bytes:
    key = XOR_KEY
    n = len(data)
    out = bytearray(data)
    for i in range(n):
        out[i] ^= key[i & 3]
    return bytes(out)


def strip_prefixes(name: str) -> str:
    changed = True
    while changed:
        changed = False
        for pfx in NAME_PREFIXES:
            if name.lower().startswith(pfx.lower()):
                name = name[len(pfx):].strip()
                changed = True
                break
    return name


def djb_game_hash(s: str) -> int:
    """Game's bStringHash (DJB variant, seed 0xFFFFFFFF) - same as CalcHash."""
    h = 0xFFFFFFFF
    for c in s.encode("ascii", errors="replace"):
        h = (h * 33 + c) & 0xFFFFFFFF
    return h


def parse_crc32_from_filename(filename: str):
    """Mirror of TPFLoader::ParseCRC32FromFilename.

    Returns (crc32_or_djb, was_explicit_hex).
    """
    base = Path(filename).stem.strip()
    base = strip_prefixes(base)

    # 1) "0x" followed by 8 hex digits anywhere in the base
    for m in re.finditer(r"0[xX]([0-9a-fA-F]{8})", base):
        val = int(m.group(1), 16)
        if val != 0:
            return val, True

    # 2) Last run of exactly 8 hex digits
    for m in list(re.finditer(r"[0-9a-fA-F]{8}", base))[::-1]:
        return int(m.group(0), 16), True

    # 3) DJB fallback over the normalized base name
    return djb_game_hash(base), False


def parse_texmod_def(content: str):
    """Returns dict: normalized entry filename -> game hash."""
    mapping = {}
    for line in content.splitlines():
        line = line.strip()
        if not line or line.startswith("#") or line.startswith("//"):
            continue
        if "|" not in line:
            continue
        game_hash_str, filename = line.split("|", 1)
        game_hash_str = game_hash_str.strip()
        filename = filename.strip()
        try:
            game_hash = int(game_hash_str, 16)
        except ValueError:
            continue
        # Strip SPEED.EXE_ prefix, matching TPFLoader
        if filename.lower().startswith("speed.exe_"):
            filename = filename[len("SPEED.EXE_"):].strip()
        mapping[filename] = game_hash
    return mapping


def load_crc32_cache(path: Path):
    """Parse HashMaps/MW_CRC32Cache_Static.h -> dict crc32 -> [gameHash, ...]"""
    text = path.read_text(encoding="utf-8-sig")
    pairs = re.findall(
        r"\{\s*0x([0-9A-Fa-f]+)u\s*,\s*0x([0-9A-Fa-f]+)u\s*\}", text)
    crc_to_game = {}
    for game_hex, crc_hex in pairs:
        game_hash, crc = int(game_hex, 16), int(crc_hex, 16)
        crc_to_game.setdefault(crc, [])
        if game_hash not in crc_to_game[crc]:
            crc_to_game[crc].append(game_hash)
    return crc_to_game


def load_name_table(path: Path):
    """Load HashMaps/MW_TextureNames.json -> (gamehash -> name, crc32 -> [names])"""
    tbl = json.loads(path.read_text(encoding="utf-8"))
    g2n = {int(k): v for k, v in tbl["gamehash_to_name"].items()}
    c2n = {int(k): v for k, v in tbl["crc32_to_names"].items()}
    return g2n, c2n


def game_id_for(game_hash: int, g2n: dict):
    """Prefer the readable texture name as gameId; fall back to hex hash."""
    name = g2n.get(game_hash)
    return name if name else f"0x{game_hash:08X}"


def convert(tpf_path: Path, out_dir: Path, crc_to_game: dict,
            g2n: dict, c2n: dict):
    raw = tpf_path.read_bytes()
    decrypted = xor_decrypt(raw)
    if decrypted[:2] != b"PK":
        print(f"error: {tpf_path} is not a valid ZIP after XOR decryption",
              file=sys.stderr)
        return 1

    zf = zipfile.ZipFile(io.BytesIO(decrypted))
    zf.pwd = TPF_ZIP_KEY  # used for ZipCrypto-encrypted entries

    known_game_hashes = set(g2n) | {g for ghs in crc_to_game.values()
                                    for g in ghs}

    def resolve_value(h: int):
        """A hash value from texmod.def / a filename can live in either hash
        space. Returns ('gamehash', h), ('crc32', h), or ('unknown', h)."""
        if h in known_game_hashes:
            return "gamehash", h
        if h in crc_to_game or h in c2n:
            return "crc32", h
        return "unknown", h

    def emit_crc32(crc32: int, tex_rel: str, source: str) -> int:
        """Resolve a Texmod CRC32 to game hashes and emit mappings.
        Returns number of mappings added."""
        added = 0
        names = c2n.get(crc32)
        if names:
            for name in names:
                before = stats[source]
                add_mapping(name, tex_rel, source)
                added += stats[source] - before
            return added
        for gh in crc_to_game.get(crc32, []):
            before = stats[source]
            add_mapping(game_id_for(gh, g2n), tex_rel, source)
            added += stats[source] - before
        return added

    # First pass: texmod.def
    def_map = {}
    for name in zf.namelist():
        if name == "texmod.def":
            def_map = parse_texmod_def(zf.read(name).decode("ascii",
                                                            errors="replace"))
            print(f"texmod.def: {len(def_map)} game-hash mappings")
            break
    else:
        print("warning: no texmod.def found; relying on CRC32 cache / "
              "filename hashing")

    textures_dir = out_dir / "textures"
    textures_dir.mkdir(parents=True, exist_ok=True)

    mappings = []
    seen = set()
    stats = {"texmoddef": 0, "crc32cache": 0, "namehash": 0, "unresolved": 0}

    def add_mapping(game_id, tex_path, source):
        key = (game_id, tex_path)
        if key in seen:
            return
        seen.add(key)
        mappings.append({"gameId": game_id, "texturePath": tex_path})
        stats[source] += 1

    for info in zf.infolist():
        if info.filename == "texmod.def" or info.is_dir():
            continue

        data = zf.read(info)  # zipfile handles ZipCrypto via zf.pwd

        # Normalize the entry name the same way the C++ loader does
        entry_name = info.filename
        if entry_name.lower().startswith("speed.exe_"):
            entry_name = entry_name[len("SPEED.EXE_"):]

        out_name = Path(entry_name).name
        (textures_dir / out_name).write_bytes(data)
        tex_rel = out_name

        # 1) texmod.def mapping - column 1 can be EITHER a game hash OR a
        # Texmod CRC32 (many packs use CRC32|file with file named the same).
        def_val = def_map.get(entry_name)
        if def_val is None:
            def_val = def_map.get(info.filename)
        if def_val is not None:
            kind, h = resolve_value(def_val)
            if kind == "gamehash":
                add_mapping(game_id_for(h, g2n), tex_rel, "texmoddef")
                continue
            if kind == "crc32" and emit_crc32(h, tex_rel, "texmoddef"):
                continue
            # Unknown hash: if it equals the filename's own CRC32, treat as
            # CRC32 (packs whose def column duplicates the filename hash);
            # otherwise trust the def and use it as a game hash.
            fcrc, fhex = parse_crc32_from_filename(entry_name)
            if kind == "crc32" or (fhex and def_val == fcrc):
                stats["unresolved"] += 1
                print(f"warning: CRC32 0x{h:08X} for {info.filename} has no "
                      f"known game hash - skipped")
                continue
            add_mapping(game_id_for(h, g2n), tex_rel, "texmoddef")
            continue

        # 2) CRC32 from filename -> static cache / name table -> game hash(es)
        crc32, was_hex = parse_crc32_from_filename(entry_name)
        if was_hex and emit_crc32(crc32, tex_rel, "crc32cache"):
            continue

        # 3) Plain-string gameId (loader hashes it with the game's DJB)
        base = strip_prefixes(Path(entry_name).stem.strip())
        if not was_hex:
            add_mapping(base, tex_rel, "namehash")
        else:
            stats["unresolved"] += 1
            print(f"warning: no game hash for {info.filename} "
                  f"(CRC32 0x{crc32:08X} not in cache) - skipped")

    pack_info = {
        "rootDirectory": "textures",
        "textureMappings": mappings,
    }
    (out_dir / "TexturePackInfo.json").write_text(
        json.dumps(pack_info, indent=2), encoding="utf-8")

    print(f"Extracted {stats['texmoddef'] + stats['crc32cache'] + stats['namehash'] + stats['unresolved']} textures -> {out_dir}")
    print(f"Mappings: {len(mappings)} total "
          f"(texmod.def: {stats['texmoddef']}, "
          f"crc32 cache: {stats['crc32cache']}, "
          f"name hash: {stats['namehash']}, "
          f"unresolved: {stats['unresolved']})")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("tpf", type=Path, help="input .tpf file")
    ap.add_argument("-o", "--output", type=Path,
                    help="output pack directory (default: <tpf name> next to input)")
    ap.add_argument("--crc32-cache", type=Path,
                    default=Path(__file__).resolve().parent.parent
                            / "HashMaps" / "MW_CRC32Cache_Static.h",
                    help="path to MW_CRC32Cache_Static.h")
    ap.add_argument("--name-table", type=Path,
                    default=Path(__file__).resolve().parent.parent
                            / "HashMaps" / "MW_TextureNames.json",
                    help="path to MW_TextureNames.json "
                         "(from generate_texture_name_table.py)")
    args = ap.parse_args()

    out_dir = args.output or args.tpf.with_suffix("")
    crc_to_game = {}
    if args.crc32_cache.is_file():
        crc_to_game = load_crc32_cache(args.crc32_cache)
        print(f"Loaded {len(crc_to_game)} CRC32 entries from "
              f"{args.crc32_cache}")
    else:
        print(f"warning: CRC32 cache not found at {args.crc32_cache}; "
              f"texmod.def-only conversion")

    g2n, c2n = {}, {}
    if args.name_table.is_file():
        g2n, c2n = load_name_table(args.name_table)
        print(f"Loaded {len(g2n)} texture names from {args.name_table}")
    else:
        print(f"warning: name table not found at {args.name_table}; "
              f"gameIds will be hex hashes")

    sys.exit(convert(args.tpf, out_dir, crc_to_game, g2n, c2n))


if __name__ == "__main__":
    main()
