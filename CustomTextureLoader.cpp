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
// CRITICAL DISCOVERY: The original ASI does NOT load textures synchronously!
// Hook 1 at 0x6C3A30: Parse JSON and build hash → file path map (FAST!)
//                     Called when graphics settings change
//                     Only stores file paths, NOT actual textures
// Hook 2 at 0x6C6C97: Swap textures every frame (sub_1005F060/sub_1005F070)
//                     Loads textures ON-DEMAND when first needed
//                     This is why startup is fast - textures load during gameplay!
// ============================================================================


namespace
{
    IDirect3DDevice9* g_d3dDevice = nullptr;

    // Hash → file path mapping (built during Hook 1)
    std::unordered_map<uint32_t, std::string> g_texturePathMap;

    // Hash → loaded 2D texture mapping (populated on-demand during Hook 2)
    std::unordered_map<uint32_t, IDirect3DTexture9*> g_textureMap;

    // Hash → loaded volume texture mapping (for 3D textures like LUTs)
    std::unordered_map<uint32_t, IDirect3DVolumeTexture9*> g_volumeTextureMap;

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

    // Parse JSON and build hash → file path map (FAST - no texture loading!)
    void LoadTexturePaths()
    {
        try
        {
            fs::path gameDir = fs::current_path();

            // Optimization: Skip re-parsing if already loaded
            if (g_pathsLoaded)
                return;

            asi_log::Log("CustomTextureLoader: Parsing texture files...");

            // Optimization: Reserve capacity for expected texture count
            g_texturePathMap.reserve(EXPECTED_TEXTURE_COUNT);

            int totalCount = 0;

            // 1. Load generic/global textures (water, noise, etc.)
            fs::path genericDir = gameDir / "NextGenGraphics" / "GenericTextures";
            if (fs::exists(genericDir))
            {
                int genericCount = 0;
                for (const auto& entry : fs::directory_iterator(genericDir))
                {
                    if (!entry.is_regular_file())
                        continue;

                    std::string filename = entry.path().stem().string(); // Without extension
                    uint32_t hash = CalcHash(filename);

                    g_texturePathMap[hash] = entry.path().string();
                    genericCount++;
                }

                asi_log::Log("CustomTextureLoader: Found %d generic textures", genericCount);
                totalCount += genericCount;
            }

            // 2. Load texture packs (custom textures)
            fs::path texturePacksDir = gameDir / "NextGenGraphics" / "TexturePacks";
            if (fs::exists(texturePacksDir))
            {
                int packCount = 0;

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

                    // Build hash → file path mappings (NO texture loading!)
                    for (const auto& mapping : packInfo["textureMappings"])
                    {
                        std::string gameId = mapping["gameId"];
                        std::string texturePath = mapping["texturePath"];

                        uint32_t hash = CalcHash(gameId);
                        fs::path fullPath = texturesDir / texturePath;

                        if (fs::exists(fullPath))
                        {
                            g_texturePathMap[hash] = fullPath.string();
                            packCount++;
                        }
                    }
                }

                asi_log::Log("CustomTextureLoader: Parsed %d texture pack mappings", packCount);
                totalCount += packCount;
            }

            asi_log::Log("CustomTextureLoader: Total %d texture mappings ready (will load on-demand)", totalCount);
            g_pathsLoaded = true;
        }
        catch (const std::exception& e)
        {
            asi_log::Log("CustomTextureLoader: Error parsing files: %s", e.what());
        }
    }

    // Load texture on-demand (called from Hook 2 when texture is first needed)
    IDirect3DTexture9* LoadTextureOnDemand(uint32_t hash)
    {
        if (!g_d3dDevice || hash == 0)
            return nullptr;

        // Optimization: Single mutex lock instead of double lock
        std::lock_guard<std::mutex> lock(g_textureMutex);

        // Check if already loaded
        auto it = g_textureMap.find(hash);
        if (it != g_textureMap.end())
            return it->second;

        // Find file path (no lock needed - path map is read-only after init)
        auto pathIt = g_texturePathMap.find(hash);
        if (pathIt == g_texturePathMap.end())
            return nullptr;

        // Load texture (D3DX call is thread-safe)
        IDirect3DTexture9* texture = nullptr;
        HRESULT hr = D3DXCreateTextureFromFileA(
            g_d3dDevice,
            pathIt->second.c_str(),
            &texture
        );

        if (SUCCEEDED(hr) && texture)
        {
            g_textureMap[hash] = texture;

            // One-time log for generic textures (from GenericTextures folder)
            if (pathIt->second.find("GenericTextures") != std::string::npos)
            {
                if (g_loggedGenericTextures.insert(hash).second)
                {
                    // Extract filename from path
                    fs::path filePath(pathIt->second);
                    std::string filename = filePath.filename().string();
                    asi_log::Log("CustomTextureLoader: Generic texture bound - %s (hash: 0x%08X)",
                                 filename.c_str(), hash);
                }
            }

            return texture;
        }

        return nullptr;
    }

    // Load volume texture on-demand (for 3D textures like LUTs, noise, etc.)
    IDirect3DVolumeTexture9* LoadVolumeTextureOnDemand(uint32_t hash)
    {
        if (!g_d3dDevice || hash == 0)
            return nullptr;

        // Optimization: Single mutex lock
        std::lock_guard<std::mutex> lock(g_textureMutex);

        // Check if already loaded
        auto it = g_volumeTextureMap.find(hash);
        if (it != g_volumeTextureMap.end())
            return it->second;

        // Find file path
        auto pathIt = g_texturePathMap.find(hash);
        if (pathIt == g_texturePathMap.end())
            return nullptr;

        // Load volume texture
        IDirect3DVolumeTexture9* volumeTexture = nullptr;
        HRESULT hr = D3DXCreateVolumeTextureFromFileA(
            g_d3dDevice,
            pathIt->second.c_str(),
            &volumeTexture
        );

        if (SUCCEEDED(hr) && volumeTexture)
        {
            g_volumeTextureMap[hash] = volumeTexture;

            // One-time log for generic volume textures (from GenericTextures folder)
            if (pathIt->second.find("GenericTextures") != std::string::npos)
            {
                if (g_loggedGenericTextures.insert(hash).second)
                {
                    // Extract filename from path
                    fs::path filePath(pathIt->second);
                    std::string filename = filePath.filename().string();
                    asi_log::Log("CustomTextureLoader: Generic volume texture bound - %s (hash: 0x%08X)",
                                 filename.c_str(), hash);
                }
            }

            return volumeTexture;
        }

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

        // Optimization: Load textures on-demand (hash check is done inside LoadTextureOnDemand)
        IDirect3DTexture9* customTex1 = LoadTextureOnDemand(hash1);
        IDirect3DTexture9* customTex2 = LoadTextureOnDemand(hash2);
        IDirect3DTexture9* customTex3 = LoadTextureOnDemand(hash3);

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

    // Hook 1: Parse JSON and build path map (FAST - no texture loading!)
    __declspec(naked) void HookLoad()
    {
        __asm
        {
            pushad
            call LoadTexturePaths
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

    // Optimization: Reserve capacity for texture cache
    g_textureMap.reserve(EXPECTED_TEXTURE_COUNT);

    // Parse JSON files immediately at startup (FAST - no texture loading!)
    LoadTexturePaths();

    // Install Hook 1: Re-parse JSON when graphics settings change
    injector::MakeJMP(ngg::mw::HOOK_LOAD_ADDR, &HookLoad, true);
    
    // Install Hook 2: Texture swapping (loads textures on-demand)
    injector::MakeJMP(ngg::mw::HOOK_SWAP_ADDR, &HookSwap, true);
    
    asi_log::Log("CustomTextureLoader: Initialization complete - textures will load on-demand");
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

        // Release volume textures
        for (auto& pair : g_volumeTextureMap)
        {
            if (pair.second)
                pair.second->Release();
        }
        g_volumeTextureMap.clear();
    }

    g_texturePathMap.clear();
    g_pathsLoaded = false;

    asi_log::Log("CustomTextureLoader: Disabled");
}

