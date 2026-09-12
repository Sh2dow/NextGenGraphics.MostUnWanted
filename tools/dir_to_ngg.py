#!/usr/bin/env python3
"""
dir_to_ngg.py - Convert a folder of Texmod-extracted textures (files named
0x<CRC32>.dds, optionally with SPEED.EXE_ / speed_t_ prefixes) into a native
NextGenGraphics (NGG) texture pack for NFS Most Wanted.

Resolution order for each .dds file:
  1. CRC32 parsed from filename -> name table (MW_TextureNames.json)
     -> gameId = original texture name
  2. CRC32 -> static cache (MW_CRC32Cache_Static.h) -> gameId = "0x<gameHash>"
  3. Non-hex filename -> gameId = normalized name string (the loader hashes
     it with the game's DJB function itself)
  4. Unresolvable CRC32 -> gameId = "0x<CRC32>" with a warning (this only
     works at runtime if the game's CRC32 cache learns the mapping; prefer
     fixing the name table)

Writes <pack_root>/TexturePackInfo.json.

Usage:
  python dir_to_ngg.py "<pack_root>" [--textures-dir Textures]
      [--id ngg.mw.packs.foo] [--name Foo] [--author me]

Example:
  python dir_to_ngg.py "d:/Games/NFS_TOOLS/NextgenGraphics/Rad v3"
"""

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from tpf_to_ngg import (load_crc32_cache, load_name_table,
                        parse_crc32_from_filename, strip_prefixes)

REPO = Path(__file__).resolve().parent.parent

def rename_textures_to_gameid(textures_dir: Path, mappings: list[dict]) -> None:
    """Rename each mapped .dds to <gameId>.dds and update texturePath.

    gameId is sanitized for Windows (<>:"/\|?* stripped, reserved names
    avoided). Collisions get a numeric suffix. Files that fail to rename
    keep their original texturePath.
    """
    import re

    def sanitize(name: str) -> str:
        # strip path-unsafe chars
        name = re.sub(r'[<>:"/\\|?*\x00-\x1f]', "_", name)
        # avoid Windows reserved names / trailing dots & spaces
        reserved = {"CON","PRN","AUX","NUL","COM1","COM2","COM3","COM4","COM5",
                    "COM6","COM7","COM8","COM9","LPT1","LPT2","LPT3","LPT4",
                    "LPT5","LPT6","LPT7","LPT8","LPT9"}
        if name.upper() in reserved:
            name = f"_{name}"
        return name.rstrip(". ") or "_"

    used: set[str] = set()
    for m in mappings:
        rel = m["texturePath"]
        src = textures_dir / rel
        if not src.is_file():
            continue

        base = sanitize(m["gameId"])
        candidate = f"{base}.dds"
        n = 1
        while candidate in used or (textures_dir / candidate).exists() \
                and (textures_dir / candidate) != src:
            candidate = f"{base}_{n}.dds"
            n += 1

        dst = textures_dir / candidate
        if src != dst:
            try:
                src.rename(dst)
            except OSError as e:
                print(f"warning: rename failed for {rel}: {e}",
                      file=sys.stderr)
                continue

        used.add(candidate)
        m["texturePath"] = candidate

def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pack_root", type=Path,
                    help="pack root folder (TexturePackInfo.json goes here)")
    ap.add_argument("--textures-dir", default="Textures",
                    help="subfolder with .dds files, relative to pack root "
                         "(default: Textures)")
    ap.add_argument("--id", default=None, help="pack id metadata")
    ap.add_argument("--name", default=None, help="pack display name")
    ap.add_argument("--author", default=None, help="pack author")
    ap.add_argument("--game", default="NFS Most Wanted")
    ap.add_argument("--crc32-cache", type=Path,
                    default=REPO / "HashMaps" / "MW_CRC32Cache_Static.h")
    ap.add_argument("--name-table", type=Path,
                    default=REPO / "HashMaps" / "MW_TextureNames.json")
    args = ap.parse_args()

    textures_dir = args.pack_root / args.textures_dir
    if not textures_dir.is_dir():
        print(f"error: {textures_dir} is not a directory", file=sys.stderr)
        return 1

    crc_to_game = load_crc32_cache(args.crc32_cache) \
        if args.crc32_cache.is_file() else {}
    g2n, c2n = load_name_table(args.name_table) \
        if args.name_table.is_file() else ({}, {})
    print(f"cache: {len(crc_to_game)} CRC32 entries, "
          f"name table: {len(g2n)} names")

    mappings = []
    seen = set()
    stats = {"name": 0, "cache": 0, "plain": 0, "unresolved": 0}

    def add(game_id, rel):
        if (game_id, rel) in seen:
            return
        seen.add((game_id, rel))
        mappings.append({"gameId": game_id, "texturePath": rel})

    files = sorted(textures_dir.rglob("*.dds"))
    for p in files:
        rel = p.relative_to(textures_dir).as_posix()
        crc32, was_hex = parse_crc32_from_filename(p.name)

        if was_hex:
            names = c2n.get(crc32)
            if names:
                for name in names:
                    add(name, rel)
                stats["name"] += 1
                continue
            ghs = crc_to_game.get(crc32)
            if ghs:
                for gh in ghs:
                    add(g2n.get(gh, f"0x{gh:08X}"), rel)
                stats["cache"] += 1
                continue
            # Unknown CRC32: emit as-is (runtime may still bridge it) + warn
            add(f"0x{crc32:08X}", rel)
            stats["unresolved"] += 1
            print(f"warning: {p.name}: CRC32 0x{crc32:08X} not in name table "
                  f"or static cache")
        else:
            base = strip_prefixes(p.stem.strip())
            add(base, rel)
            stats["plain"] += 1

    rename_textures_to_gameid(textures_dir, mappings)
    
        pack_info = {}
    pack_info["description"] = {
        "id": args.id or f"ngg.mw.packs.{args.pack_root.name.lower()}" or "ngg.mw.packs.test",
        "game": args.game or "test",
        "name": args.name or args.pack_root.name or "test",
        "description": args.name or args.pack_root.name or "test",
        "author": args.author or "test",
    }
    pack_info["rootDirectory"] = args.textures_dir
    pack_info["textureMappings"] = mappings

    out = args.pack_root / "TexturePackInfo.json"
    out.write_text(json.dumps(pack_info, indent=2), encoding="utf-8")

    print(f"{len(files)} textures -> {out}")
    print(f"Mappings: {len(mappings)} total "
          f"(by name: {stats['name']}, via cache: {stats['cache']}, "
          f"plain name: {stats['plain']}, unresolved: {stats['unresolved']})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
