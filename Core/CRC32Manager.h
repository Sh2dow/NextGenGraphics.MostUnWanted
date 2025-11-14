#pragma once

#include <windows.h>
#include <d3d9.h>
#include <unordered_map>
#include <vector>
#include <cstdint>

namespace ngg {
namespace mw {

// ============================================================================
// CRC32Manager - Texmod compatibility layer
// ============================================================================
// Manages CRC32-based texture hashing for Texmod compatibility.
//
// Background:
// - Game uses name-based hashing (DJB variant): hash = DJB("texture_name")
// - Texmod uses CRC32 of pixel data: hash = CRC32(pixel_data)
// - These are completely different hash spaces!
//
// Solution:
// - Build a mapping cache: CRC32 <-> Game Hash
// - Cache is saved to file for reuse across sessions
// - One CRC32 can map to MULTIPLE game hashes (same texture reused)
//
// Thread Safety:
// - All operations are protected by a critical section
// - Safe for concurrent access from multiple threads
// ============================================================================
class CRC32Manager
{
public:
    CRC32Manager();
    ~CRC32Manager();

    // Initialization
    void Initialize();

    // Cache management
    void LoadCache();
    // Deprecated: static mapping only; no file writes
    void SaveCache();
    bool IsCacheDirty() const { return m_cacheDirty; }

    // CRC32 calculation
    uint32_t CalculateTexmodHash(IDirect3DTexture9* texture);

    // Mapping operations
    uint32_t GetGameHashByCRC32(uint32_t crc32Hash);
    std::vector<uint32_t> GetAllGameHashesByCRC32(uint32_t crc32Hash);
    uint32_t GetCRC32ByGameHash(uint32_t gameHash);
    void AddMapping(uint32_t crc32Hash, uint32_t gameHash);

    // Raw map accessors (read-only use by higher-level systems)
    // NOTE: The maps are owned for the lifetime of the process; do not modify them directly.
    std::unordered_map<uint32_t, std::vector<uint32_t>>* GetCrc32ToGameMapPtr() { return m_crc32ToGameHash; }
    std::unordered_map<uint32_t, uint32_t>* GetGameToCrc32MapPtr() { return m_gameHashToCRC32; }

private:
    // CRC32 calculation helpers
    static uint32_t GetCRC32(const char* data, uint32_t length);
    static int GetBitsFromFormat(D3DFORMAT format);

    // Mapping tables
    // One CRC32 can map to MULTIPLE game hashes (same texture reused for different materials)
    std::unordered_map<uint32_t, std::vector<uint32_t>>* m_crc32ToGameHash;  // CRC32 -> [GameHash1, GameHash2, ...]
    std::unordered_map<uint32_t, uint32_t>* m_gameHashToCRC32;  // GameHash -> CRC32 (reverse lookup)

    CRITICAL_SECTION m_lock;
    bool m_lockInitialized = false;  // Tracks whether m_lock has been initialized
    bool m_initialized;
    bool m_cacheDirty;  // True if cache needs to be saved
};

} // namespace mw
} // namespace ngg

