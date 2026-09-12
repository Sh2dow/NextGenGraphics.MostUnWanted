#!/usr/bin/env python3
"""
generate_texture_name_table.py - Build CRC32 <-> texture name tables for NFSMW.

Scans an extracted game texture dump (e.g. STREAML2RA.BUN export produced by
NFS-TexEd / similar tools, organized as <root>/<TPK*>/<NAME>.dds) and, for
each DDS, computes:

  - gameHash: the game's bStringHash (DJB variant, seed 0xFFFFFFFF) of the
    filename stem - this is what the runtime wrapper hash contains
  - crc32:    the Texmod content hash (CRC32 of level-0 pixel data, init
    0xFFFFFFFF, no final XOR - same as CRC32Manager::CalculateTexmodHash)

Outputs a JSON table:
  HashMaps/MW_TextureNames.json
    {
      "name_to_gamehash": { "ARC_FOO": 1298771504, ... },
      "gamehash_to_name": { "1298771504": "ARC_FOO", ... },
      "crc32_to_names":   { "305419896": ["ARC_FOO", ...], ... }
    }

Usage:
  python generate_texture_name_table.py <export_root> [-o out.json]
"""

import argparse
import json
import struct
import sys
import zlib
from pathlib import Path

FOURCC_BITS = {b"DXT1": 4, b"DXT2": 8, b"DXT3": 8, b"DXT4": 8, b"DXT5": 8}


def djb_game_hash(s: str) -> int:
    h = 0xFFFFFFFF
    for c in s.encode("ascii", errors="replace"):
        h = (h * 33 + c) & 0xFFFFFFFF
    return h


def texmod_crc32(path: Path):
    """Returns (crc32, fourcc) or raises on unparsable file."""
    d = path.read_bytes()
    if len(d) < 128 or d[:4] != b"DDS ":
        raise ValueError("not a DDS")
    height, width = struct.unpack_from("<II", d, 12)
    fourcc = d[84:88]
    pitch_or_linear, bpp = struct.unpack_from("<II", d, 76 + 12)  # ddspf size fields below
    bpp = struct.unpack_from("<I", d, 88)[0]
    if fourcc in FOURCC_BITS:
        bits = FOURCC_BITS[fourcc]
    elif fourcc == b"\0\0\0\0":
        bits = bpp
    else:
        bits = 32
    size = (bits * width * height) // 8
    data = d[128:128 + size]
    if len(data) < size:
        raise ValueError("truncated level 0")
    # CRC32Manager::GetCRC32 == standard crc32 without the final XOR
    return zlib.crc32(data) ^ 0xFFFFFFFF, fourcc.decode("ascii", "replace").strip("\0")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("export_root", type=Path,
                    help=r"e.g. D:\Games\NFS MW - Vanilla\TRACKS\Export\TRACKS\STREAML2RA.BUN")
    ap.add_argument("-o", "--output", type=Path,
                    default=Path(__file__).resolve().parent.parent
                            / "HashMaps" / "MW_TextureNames.json")
    args = ap.parse_args()

    files = sorted(args.export_root.rglob("*.dds"))
    if not files:
        print(f"error: no .dds files under {args.export_root}", file=sys.stderr)
        return 1

    name_to_gamehash = {}
    crc32_to_names = {}
    errors = 0
    for i, p in enumerate(files):
        name = p.stem
        try:
            crc, _cc = texmod_crc32(p)
        except Exception as e:
            errors += 1
            continue
        name_to_gamehash[name] = djb_game_hash(name)
        crc32_to_names.setdefault(crc, [])
        if name not in crc32_to_names[crc]:
            crc32_to_names[crc].append(name)
        if (i + 1) % 500 == 0:
            print(f"  {i + 1}/{len(files)}...")

    table = {
        "name_to_gamehash": name_to_gamehash,
        "gamehash_to_name": {str(v): k for k, v in name_to_gamehash.items()},
        "crc32_to_names": {str(k): v for k, v in crc32_to_names.items()},
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(table, indent=1), encoding="utf-8")

    print(f"scanned {len(files)} files ({errors} skipped)")
    print(f"names: {len(name_to_gamehash)}, unique CRC32: {len(crc32_to_names)}")
    print(f"wrote {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
