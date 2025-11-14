#pragma once

#include <windows.h>
#include <d3d9.h>
#include <string>
#include <cstdint>

namespace ngg {
namespace mw {

// ============================================================================
// Entry structure definitions
// ============================================================================

// Texture path entry (allocated from FastMem)
// Supports DUAL HASHING: both name-based (game) and CRC32-based (Texmod)
struct TexturePathEntry
{
    uint32_t hash;           // Primary hash (name-based OR CRC32-based)
    uint32_t crc32Hash;      // Secondary CRC32 hash (0 if not calculated yet)
    char* path;              // Allocated from FastMem
    TexturePathEntry* next;  // Collision chain
};

// Texture entry (allocated from FastMem)
struct TextureEntry
{
    uint32_t hash;
    IDirect3DTexture9* texture;
    TextureEntry* next;      // Collision chain
};

// Volume texture entry (allocated from FastMem)
struct VolumeTextureEntry
{
    uint32_t hash;
    IDirect3DVolumeTexture9* texture;
    VolumeTextureEntry* next; // Collision chain
};

// ============================================================================
// TextureHashTable - FastMem-based hash table for texture storage
// ============================================================================
// Thread-safe hash table using game's FastMem allocator for texture storage.
// Supports three types of entries:
// - TexturePathEntry: Maps hash -> file path (for deferred loading)
// - TextureEntry: Maps hash -> D3D texture (loaded textures)
// - VolumeTextureEntry: Maps hash -> D3D volume texture
//
// Thread Safety:
// - Uses per-bucket critical sections for fine-grained locking
// - Safe for concurrent reads/writes from multiple threads
//
// Memory Management:
// - All allocations use game's FastMem (never freed - OS cleans up)
// - Critical sections never deleted (OS cleans up on process exit)
// ============================================================================
class TextureHashTable
{
public:
    TextureHashTable();
    ~TextureHashTable();

    // Initialization and cleanup
    void Initialize();
    void Cleanup();

    // Texture path operations (hash -> file path mapping)
    void AddTexturePath(uint32_t hash, const std::string& path);
    const char* GetTexturePath(uint32_t hash);
    void SetCRC32Hash(uint32_t hash, uint32_t crc32Hash);
    int CountTexturePaths();

    // Texture operations (hash -> D3D texture mapping)
    void AddTexture(uint32_t hash, IDirect3DTexture9* texture);
    IDirect3DTexture9* GetTexture(uint32_t hash);

    // Volume texture operations
    void AddVolumeTexture(uint32_t hash, IDirect3DVolumeTexture9* texture);
    IDirect3DVolumeTexture9* GetVolumeTexture(uint32_t hash);

    // Iteration
    template<typename Func>
    void ForEachTexturePath(Func callback);

    template<typename Func>
    void ForEachTexture(Func callback);

    // Access to raw tables (for advanced operations like BuildSwapTable)
    TexturePathEntry** GetPathTable() { return m_pathTable; }
    TextureEntry** GetTextureTable() { return m_textureTable; }
    VolumeTextureEntry** GetVolumeTable() { return m_volumeTable; }
    
    static constexpr size_t GetTableSize() { return HASH_TABLE_SIZE; }

private:
    static constexpr size_t HASH_TABLE_SIZE = 1024;

    // Hash tables (allocated from FastMem)
    TexturePathEntry** m_pathTable;
    TextureEntry** m_textureTable;
    VolumeTextureEntry** m_volumeTable;

    // Critical sections for thread-safe access (one per bucket)
    CRITICAL_SECTION* m_pathTableLocks;
    CRITICAL_SECTION* m_textureTableLocks;
    CRITICAL_SECTION* m_volumeTableLocks;

    bool m_initialized;

    // Helper: Get bucket index from hash
    static size_t GetBucketIndex(uint32_t hash)
    {
        return hash % HASH_TABLE_SIZE;
    }
};

// ============================================================================
// Template implementations (must be in header)
// ============================================================================

template<typename Func>
void TextureHashTable::ForEachTexturePath(Func callback)
{
    if (!m_initialized || !m_pathTable)
        return;

    for (size_t bucket = 0; bucket < HASH_TABLE_SIZE; bucket++)
    {
        EnterCriticalSection(&m_pathTableLocks[bucket]);

        TexturePathEntry* entry = m_pathTable[bucket];
        while (entry)
        {
            callback(entry->hash, entry->path);
            entry = entry->next;
        }

        LeaveCriticalSection(&m_pathTableLocks[bucket]);
    }
}

template<typename Func>
void TextureHashTable::ForEachTexture(Func callback)
{
    if (!m_initialized || !m_textureTable)
        return;

    for (size_t bucket = 0; bucket < HASH_TABLE_SIZE; bucket++)
    {
        EnterCriticalSection(&m_textureTableLocks[bucket]);

        TextureEntry* entry = m_textureTable[bucket];
        while (entry)
        {
            callback(entry->hash, entry->texture);
            entry = entry->next;
        }

        LeaveCriticalSection(&m_textureTableLocks[bucket]);
    }
}

} // namespace mw
} // namespace ngg

