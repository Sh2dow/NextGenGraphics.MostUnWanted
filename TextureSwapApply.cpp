#include "stdafx.h"

#include <windows.h>
#include <unordered_map>
#include <unordered_set>
#include <atomic>

#include "Log.h"
#include "TextureSwapApply.h"
#include "Core/TextureHashTable.h"
#include "Core/CRC32Manager.h"

namespace ngg {

#ifdef GAME_MW
#include "NFSMW_PreFEngHook.h"
    namespace mw {
#elif GAME_CARBON
#include "NFSC_PreFEngHook.h"
    namespace carbon {
#endif


bool SetMaterialTexture(void* material,
                        const char* paramName,
                        IDirect3DTexture9* texture,
                        IDirect3DTexture9** texPtrStorage,
                        bool addRefTexture)
{
    if (!material)
        return false;

    // Do not pass NULL textures to the game
    if (!texture)
        return false;

    // Avoid refcount churn: only AddRef when the pointer actually changes
    IDirect3DTexture9* prevTex = texPtrStorage ? *texPtrStorage : nullptr;
    bool changed = (texture != prevTex);

    // Only AddRef custom textures, NOT the game's original textures
    // Do it only when the pointer changes
    bool didAddRef = false;
    if (addRefTexture && changed) {
        texture->AddRef();
        didAddRef = true;
    }

    // Store texture in the provided persistent storage
    if (changed)
        *texPtrStorage = texture;

    // Get material vtable
    void** vtable = *reinterpret_cast<void***>(material);
    (void)vtable; // suppress unused on /Od

    void* param = nullptr;

    // Call GetParameter - custom calling convention:
    // Material pointer is in ECX AND pushed on stack
    __asm {
        mov ecx, material
        push paramName
        push 0
        push ecx
        mov eax, material
        mov eax, [eax]
        call dword ptr [eax + 0x28]
        mov param, eax
    }

    if (!param)
    {
        // Failed to get parameter - only undo ref if we actually AddRef'd in this call
        if (didAddRef)
            texture->Release();
        return false;
    }

    // Call SetValue - custom calling convention:
    // Material pointer is in ECX AND pushed on stack
    // CRITICAL: Pass the address of the PERSISTENT storage, not a local variable!
    __asm {
        mov ecx, material
        mov eax, texPtrStorage
        push 4
        push eax
        push param
        push ecx
        mov edx, material
        mov edx, [edx]
        call dword ptr [edx + 0x50]
    }

    return true;
}


void SwapTextures(const SwapContext& ctx)
{
    if (ctx.swapCallCount) (*ctx.swapCallCount)++;

#ifdef _DEBUG
    if (ctx.swapCallCount && ctx.swapSuccessCount)
    {
        uint32_t calls = *ctx.swapCallCount;
        if (calls <= 10 || (calls % 1000) == 0)
        {
            asi_log::Log("CustomTextureLoader: SwapTextures called %u times (%u successful swaps, %d textures loaded)",
                         calls,
                         *ctx.swapSuccessCount,
                         ctx.texturesLoaded ? ctx.texturesLoaded->load() : 0);
        }
    }
#endif

    if (!ctx.pathsLoaded || !(*ctx.pathsLoaded))
        return;

    // Game context and material
    void** ptrContext = reinterpret_cast<void**>(ngg::mw::GAME_CONTEXT_PTR);
    void* context = ptrContext ? *ptrContext : nullptr;
    if (!context)
        return;

    void* material = *reinterpret_cast<void**>(reinterpret_cast<char*>(context) + 0x48);
    if (!material)
        return;

    // Texture wrappers
    void** ptrWrapper1 = reinterpret_cast<void**>(ngg::mw::GAME_TEX_WRAPPER_1);
    void** ptrWrapper2 = reinterpret_cast<void**>(ngg::mw::GAME_TEX_WRAPPER_2);
    void** ptrWrapper3 = reinterpret_cast<void**>(ngg::mw::GAME_TEX_WRAPPER_3);

    void* wrapper1 = ptrWrapper1 ? *ptrWrapper1 : nullptr;
    void* wrapper2 = ptrWrapper2 ? *ptrWrapper2 : nullptr;
    void* wrapper3 = ptrWrapper3 ? *ptrWrapper3 : nullptr;

    if (!wrapper1)
        return; // must have at least diffuse

    void** innerStruct1 = wrapper1 ? *reinterpret_cast<void***>(wrapper1) : nullptr;
    void** innerStruct2 = wrapper2 ? *reinterpret_cast<void***>(wrapper2) : nullptr;
    void** innerStruct3 = wrapper3 ? *reinterpret_cast<void***>(wrapper3) : nullptr;

    IDirect3DTexture9* gameTex1 = innerStruct1 ? *reinterpret_cast<IDirect3DTexture9**>(reinterpret_cast<char*>(innerStruct1) + 0x18) : nullptr;
    IDirect3DTexture9* gameTex2 = innerStruct2 ? *reinterpret_cast<IDirect3DTexture9**>(reinterpret_cast<char*>(innerStruct2) + 0x18) : nullptr;
    IDirect3DTexture9* gameTex3 = innerStruct3 ? *reinterpret_cast<IDirect3DTexture9**>(reinterpret_cast<char*>(innerStruct3) + 0x18) : nullptr;

    uint32_t hash1 = *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(wrapper1) + 0x24);
    uint32_t hash2 = wrapper2 ? *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(wrapper2) + 0x24) : 0u;
    uint32_t hash3 = wrapper3 ? *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(wrapper3) + 0x24) : 0u;

    IDirect3DTexture9* customTex1 = nullptr;
    IDirect3DTexture9* customTex2 = nullptr;
    IDirect3DTexture9* customTex3 = nullptr;


    if (ctx.swapTableBuilt && ctx.swapTableBuilt->load() && ctx.swapTablePtr && ctx.swapTableLock)
    {
        // Protect swap table pointer and its map from concurrent rebuild/deletion
        EnterCriticalSection(ctx.swapTableLock);
        auto* swapTable = ctx.swapTablePtr ? *ctx.swapTablePtr : nullptr;
        if (swapTable)
        {
            // Hash1 (Diffuse)
            if (hash1 != 0)
            {
                auto it = swapTable->find(hash1);
                if (it != swapTable->end())
                {
                    customTex1 = it->second;
                }
                else
                {
                    // Try by GameHash in main table
                    customTex1 = ctx.hashTable ? ctx.hashTable->GetTexture(hash1) : nullptr;

                    // Try cached CRC32 mapping → GH
                    if (!customTex1 && ctx.crc32Manager)
                    {
                        uint32_t cachedCRC32 = ctx.crc32Manager->GetCRC32ByGameHash(hash1);
                        if (cachedCRC32 != 0 && ctx.hashTable)
                        {
                            IDirect3DTexture9* t = ctx.hashTable->GetTexture(cachedCRC32);
                            if (t)
                            {
                                customTex1 = t;
                                t->AddRef();
                                (*swapTable)[hash1] = t;
                            }
                        }
                    }

            #ifdef _DEBUG
                    static int fallbackCount = 0;
                    if (fallbackCount < 10)
                    {
                        bool inSwap = (swapTable && swapTable->find(hash1) != swapTable->end());
                        bool inGH2CRC = false; uint32_t ghCRC = 0;
                        if (ctx.gameHashToCRC32Map && ctx.crc32MapLock) {
                            EnterCriticalSection(ctx.crc32MapLock);
                            auto it2 = ctx.gameHashToCRC32Map->find(hash1);
                            inGH2CRC = (it2 != ctx.gameHashToCRC32Map->end());
                            if (inGH2CRC) ghCRC = it2->second;
                            LeaveCriticalSection(ctx.crc32MapLock);
                        }
                        uint32_t mgrCRC = ctx.crc32Manager ? ctx.crc32Manager->GetCRC32ByGameHash(hash1) : 0;
                        bool hasByGH = (ctx.hashTable && ctx.hashTable->GetTexture(hash1) != nullptr);
                        bool hasByGhCRC = (ghCRC != 0 && ctx.hashTable && ctx.hashTable->GetTexture(ghCRC) != nullptr);
                        bool hasByMgrCRC = (mgrCRC != 0 && ctx.hashTable && ctx.hashTable->GetTexture(mgrCRC) != nullptr);

                        if (customTex1)
                        {
                            asi_log::Log("FALLBACK SUCCESS: 0x%08X (inSwap=%d, gh2crc=%d, ghCRC=0x%08X, mgrCRC=0x%08X, hashTbl: GH=%d, ghCRC=%d, mgrCRC=%d)",
                                         hash1, (int)inSwap, (int)inGH2CRC, ghCRC, mgrCRC, (int)hasByGH, (int)hasByGhCRC, (int)hasByMgrCRC);
                        }
                        else
                        {
                            asi_log::Log("FALLBACK FAILED: 0x%08X (inSwap=%d, gh2crc=%d, ghCRC=0x%08X, mgrCRC=0x%08X, hashTbl: GH=%d, ghCRC=%d, mgrCRC=%d)",
                                         hash1, (int)inSwap, (int)inGH2CRC, ghCRC, mgrCRC, (int)hasByGH, (int)hasByGhCRC, (int)hasByMgrCRC);
                        }
                        fallbackCount++;
                    }
            #endif
                }
            }

            // Hash2 (Normal)
            if (hash2 != 0)
            {
                auto it = swapTable->find(hash2);
                if (it != swapTable->end())
                {
                    customTex2 = it->second;
                }
                else
                {
                    customTex2 = ctx.hashTable ? ctx.hashTable->GetTexture(hash2) : nullptr;
                    if (!customTex2 && ctx.crc32Manager)
                    {
                        uint32_t cachedCRC32 = ctx.crc32Manager->GetCRC32ByGameHash(hash2);
                        if (cachedCRC32 != 0 && ctx.hashTable)
                        {
                            IDirect3DTexture9* t = ctx.hashTable->GetTexture(cachedCRC32);
                            if (t)
                            {
                                customTex2 = t;
                                t->AddRef();
                                (*swapTable)[hash2] = t;
                            }
                        }
                    }
                }
            }

            // Hash3 (Specular)
            if (hash3 != 0)
            {
                auto it = swapTable->find(hash3);
                if (it != swapTable->end())
                {
                    customTex3 = it->second;
                }
                else
                {
                    customTex3 = ctx.hashTable ? ctx.hashTable->GetTexture(hash3) : nullptr;
                    if (!customTex3 && ctx.crc32Manager)
                    {
                        uint32_t cachedCRC32 = ctx.crc32Manager->GetCRC32ByGameHash(hash3);
                        if (cachedCRC32 != 0 && ctx.hashTable)
                        {
                            IDirect3DTexture9* t = ctx.hashTable->GetTexture(cachedCRC32);
                            if (t)
                            {
                                customTex3 = t;
                                t->AddRef();
                                (*swapTable)[hash3] = t;
                            }
                        }
                    }
                }
            }
        }
        LeaveCriticalSection(ctx.swapTableLock);
    }
    else
    {
        // Fallback when swap table is not built yet
        customTex1 = ctx.hashTable ? ctx.hashTable->GetTexture(hash1) : nullptr;
        customTex2 = ctx.hashTable ? ctx.hashTable->GetTexture(hash2) : nullptr;
        customTex3 = ctx.hashTable ? ctx.hashTable->GetTexture(hash3) : nullptr;

        if (!customTex1 && gameTex1 && ctx.crc32Manager)
        {
            uint32_t cachedCRC32 = ctx.crc32Manager->GetCRC32ByGameHash(hash1);
            if (cachedCRC32 != 0 && ctx.hashTable)
                customTex1 = ctx.hashTable->GetTexture(cachedCRC32);
        }
        if (!customTex2 && gameTex2 && ctx.crc32Manager)
        {
            uint32_t cachedCRC32 = ctx.crc32Manager->GetCRC32ByGameHash(hash2);
            if (cachedCRC32 != 0 && ctx.hashTable)
                customTex2 = ctx.hashTable->GetTexture(cachedCRC32);
        }
        if (!customTex3 && gameTex3 && ctx.crc32Manager)
        {
            uint32_t cachedCRC32 = ctx.crc32Manager->GetCRC32ByGameHash(hash3);
            if (cachedCRC32 != 0 && ctx.hashTable)
                customTex3 = ctx.hashTable->GetTexture(cachedCRC32);
        }
    }

    // Track hashes that succeeded once and later failed (diagnostics)
    static std::unordered_set<uint32_t>* successfulHashes = nullptr;
    static std::unordered_set<uint32_t>* failedAfterSuccess = nullptr;
    if (!successfulHashes) { successfulHashes = new std::unordered_set<uint32_t>(); failedAfterSuccess = new std::unordered_set<uint32_t>(); }

    if (customTex1 && hash1 != 0) successfulHashes->insert(hash1);
    if (!customTex1 && hash1 != 0 && successfulHashes->count(hash1) > 0)
    {
        if (failedAfterSuccess->count(hash1) == 0)
        {
            failedAfterSuccess->insert(hash1);
            asi_log::Log("CustomTextureLoader: *** CRITICAL *** Hash 0x%08X was working but now fails! (%zu hashes failed after success)",
                         hash1, failedAfterSuccess->size());
        }
    }

    // Persistent storage for passing texture pointers to game's SetValue
    static IDirect3DTexture9** s_texPtr1 = new IDirect3DTexture9*(nullptr);
    static IDirect3DTexture9** s_texPtr2 = new IDirect3DTexture9*(nullptr);
    static IDirect3DTexture9** s_texPtr3 = new IDirect3DTexture9*(nullptr);

    static const char* diffuseMapStr  = "DiffuseMap";
    static const char* normalMapStr   = "NormalMapTexture";
    static const char* specularMapStr = "SPECULARMAPTEXTURE";

    // Only set when we actually have a custom texture; avoid redundant re-binding of original textures
    if (customTex1 && SetMaterialTexture(material, diffuseMapStr, customTex1, s_texPtr1, true))
    {
        if (ctx.swapSuccessCount) (*ctx.swapSuccessCount)++;
    }
    if (customTex2 && SetMaterialTexture(material, normalMapStr, customTex2, s_texPtr2, true))
    {
        if (ctx.swapSuccessCount) (*ctx.swapSuccessCount)++;
    }
    if (customTex3 && SetMaterialTexture(material, specularMapStr, customTex3, s_texPtr3, true))
    {
        if (customTex3 && ctx.swapSuccessCount) (*ctx.swapSuccessCount)++;
    }
}

} // namespace mw
} // namespace ngg

