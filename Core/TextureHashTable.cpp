#include "../stdafx.h"
#include "TextureHashTable.h"
#include "../Log.h"
#include <cstring>

namespace ngg {
namespace mw {

// ============================================================================
// FastMem integration
// ============================================================================

#define FASTMEM_INSTANCE 0x925B30
typedef void* (__thiscall* FastMem_Alloc_t)(DWORD*, size_t, const char*);
static FastMem_Alloc_t FastMem_Alloc = (FastMem_Alloc_t)0x5D29D0;

// Helper: Allocate from game's FastMem
static void* AllocFromFastMem(size_t size, const char* kind)
{
    DWORD* fastMemInstance = reinterpret_cast<DWORD*>(FASTMEM_INSTANCE);
    return FastMem_Alloc(fastMemInstance, size, kind);
}

// ============================================================================
// TextureHashTable implementation
// ============================================================================

TextureHashTable::TextureHashTable()
    : m_pathTable(nullptr)
    , m_textureTable(nullptr)
    , m_volumeTable(nullptr)
    , m_pathTableLocks(nullptr)
    , m_textureTableLocks(nullptr)
    , m_volumeTableLocks(nullptr)
    , m_initialized(false)
{
}

TextureHashTable::~TextureHashTable()
{
    // Note: We don't cleanup here because FastMem allocations are never freed
    // and critical sections are never deleted (OS cleans up on process exit)
}

void TextureHashTable::Initialize()
{
    if (m_initialized)
        return;

    asi_log::Log("TextureHashTable: Initializing FastMem hash tables...");

    // Allocate path table
    m_pathTable = static_cast<TexturePathEntry**>(
        AllocFromFastMem(sizeof(TexturePathEntry*) * HASH_TABLE_SIZE, "TexturePathTable"));
    memset(m_pathTable, 0, sizeof(TexturePathEntry*) * HASH_TABLE_SIZE);

    // Allocate texture table
    m_textureTable = static_cast<TextureEntry**>(
        AllocFromFastMem(sizeof(TextureEntry*) * HASH_TABLE_SIZE, "TextureTable"));
    memset(m_textureTable, 0, sizeof(TextureEntry*) * HASH_TABLE_SIZE);

    // Allocate volume texture table
    m_volumeTable = static_cast<VolumeTextureEntry**>(
        AllocFromFastMem(sizeof(VolumeTextureEntry*) * HASH_TABLE_SIZE, "VolumeTextureTable"));
    memset(m_volumeTable, 0, sizeof(VolumeTextureEntry*) * HASH_TABLE_SIZE);

    // Allocate and initialize critical sections (one per bucket)
    m_pathTableLocks = static_cast<CRITICAL_SECTION*>(
        AllocFromFastMem(sizeof(CRITICAL_SECTION) * HASH_TABLE_SIZE, "PathTableLocks"));
    m_textureTableLocks = static_cast<CRITICAL_SECTION*>(
        AllocFromFastMem(sizeof(CRITICAL_SECTION) * HASH_TABLE_SIZE, "TextureTableLocks"));
    m_volumeTableLocks = static_cast<CRITICAL_SECTION*>(
        AllocFromFastMem(sizeof(CRITICAL_SECTION) * HASH_TABLE_SIZE, "VolumeTableLocks"));

    for (size_t i = 0; i < HASH_TABLE_SIZE; i++)
    {
        InitializeCriticalSection(&m_pathTableLocks[i]);
        InitializeCriticalSection(&m_textureTableLocks[i]);
        InitializeCriticalSection(&m_volumeTableLocks[i]);
    }

    m_initialized = true;
    asi_log::Log("TextureHashTable: Initialized with %zu buckets", HASH_TABLE_SIZE);
}

void TextureHashTable::Cleanup()
{
    if (!m_initialized)
        return;

    asi_log::Log("TextureHashTable: Cleaning up hash tables...");

    // Release all textures
    if (m_textureTable)
    {
        for (size_t bucket = 0; bucket < HASH_TABLE_SIZE; bucket++)
        {
            EnterCriticalSection(&m_textureTableLocks[bucket]);

            TextureEntry* entry = m_textureTable[bucket];
            while (entry)
            {
                if (entry->texture)
                {
                    entry->texture->Release();
                    entry->texture = nullptr;
                }
                entry = entry->next;
            }

            LeaveCriticalSection(&m_textureTableLocks[bucket]);
        }
    }

    // Release all volume textures
    if (m_volumeTable)
    {
        for (size_t bucket = 0; bucket < HASH_TABLE_SIZE; bucket++)
        {
            EnterCriticalSection(&m_volumeTableLocks[bucket]);

            VolumeTextureEntry* entry = m_volumeTable[bucket];
            while (entry)
            {
                if (entry->texture)
                {
                    entry->texture->Release();
                    entry->texture = nullptr;
                }
                entry = entry->next;
            }

            LeaveCriticalSection(&m_volumeTableLocks[bucket]);
        }
    }

    // Note: We don't delete critical sections or free FastMem allocations
    // OS will clean them up on process exit

    asi_log::Log("TextureHashTable: Cleanup complete");
}

void TextureHashTable::AddTexturePath(uint32_t hash, const std::string& path)
{
    if (!m_initialized)
        return;

    size_t bucket = GetBucketIndex(hash);
    EnterCriticalSection(&m_pathTableLocks[bucket]);

    // CRITICAL: DO NOT check if already exists!
    // The original ASI always adds a new entry to the front of the list.
    // This matches the original behavior exactly.

    // Allocate new entry from FastMem
    TexturePathEntry* newEntry = static_cast<TexturePathEntry*>(
        AllocFromFastMem(sizeof(TexturePathEntry), "TexturePathEntry"));

    newEntry->hash = hash;
    newEntry->crc32Hash = 0; // Will be calculated later if needed

    // Allocate path string from FastMem
    size_t pathLen = path.length() + 1;
    newEntry->path = static_cast<char*>(AllocFromFastMem(pathLen, "TexturePath"));
    strcpy_s(newEntry->path, pathLen, path.c_str());

    // Insert at head of bucket (matches original ASI behavior)
    newEntry->next = m_pathTable[bucket];
    m_pathTable[bucket] = newEntry;

    LeaveCriticalSection(&m_pathTableLocks[bucket]);
}

const char* TextureHashTable::GetTexturePath(uint32_t hash)
{
    if (!m_initialized)
        return nullptr;

    size_t bucket = GetBucketIndex(hash);
    EnterCriticalSection(&m_pathTableLocks[bucket]);

    TexturePathEntry* entry = m_pathTable[bucket];
    while (entry)
    {
        if (entry->hash == hash)
        {
            const char* path = entry->path;
            LeaveCriticalSection(&m_pathTableLocks[bucket]);
            return path;
        }
        entry = entry->next;
    }

    LeaveCriticalSection(&m_pathTableLocks[bucket]);
    return nullptr;
}

void TextureHashTable::SetCRC32Hash(uint32_t hash, uint32_t crc32Hash)
{
    if (!m_initialized)
        return;

    size_t bucket = GetBucketIndex(hash);
    EnterCriticalSection(&m_pathTableLocks[bucket]);

    TexturePathEntry* entry = m_pathTable[bucket];
    while (entry)
    {
        if (entry->hash == hash)
        {
            entry->crc32Hash = crc32Hash;
            LeaveCriticalSection(&m_pathTableLocks[bucket]);
            return;
        }
        entry = entry->next;
    }

    LeaveCriticalSection(&m_pathTableLocks[bucket]);
}

int TextureHashTable::CountTexturePaths()
{
    if (!m_initialized)
        return 0;

    int count = 0;
    for (size_t bucket = 0; bucket < HASH_TABLE_SIZE; bucket++)
    {
        EnterCriticalSection(&m_pathTableLocks[bucket]);

        TexturePathEntry* entry = m_pathTable[bucket];
        while (entry)
        {
            count++;
            entry = entry->next;
        }

        LeaveCriticalSection(&m_pathTableLocks[bucket]);
    }

    return count;
}

void TextureHashTable::AddTexture(uint32_t hash, IDirect3DTexture9* texture)
{
    if (!m_initialized || !texture)
        return;

    size_t bucket = GetBucketIndex(hash);
    EnterCriticalSection(&m_textureTableLocks[bucket]);

    // CRITICAL: DO NOT check if already exists!
    // The original ASI always adds a new entry to the front of the list.
    // This allows multiple textures with the same hash (collision chain).
    // Checking and replacing would cause textures to be released prematurely!

    // Allocate new entry from FastMem
    TextureEntry* newEntry = static_cast<TextureEntry*>(
        AllocFromFastMem(sizeof(TextureEntry), "TextureEntry"));

    newEntry->hash = hash;
    newEntry->texture = texture;
    // CRITICAL: Call AddRef() to keep the texture alive!
    // The hash table owns a reference to prevent textures from being released by D3D9.
    // Without this, textures can be destroyed when the game releases them, causing crashes.
    texture->AddRef();

    // Insert at head of bucket (matches original ASI behavior)
    newEntry->next = m_textureTable[bucket];
    m_textureTable[bucket] = newEntry;

    LeaveCriticalSection(&m_textureTableLocks[bucket]);
}

IDirect3DTexture9* TextureHashTable::GetTexture(uint32_t hash)
{
    if (!m_initialized)
        return nullptr;

    size_t bucket = GetBucketIndex(hash);
    EnterCriticalSection(&m_textureTableLocks[bucket]);

    TextureEntry* entry = m_textureTable[bucket];
    while (entry)
    {
        if (entry->hash == hash)
        {
            IDirect3DTexture9* texture = entry->texture;
            LeaveCriticalSection(&m_textureTableLocks[bucket]);
            return texture;
        }
        entry = entry->next;
    }

    LeaveCriticalSection(&m_textureTableLocks[bucket]);
    return nullptr;
}

void TextureHashTable::AddVolumeTexture(uint32_t hash, IDirect3DVolumeTexture9* texture)
{
    if (!m_initialized || !texture)
        return;

    size_t bucket = GetBucketIndex(hash);
    EnterCriticalSection(&m_volumeTableLocks[bucket]);

    // CRITICAL: DO NOT check if already exists!
    // The original ASI always adds a new entry to the front of the list.
    // This allows multiple textures with the same hash (collision chain).
    // Checking and replacing would cause textures to be released prematurely!

    // Allocate new entry from FastMem
    VolumeTextureEntry* newEntry = static_cast<VolumeTextureEntry*>(
        AllocFromFastMem(sizeof(VolumeTextureEntry), "VolumeTextureEntry"));

    newEntry->hash = hash;
    newEntry->texture = texture;
    // CRITICAL: Call AddRef() to keep the texture alive!
    // The hash table owns a reference to prevent textures from being released by D3D9.
    // Without this, textures can be destroyed when the game releases them, causing crashes.
    texture->AddRef();

    // Insert at head of bucket (matches original ASI behavior)
    newEntry->next = m_volumeTable[bucket];
    m_volumeTable[bucket] = newEntry;

    LeaveCriticalSection(&m_volumeTableLocks[bucket]);
}

IDirect3DVolumeTexture9* TextureHashTable::GetVolumeTexture(uint32_t hash)
{
    if (!m_initialized)
        return nullptr;

    size_t bucket = GetBucketIndex(hash);
    EnterCriticalSection(&m_volumeTableLocks[bucket]);

    VolumeTextureEntry* entry = m_volumeTable[bucket];
    while (entry)
    {
        if (entry->hash == hash)
        {
            IDirect3DVolumeTexture9* texture = entry->texture;
            LeaveCriticalSection(&m_volumeTableLocks[bucket]);
            return texture;
        }
        entry = entry->next;
    }

    LeaveCriticalSection(&m_volumeTableLocks[bucket]);
    return nullptr;
}

} // namespace mw
} // namespace ngg

