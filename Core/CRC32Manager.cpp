#include "../stdafx.h"
#include "CRC32Manager.h"
#include "../Log.h"
#include "../includes/json/include/nlohmann/json.hpp"
#include <fstream>

// Optional STATIC CRC32 cache (compile-time), like GameTextureHashes.h
// If present, we prefer it over the JSON file at runtime.
#if defined(__has_include)
#if __has_include("../HashMaps/MW_CRC32Cache_Static.h")
#include "../HashMaps/MW_CRC32Cache_Static.h"
#define NGG_HAVE_STATIC_CRC32_CACHE 1
#endif
#endif
#ifndef NGG_HAVE_STATIC_CRC32_CACHE
#  define NGG_HAVE_STATIC_CRC32_CACHE 0
#endif

using json = nlohmann::json;

namespace ngg {
namespace mw {

// CRC-32 Polynomial (same as Texmod)
#define CRC32POLY 0xEDB88320u

// ============================================================================
// CRC32Manager implementation
// ============================================================================

CRC32Manager::CRC32Manager()
    : m_crc32ToGameHash(nullptr)
    , m_gameHashToCRC32(nullptr)
    , m_initialized(false)
    , m_cacheDirty(false)
{
    // Initialize lock early to satisfy static analysis and ensure safety even if Initialize() isn't called
    InitializeCriticalSection(&m_lock);
    m_lockInitialized = true;
}

CRC32Manager::~CRC32Manager()
{
    if (m_lockInitialized)
    {
        DeleteCriticalSection(&m_lock);
        m_lockInitialized = false;
    }
}

void CRC32Manager::Initialize()
{
    if (m_initialized)
        return;

    // Ensure lock is initialized (constructor already does this, but guard just in case)
    if (!m_lockInitialized) {
        InitializeCriticalSection(&m_lock);
        m_lockInitialized = true;
    }

    m_crc32ToGameHash = new std::unordered_map<uint32_t, std::vector<uint32_t>>();
    m_gameHashToCRC32 = new std::unordered_map<uint32_t, uint32_t>();

    m_initialized = true;
    asi_log::Log("CRC32Manager: Initialized");
}

void CRC32Manager::LoadCache()
{
    if (!m_initialized)
        return;

#if NGG_HAVE_STATIC_CRC32_CACHE
    // Load from compiled static header (like GameTextureHashes.h)
    EnterCriticalSection(&m_lock);
    m_crc32ToGameHash->clear();
    m_gameHashToCRC32->clear();
    for (size_t i = 0; i < ngg::mw::kGameToCrc32PairsCount; ++i)
    {
        uint32_t gameHash = ngg::mw::kGameToCrc32Pairs[i].first;
        uint32_t crc32    = ngg::mw::kGameToCrc32Pairs[i].second;

        auto& gameHashes = (*m_crc32ToGameHash)[crc32];
        if (std::find(gameHashes.begin(), gameHashes.end(), gameHash) == gameHashes.end())
            gameHashes.push_back(gameHash);

        (*m_gameHashToCRC32)[gameHash] = crc32;
    }
    LeaveCriticalSection(&m_lock);

    asi_log::Log("CRC32Manager: Loaded STATIC cache - %zu CRC32 mappings, %zu game hash mappings",
                 m_crc32ToGameHash->size(), m_gameHashToCRC32->size());
    return;
#endif

    const char* cacheFile = "Resources/MW_CRC32Cache.json";

    std::ifstream file(cacheFile);
    if (!file.is_open())
    {
        asi_log::Log("CRC32Manager: No cache file found - will build from scratch");
        return;
    }

    try
    {
        json j;
        file >> j;
        file.close();

        EnterCriticalSection(&m_lock);

        // Check if it's the NEW format (with crc32_to_game and game_to_crc32)
        if (j.contains("crc32_to_game") && j.contains("game_to_crc32"))
        {
            // Load CRC32 -> GameHash mappings
            for (auto& [crc32Str, gameHashArray] : j["crc32_to_game"].items())
            {
                uint32_t crc32 = std::stoul(crc32Str);
                std::vector<uint32_t> gameHashes;

                for (auto& hashVal : gameHashArray)
                {
                    gameHashes.push_back(hashVal.get<uint32_t>());
                }

                (*m_crc32ToGameHash)[crc32] = gameHashes;
            }

            // Load GameHash -> CRC32 mappings
            for (auto& [gameHashStr, crc32Val] : j["game_to_crc32"].items())
            {
                uint32_t gameHash = std::stoul(gameHashStr);
                uint32_t crc32 = crc32Val.get<uint32_t>();
                (*m_gameHashToCRC32)[gameHash] = crc32;
            }
        }
        else
        {
            // OLD format: flat map of "CRC32_hex" -> gameHash_decimal
            // Convert to new format
            for (auto& [crc32HexStr, gameHashVal] : j.items())
            {
                // Parse CRC32 from hex string
                uint32_t crc32 = std::stoul(crc32HexStr, nullptr, 16);
                uint32_t gameHash = gameHashVal.get<uint32_t>();

                // Add to CRC32 -> GameHash map (one CRC32 can map to multiple game hashes)
                auto& gameHashes = (*m_crc32ToGameHash)[crc32];
                if (std::find(gameHashes.begin(), gameHashes.end(), gameHash) == gameHashes.end())
                {
                    gameHashes.push_back(gameHash);
                }

                // Add to GameHash -> CRC32 map (one game hash maps to one CRC32)
                (*m_gameHashToCRC32)[gameHash] = crc32;
            }
        }

        LeaveCriticalSection(&m_lock);

        asi_log::Log("CRC32Manager: Loaded cache - %zu CRC32 mappings, %zu game hash mappings",
                     m_crc32ToGameHash->size(), m_gameHashToCRC32->size());
    }
    catch (const std::exception& e)
    {
        asi_log::Log("CRC32Manager: Failed to load cache: %s", e.what());
    }
}

void CRC32Manager::SaveCache()
{
    // Deprecated: static mapping only; no file writes
    if (!m_initialized)
        return;

    if (m_cacheDirty)
    {
        asi_log::Log("CRC32Manager: SaveCache() is deprecated - static mapping only");
        m_cacheDirty = false;
    }
}

uint32_t CRC32Manager::GetCRC32(const char* data, uint32_t length)
{
    uint32_t crc = 0xFFFFFFFF;
    for (uint32_t idx = 0; idx < length; idx++)
    {
        uint32_t byte = static_cast<unsigned char>(*data++);
        for (uint32_t bit = 0; bit < 8; bit++, byte >>= 1)
        {
            crc = (crc >> 1) ^ (((crc ^ byte) & 1) ? CRC32POLY : 0);
        }
    }
    return crc;
}

int CRC32Manager::GetBitsFromFormat(D3DFORMAT format)
{
    switch (format)
    {
        case D3DFMT_R8G8B8:      return 24;
        case D3DFMT_A8R8G8B8:    return 32;
        case D3DFMT_X8R8G8B8:    return 32;
        case D3DFMT_R5G6B5:      return 16;
        case D3DFMT_X1R5G5B5:    return 16;
        case D3DFMT_A1R5G5B5:    return 16;
        case D3DFMT_A4R4G4B4:    return 16;
        case D3DFMT_R3G3B2:      return 8;
        case D3DFMT_A8:          return 8;
        case D3DFMT_A8R3G3B2:    return 16;
        case D3DFMT_X4R4G4B4:    return 16;
        case D3DFMT_A2B10G10R10: return 32;
        case D3DFMT_A8B8G8R8:    return 32;
        case D3DFMT_X8B8G8R8:    return 32;
        case D3DFMT_G16R16:      return 32;
        case D3DFMT_A2R10G10B10: return 32;
        case D3DFMT_A16B16G16R16:return 64;
        case D3DFMT_A8P8:        return 16;
        case D3DFMT_P8:          return 8;
        case D3DFMT_L8:          return 8;
        case D3DFMT_A8L8:        return 16;
        case D3DFMT_A4L4:        return 8;
        case D3DFMT_L16:         return 16;
        case D3DFMT_DXT1:        return 4;  // Compressed: 4 bits per pixel
        case D3DFMT_DXT2:        return 8;  // Compressed: 8 bits per pixel
        case D3DFMT_DXT3:        return 8;  // Compressed: 8 bits per pixel
        case D3DFMT_DXT4:        return 8;  // Compressed: 8 bits per pixel
        case D3DFMT_DXT5:        return 8;  // Compressed: 8 bits per pixel
        default:                 return 32; // Default to 32-bit
    }
}

uint32_t CRC32Manager::CalculateTexmodHash(IDirect3DTexture9* pTexture)
{
    if (!pTexture)
        return 0;

    D3DSURFACE_DESC desc;
    if (FAILED(pTexture->GetLevelDesc(0, &desc)))
        return 0;

    D3DLOCKED_RECT d3dlr;
    if (FAILED(pTexture->LockRect(0, &d3dlr, NULL, D3DLOCK_READONLY)))
        return 0;

    // Calculate size (same as Texmod - may be incorrect for some formats due to pitch)
    int size = (GetBitsFromFormat(desc.Format) * desc.Width * desc.Height) / 8;
    uint32_t hash = GetCRC32(static_cast<const char*>(d3dlr.pBits), size);

    pTexture->UnlockRect(0);
    return hash;
}

uint32_t CRC32Manager::GetGameHashByCRC32(uint32_t crc32Hash)
{
    if (!m_initialized)
        return 0;

    EnterCriticalSection(&m_lock);

    auto it = m_crc32ToGameHash->find(crc32Hash);
    if (it != m_crc32ToGameHash->end() && !it->second.empty())
    {
        // Return first game hash (most common case)
        uint32_t gameHash = it->second[0];
        LeaveCriticalSection(&m_lock);
        return gameHash;
    }

    LeaveCriticalSection(&m_lock);
    return 0;
}

std::vector<uint32_t> CRC32Manager::GetAllGameHashesByCRC32(uint32_t crc32Hash)
{
    if (!m_initialized)
        return std::vector<uint32_t>();

    EnterCriticalSection(&m_lock);

    auto it = m_crc32ToGameHash->find(crc32Hash);
    if (it != m_crc32ToGameHash->end())
    {
        // Return copy of all game hashes for this CRC32
        std::vector<uint32_t> gameHashes = it->second;
        LeaveCriticalSection(&m_lock);
        return gameHashes;
    }

    LeaveCriticalSection(&m_lock);
    return std::vector<uint32_t>();
}

uint32_t CRC32Manager::GetCRC32ByGameHash(uint32_t gameHash)
{
    if (!m_initialized)
        return 0;

    EnterCriticalSection(&m_lock);

    auto it = m_gameHashToCRC32->find(gameHash);
    if (it != m_gameHashToCRC32->end())
    {
        uint32_t crc32 = it->second;
        LeaveCriticalSection(&m_lock);
        return crc32;
    }

    LeaveCriticalSection(&m_lock);
    return 0;
}

void CRC32Manager::AddMapping(uint32_t crc32Hash, uint32_t gameHash)
{
    if (!m_initialized)
        return;

    EnterCriticalSection(&m_lock);

    // Add to CRC32 -> GameHash map (one CRC32 can map to multiple game hashes)
    auto& gameHashes = (*m_crc32ToGameHash)[crc32Hash];
    if (std::find(gameHashes.begin(), gameHashes.end(), gameHash) == gameHashes.end())
    {
        gameHashes.push_back(gameHash);
        m_cacheDirty = true;
    }

    // Add to GameHash -> CRC32 map (one game hash maps to one CRC32)
    if (m_gameHashToCRC32->find(gameHash) == m_gameHashToCRC32->end())
    {
        (*m_gameHashToCRC32)[gameHash] = crc32Hash;
        m_cacheDirty = true;
    }

    LeaveCriticalSection(&m_lock);
}

} // namespace mw
} // namespace ngg

