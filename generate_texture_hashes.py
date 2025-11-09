#!/usr/bin/env python3
"""
Generate a static hash map of all game textures from TRACKS_Extracted_tree.log
This creates a C++ header file with compile-time hash validation.
"""

import re

def calc_hash(name):
    """
    Calculate FNV-1a style hash (actually DJB variant) used by the game.
    This matches the game's hash function at 0x460BF0.
    """
    hash_val = 0xFFFFFFFF
    
    for char in name.upper():
        hash_val = ((hash_val * 0x21) + ord(char)) & 0xFFFFFFFF
    
    return hash_val

def main():
    # Read the log file
    with open('disasm_carbon/TRACKS_Extracted_tree.log', 'r') as f:
        lines = f.readlines()
    
    # Extract all .dds filenames
    textures = set()
    for line in lines:
        line = line.strip()
        if line.endswith('.dds'):
            # Remove path separators and get just the filename
            filename = line.split('|')[-1].strip()
            if filename.endswith('.dds'):
                # Remove extension
                name_without_ext = filename[:-4]
                textures.add(name_without_ext)
    
    # Calculate hashes
    hash_map = {}
    for texture in sorted(textures):
        hash_val = calc_hash(texture)
        hash_map[hash_val] = texture
    
    print(f'Generating hash map for {len(hash_map)} textures...')
    
    # Generate C++ header file
    with open('GameTextureHashes.h', 'w') as f:
        f.write("""// Auto-generated file - DO NOT EDIT
// Generated from disasm_carbon/TRACKS_Extracted_tree.log
// Contains all valid game texture hashes for validation

#pragma once

#include <unordered_set>
#include <cstdint>

namespace ngg {
namespace carbon {

// Static set of all valid game texture hashes
// Use this to validate that a hash corresponds to a real game texture
inline const std::unordered_set<uint32_t> g_validGameTextureHashes = {
""")
        
        # Write hashes in groups of 8 per line
        hashes = sorted(hash_map.keys())
        for i in range(0, len(hashes), 8):
            chunk = hashes[i:i+8]
            line = "    " + ", ".join(f"0x{h:08X}" for h in chunk)
            if i + 8 < len(hashes):
                line += ","
            f.write(line + "\n")
        
        f.write("""};

// Total number of valid game textures
constexpr size_t TOTAL_GAME_TEXTURES = """ + str(len(hash_map)) + """;

// Check if a hash corresponds to a valid game texture
inline bool IsValidGameTextureHash(uint32_t hash) {
    return g_validGameTextureHashes.find(hash) != g_validGameTextureHashes.end();
}

} // namespace carbon
} // namespace ngg
""")
    
    print(f'Generated GameTextureHashes.h with {len(hash_map)} hashes')
    
    # Print some statistics
    print(f'\nStatistics:')
    print(f'  Total textures: {len(hash_map)}')
    print(f'  First hash: 0x{min(hashes):08X} ({hash_map[min(hashes)]})')
    print(f'  Last hash: 0x{max(hashes):08X} ({hash_map[max(hashes)]})')
    
    # Check for hash collisions
    if len(hash_map) != len(textures):
        print(f'\n  WARNING: Hash collisions detected!')
        print(f'  Unique textures: {len(textures)}')
        print(f'  Unique hashes: {len(hash_map)}')

if __name__ == '__main__':
    main()

