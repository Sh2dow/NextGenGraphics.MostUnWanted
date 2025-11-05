#include "stdafx.h"
#include "CustomTextureLoader.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <fstream>
#include <mutex>

#include <windows.h>
#include <d3dx9.h>

#include "Log.h"
#include "MinHook.h"
#include "NFSMW_PreFEngHook.h"
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

    // Hash → loaded texture mapping (populated on-demand during Hook 2)
    std::unordered_map<uint32_t, IDirect3DTexture9*> g_textureMap;

    // Mutex for thread-safe texture loading
    std::mutex g_textureMutex;

    bool g_pathsLoaded = false;
    uint32_t g_swapCallCount = 0;
    uint32_t g_swapSuccessCount = 0;

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
        if (g_pathsLoaded)
            return;

        asi_log::Log("CustomTextureLoader: Parsing texture pack JSON files...");

        try
        {
            // Find TexturePacks directory
            fs::path gameDir = fs::current_path();
            fs::path texturePacksDir = gameDir / "NextGenGraphics" / "TexturePacks";

            if (!fs::exists(texturePacksDir))
            {
                asi_log::Log("CustomTextureLoader: TexturePacks directory not found");
                return;
            }

            int mappingCount = 0;

            // Scan all texture packs
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
                        mappingCount++;
                    }
                }
            }

            asi_log::Log("CustomTextureLoader: Parsed %d texture mappings (textures will load on-demand)", mappingCount);
            g_pathsLoaded = true;
        }
        catch (const std::exception& e)
        {
            asi_log::Log("CustomTextureLoader: Error parsing JSON: %s", e.what());
        }
    }

    // Load texture on-demand (called from Hook 2 when texture is first needed)
    IDirect3DTexture9* LoadTextureOnDemand(uint32_t hash)
    {
        if (!g_d3dDevice)
            return nullptr;

        // Check if already loaded
        {
            std::lock_guard<std::mutex> lock(g_textureMutex);
            auto it = g_textureMap.find(hash);
            if (it != g_textureMap.end())
                return it->second;
        }

        // Find file path
        auto pathIt = g_texturePathMap.find(hash);
        if (pathIt == g_texturePathMap.end())
            return nullptr;

        // Load texture
        IDirect3DTexture9* texture = nullptr;
        HRESULT hr = D3DXCreateTextureFromFileA(
            g_d3dDevice,
            pathIt->second.c_str(),
            &texture
        );

        if (SUCCEEDED(hr) && texture)
        {
            std::lock_guard<std::mutex> lock(g_textureMutex);
            g_textureMap[hash] = texture;
            return texture;
        }

        return nullptr;
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

        // Load textures on-demand (only if not already loaded)
        IDirect3DTexture9* customTex1 = LoadTextureOnDemand(hash1);
        IDirect3DTexture9* customTex2 = hash2 ? LoadTextureOnDemand(hash2) : nullptr;
        IDirect3DTexture9* customTex3 = hash3 ? LoadTextureOnDemand(hash3) : nullptr;

        // If no custom textures found, skip
        if (!customTex1 && !customTex2 && !customTex3)
            return;

        // Get material vtable
        void** vtable = *reinterpret_cast<void***>(material);

        // Get function pointers from vtable (accessed by BYTE offset!)
        void* getParameterFn = *reinterpret_cast<void**>(reinterpret_cast<char*>(vtable) + 0x28);
        void* setValueFn = *reinterpret_cast<void**>(reinterpret_cast<char*>(vtable) + 0x50);

        // Parameter name strings
        static const char* diffuseMapStr = "DiffuseMap";
        static const char* normalMapStr = "NormalMapTexture";
        static const char* specularMapStr = "SPECULARMAPTEXTURE";

        // Set diffuse texture if we found a custom one
        if (customTex1)
        {
            void* param = nullptr;

            // Call GetParameter using inline assembly to match the original ASI exactly
            // Assembly: push "DiffuseMap"; push 0; push material; mov ecx, material; call [vtable+28h]
            __asm {
                mov eax, diffuseMapStr
                push eax
                push 0
                mov ecx, material
                push ecx
                call getParameterFn
                mov param, eax
            }

            if (param)
            {
                // Call SetValue using inline assembly
                // Assembly: push 4; push &customTex; push param; push material; mov ecx, material; call [vtable+50h]
                __asm {
                    push 4
                    lea eax, customTex1
                    push eax
                    push param
                    mov ecx, material
                    push ecx
                    call setValueFn
                }

                g_swapSuccessCount++;
            }
        }

        // Set normal texture if we found a custom one
        if (customTex2 && wrapper2)
        {
            void* param = nullptr;

            __asm {
                mov eax, normalMapStr
                push eax
                push 0
                mov ecx, material
                push ecx
                call getParameterFn
                mov param, eax
            }

            if (param)
            {
                __asm {
                    push 4
                    lea eax, customTex2
                    push eax
                    push param
                    mov ecx, material
                    push ecx
                    call setValueFn
                }

                g_swapSuccessCount++;
            }
        }

        // Set specular texture if we found a custom one
        if (customTex3 && wrapper3)
        {
            void* param = nullptr;

            __asm {
                mov eax, specularMapStr
                push eax
                push 0
                mov ecx, material
                push ecx
                call getParameterFn
                mov param, eax
            }

            if (param)
            {
                __asm {
                    push 4
                    lea eax, customTex3
                    push eax
                    push param
                    mov ecx, material
                    push ecx
                    call setValueFn
                }

                g_swapSuccessCount++;
            }
        }
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

    // Parse JSON files immediately at startup (FAST - no texture loading!)
    LoadTexturePaths();

    // Install Hook 1: Re-parse JSON when graphics settings change
    // injector::MakeJMP(HOOK_LOAD_ADDR, &HookLoad, true);
    DWORD oldProtect;
    VirtualProtect((LPVOID)ngg::mw::HOOK_LOAD_ADDR, 16, PAGE_EXECUTE_READWRITE, &oldProtect);

    MH_CreateHook((LPVOID)ngg::mw::HOOK_LOAD_ADDR, &HookLoad, nullptr);
    MH_EnableHook((LPVOID)ngg::mw::HOOK_LOAD_ADDR);

    VirtualProtect((LPVOID)ngg::mw::HOOK_LOAD_ADDR, 16, oldProtect, &oldProtect);

    
    // Install Hook 2: Texture swapping (loads textures on-demand)
    // injector::MakeJMP(HOOK_SWAP_ADDR, &HookSwap, true);
    DWORD oldProtect2;
    VirtualProtect((LPVOID)ngg::mw::HOOK_SWAP_ADDR, 16, PAGE_EXECUTE_READWRITE, &oldProtect2);

    MH_CreateHook((LPVOID)ngg::mw::HOOK_SWAP_ADDR, &HookSwap, nullptr);
    MH_EnableHook((LPVOID)ngg::mw::HOOK_SWAP_ADDR);

    VirtualProtect((LPVOID)ngg::mw::HOOK_SWAP_ADDR, 16, oldProtect2, &oldProtect2);
    
    asi_log::Log("CustomTextureLoader: Initialization complete - textures will load on-demand");
}

// Disable feature (cleanup)
void CustomTextureLoader::disable()
{
    Feature::disable();

    // Release all textures
    {
        std::lock_guard<std::mutex> lock(g_textureMutex);
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

