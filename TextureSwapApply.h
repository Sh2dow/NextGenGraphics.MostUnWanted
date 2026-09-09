#pragma once
#include <windows.h>
#include <unordered_map>
#include <atomic>


#include <d3dx9.h>

namespace ngg {

#ifdef GAME_MW
    namespace mw {
#elif GAME_CARBON
    namespace carbon {
#endif

// Sets a material texture parameter via the game's material API.
// - material: material object pointer from the game
// - paramName: parameter name (e.g., "DiffuseMap", "NormalMapTexture", "SPECULARMAPTEXTURE")
// - texture: the texture to set (must be non-null)
// - texPtrStorage: pointer to persistent storage where the game expects to read the texture pointer from
// - addRefTexture: whether to AddRef() the texture (true for our custom textures; false for the game's originals)
// Returns true on success.
bool SetMaterialTexture(void* material,
                        const char* paramName,
                        IDirect3DTexture9* texture,
                        IDirect3DTexture9** texPtrStorage,
                        bool addRefTexture);

// Forward declarations to avoid heavy includes
class TextureHashTable;
class CRC32Manager;

struct SwapContext {
    // Flags
    bool* pathsLoaded;                      // g_pathsLoaded

    // Swap table state
    std::atomic<bool>* swapTableBuilt;      // g_swapTableBuilt
    std::atomic<int>*  texturesLoaded;      // g_texturesLoaded

    // Tables and managers
    TextureHashTable* hashTable;            // g_hashTable
    CRC32Manager*     crc32Manager;         // g_crc32Manager

    // Swap table pointer and lock (double indirection allows atomic pointer swap by builder)
    std::unordered_map<uint32_t, IDirect3DTexture9*>** swapTablePtr; // &g_swapTable
    CRITICAL_SECTION* swapTableLock;                                // &g_swapTableLock

    std::unordered_map<uint32_t, uint32_t>*            gameHashToCRC32Map; // g_gameHashToCRC32Map
    CRITICAL_SECTION* crc32MapLock;         // &g_crc32MapLock

    // Counters
    uint32_t* swapCallCount;                // &g_swapCallCount
    uint32_t* swapSuccessCount;             // &g_swapSuccessCount
};

// Perform texture swapping using context-provided state
void SwapTextures(const SwapContext& ctx);


} // namespace mw
} // namespace ngg

