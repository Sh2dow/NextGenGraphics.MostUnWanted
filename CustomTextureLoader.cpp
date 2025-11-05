#include "stdafx.h"
#include "CustomTextureLoader.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <fstream>
#include <mutex>

#include <windows.h>
#include <d3dx9.h>
#include "NFSMW_PreFEngHook.h"

#include "Log.h"
#include "includes/injector/injector.hpp"
#include "includes/json/include/nlohmann/json.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

// ============================================================================
// ORIGINAL ASI CUSTOMTEXTURELOADER RE-IMPLEMENTATION
// ============================================================================
// CRITICAL DISCOVERY: Hook 1 at 0x6C3A30 (nullsub_33) is ONLY called when graphics settings change!
// It's NOT called at startup, so we must manually load textures in enable().
//
// Hook 1 at 0x6C3A30: Parse JSON and load ALL textures (sub_1005DD50)
//                     Called ONLY when graphics settings change (NOT at startup!)
//                     nullsub_33 is just a "retn" instruction - a placeholder
// Hook 2 at 0x6C6C97: Swap textures every frame (sub_1005F060/sub_1005F070)
//                     Just swaps textures - NO loading (textures already loaded)
//                     This prevents stuttering during gameplay!
//
// Our implementation:
// - enable(): Manually load ALL textures at startup (prevents on-demand loading stutter)
// - Hook 1: Reload all textures when graphics settings change
// - Hook 2: Swap textures every frame (no loading)
// ============================================================================


namespace
{
    IDirect3DDevice9* g_d3dDevice = nullptr;

    // Hash → file path mapping (built during Hook 1)
    std::unordered_map<uint32_t, std::string> g_texturePathMap;

    // Hash → loaded 2D texture mapping (populated on-demand during Hook 2)
    std::unordered_map<uint32_t, IDirect3DTexture9*> g_textureMap;

    // Mutex for thread-safe texture loading
    std::mutex g_textureMutex;

    bool g_pathsLoaded = false;
    uint32_t g_swapCallCount = 0;
    uint32_t g_swapSuccessCount = 0;

    // Track which generic textures have been logged (one-time logging)
    std::unordered_set<uint32_t> g_loggedGenericTextures;

    // Optimization: Reserve capacity for expected texture count (1000-2000+)
    // No cache size limit - textures are loaded on-demand and kept in memory
    constexpr size_t EXPECTED_TEXTURE_COUNT = 2000;

    // Optimization: Track directory modification time to avoid re-parsing unchanged JSON
    fs::file_time_type g_lastDirModTime;

    // bStringHash from game at 0x460BF0 (DJB hash variant)
    constexpr uint32_t CalcHash(std::string_view str)
    {
        uint32_t hash = 0xFFFFFFFFu;
        for (unsigned char c : str)
            hash = hash * 33u + c;
        return hash;
    }

    // Parse JSON and load ALL textures (matches original ASI behavior)
    void LoadAllTextures()
    {
        try
        {
            fs::path gameDir = fs::current_path();

            // Optimization: Skip re-loading if already loaded
            if (g_pathsLoaded)
                return;

            if (!g_d3dDevice)
            {
                asi_log::Log("CustomTextureLoader: ERROR - D3D device not set!");
                return;
            }

            asi_log::Log("CustomTextureLoader: Loading all textures...");

            // Optimization: Reserve capacity for expected texture count
            g_texturePathMap.reserve(EXPECTED_TEXTURE_COUNT);
            g_textureMap.reserve(EXPECTED_TEXTURE_COUNT);

            int totalPaths = 0;
            int loaded2D = 0;
            int failed = 0;

            // 1. Load generic/global textures (water, noise, etc.)
            fs::path genericDir = gameDir / "NextGenGraphics" / "GenericTextures";
            if (fs::exists(genericDir))
            {
                for (const auto& entry : fs::directory_iterator(genericDir))
                {
                    if (!entry.is_regular_file())
                        continue;

                    std::string filename = entry.path().stem().string(); // Without extension
                    std::string extension = entry.path().extension().string();
                    uint32_t hash = CalcHash(filename);
                    std::string fullPath = entry.path().string();

                    g_texturePathMap[hash] = fullPath;
                    totalPaths++;

                    // Load as 2D texture
                    IDirect3DTexture9* texture = nullptr;
                    HRESULT hr = D3DXCreateTextureFromFileA(
                        g_d3dDevice,
                        fullPath.c_str(),
                        &texture
                    );

                    if (SUCCEEDED(hr) && texture)
                    {
                        g_textureMap[hash] = texture;
                        loaded2D++;
                    }
                    else
                    {
                        failed++;
                    }
                }
            }

            // 2. Load texture packs (custom textures)
            fs::path texturePacksDir = gameDir / "NextGenGraphics" / "TexturePacks";
            if (fs::exists(texturePacksDir))
            {
                for (const auto& packEntry : fs::directory_iterator(texturePacksDir))
                {
                    if (!packEntry.is_directory())
                        continue;

                    fs::path jsonPath = packEntry.path() / "TexturePackInfo.json";
                    if (!fs::exists(jsonPath))
                        continue;

                    // Parse JSON
                    std::ifstream jsonFile(jsonPath);
                    json packInfo = json::parse(jsonFile);

                    std::string rootDir = packInfo["rootDirectory"];
                    fs::path texturesDir = packEntry.path() / rootDir;

                    // Load all textures from this pack
                    for (const auto& mapping : packInfo["textureMappings"])
                    {
                        std::string gameId = mapping["gameId"];
                        std::string texturePath = mapping["texturePath"];

                        uint32_t hash = CalcHash(gameId);
                        fs::path fullPath = texturesDir / texturePath;

                        if (!fs::exists(fullPath))
                            continue;

                        g_texturePathMap[hash] = fullPath.string();
                        totalPaths++;

                        // Load texture immediately (2D only for texture packs)
                        IDirect3DTexture9* texture = nullptr;
                        HRESULT hr = D3DXCreateTextureFromFileA(
                            g_d3dDevice,
                            fullPath.string().c_str(),
                            &texture
                        );

                        if (SUCCEEDED(hr) && texture)
                        {
                            g_textureMap[hash] = texture;
                            loaded2D++;
                        }
                        else
                        {
                            failed++;
                        }
                    }
                }
            }

            asi_log::Log("CustomTextureLoader: Loaded %d textures (%d failed) from %d total paths",
                        loaded2D, failed, totalPaths);
            g_pathsLoaded = true;
        }
        catch (const std::exception& e)
        {
            asi_log::Log("CustomTextureLoader: Error loading textures: %s", e.what());
        }
    }

    // Get already-loaded texture by hash (called from Hook 2)
    IDirect3DTexture9* GetTexture(uint32_t hash)
    {
        if (hash == 0)
            return nullptr;

        std::lock_guard<std::mutex> lock(g_textureMutex);

        auto it = g_textureMap.find(hash);
        if (it != g_textureMap.end())
            return it->second;

        return nullptr;
    }

    // Optimization: Helper function to set material texture (reduces code duplication)
    bool SetMaterialTexture(void* material, const char* paramName, IDirect3DTexture9* texture)
    {
        if (!material || !texture)
            return false;

        // Get material vtable
        void** vtable = *reinterpret_cast<void***>(material);
        void* getParameterFn = *reinterpret_cast<void**>(reinterpret_cast<char*>(vtable) + 0x28);
        void* setValueFn = *reinterpret_cast<void**>(reinterpret_cast<char*>(vtable) + 0x50);

        void* param = nullptr;

        // Call GetParameter
        __asm {
            mov eax, paramName
            push eax
            push 0
            mov ecx, material
            push ecx
            call getParameterFn
            mov param, eax
        }

        if (!param)
            return false;

        // Call SetValue
        __asm {
            push 4
            lea eax, texture
            push eax
            push param
            mov ecx, material
            push ecx
            call setValueFn
        }

        return true;
    }

    // Swap textures (called from Hook 2)
    void SwapTextures()
    {
        g_swapCallCount++;

        // Don't swap if paths haven't been loaded yet
        if (!g_pathsLoaded)
            return;

        // NEW APPROACH: Use the game's material API to set textures
        // Read the game context object
        void** ptrContext = reinterpret_cast<void**>(ngg::mw::GAME_CONTEXT_PTR);
        void* context = *ptrContext;

        if (!context)
            return;

        // Get the material from context + 0x48
        void* material = *reinterpret_cast<void**>(reinterpret_cast<char*>(context) + 0x48);

        if (!material)
            return;

        // Read texture wrapper pointers
        void** ptrWrapper1 = reinterpret_cast<void**>(ngg::mw::GAME_TEX_WRAPPER_1);
        void** ptrWrapper2 = reinterpret_cast<void**>(ngg::mw::GAME_TEX_WRAPPER_2);
        void** ptrWrapper3 = reinterpret_cast<void**>(ngg::mw::GAME_TEX_WRAPPER_3);

        void* wrapper1 = *ptrWrapper1;
        void* wrapper2 = *ptrWrapper2;
        void* wrapper3 = *ptrWrapper3;

        if (!wrapper1)
            return;

        // CRITICAL DISCOVERY: The wrapper contains a POINTER to another structure!
        // mov eax, [edx] ; mov ecx, [eax+18h]
        // This means: D3D texture = **wrapper + 0x18 (double dereference!)
        // But hash is at *wrapper + 0x24 (single dereference)

        // Read hashes from the wrapper structures at offset +0x24 (single dereference)
        uint32_t hash1 = *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(wrapper1) + 0x24);
        uint32_t hash2 = wrapper2 ? *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(wrapper2) + 0x24) : 0;
        uint32_t hash3 = wrapper3 ? *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(wrapper3) + 0x24) : 0;

        // Get already-loaded textures (all textures loaded in Hook 1)
        IDirect3DTexture9* customTex1 = GetTexture(hash1);
        IDirect3DTexture9* customTex2 = GetTexture(hash2);
        IDirect3DTexture9* customTex3 = GetTexture(hash3);

        // If no custom textures found, skip
        if (!customTex1 && !customTex2 && !customTex3)
            return;

        // Optimization: Use helper function to reduce code duplication
        // Parameter name strings
        static const char* diffuseMapStr = "DiffuseMap";
        static const char* normalMapStr = "NormalMapTexture";
        static const char* specularMapStr = "SPECULARMAPTEXTURE";

        // Set textures using helper function
        if (customTex1 && SetMaterialTexture(material, diffuseMapStr, customTex1))
            g_swapSuccessCount++;

        if (customTex2 && SetMaterialTexture(material, normalMapStr, customTex2))
            g_swapSuccessCount++;

        if (customTex3 && SetMaterialTexture(material, specularMapStr, customTex3))
            g_swapSuccessCount++;
    }

    // Hook 1: Load all textures (called on startup and when graphics settings change)
    __declspec(naked) void HookLoad()
    {
        __asm
        {
            pushad
        }

        // Log that Hook 1 was called
        static int hookCallCount = 0;
        hookCallCount++;
        asi_log::Log("CustomTextureLoader: Hook 1 called (count: %d) - loading all textures...", hookCallCount);

        // Load all textures
        LoadAllTextures();

        __asm
        {
            popad
            ret
        }
    }

    // Hook 2: Texture swapping (BACK to epilogue at 0x6C6C97)
    __declspec(naked) void HookSwap()
    {
        __asm
        {
            pushad
            call SwapTextures
            popad

            // Execute original epilogue
            pop edi
            pop esi
            pop ebx
            mov esp, ebp
            pop ebp
            ret
        }
    }
}

// Set D3D device (called from dllmain.cpp)
void CustomTextureLoader::SetD3DDevice(IDirect3DDevice9* device)
{
    g_d3dDevice = device;
}

// Enable feature (install hooks)
void CustomTextureLoader::enable()
{
    Feature::enable();

    asi_log::Log("CustomTextureLoader: Installing hooks...");

    // Install Hook 1: Load all textures (called when graphics settings change)
    injector::MakeJMP(ngg::mw::HOOK_LOAD_ADDR, &HookLoad, true);

    // Install Hook 2: Texture swapping (just swaps, no loading)
    injector::MakeJMP(ngg::mw::HOOK_SWAP_ADDR, &HookSwap, true);

    asi_log::Log("CustomTextureLoader: Hooks installed");

    // CRITICAL: Hook 1 at 0x6C3A30 (nullsub_33) is ONLY called when graphics settings change,
    // NOT at startup! We must manually load all textures here to avoid on-demand loading stutter.
    asi_log::Log("CustomTextureLoader: Loading all textures at startup...");
    LoadAllTextures();
    asi_log::Log("CustomTextureLoader: Startup texture loading complete");
}

// Disable feature (cleanup)
void CustomTextureLoader::disable()
{
    Feature::disable();

    // Release all textures
    {
        std::lock_guard<std::mutex> lock(g_textureMutex);

        // Release 2D textures
        for (auto& pair : g_textureMap)
        {
            if (pair.second)
                pair.second->Release();
        }
        g_textureMap.clear();
    }

    g_texturePathMap.clear();
    g_pathsLoaded = false;

    asi_log::Log("CustomTextureLoader: Disabled");
}

