#include "stdafx.h"
#include "CustomTextureLoader.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <fstream>
#include <mutex>
#include <thread>
#include <atomic>

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
// CRITICAL DISCOVERY: Original ASI uses BACKGROUND THREAD for texture loading!
//
// Hook 1 at 0x6C3A30: Parse JSON (sub_1005DD50)
//                     Called ONLY when graphics settings change (NOT at startup!)
//                     nullsub_33 is just a "retn" instruction - a placeholder
// Hook 2 at 0x6C6C97: Swap textures every frame (sub_1005F060/sub_1005F070)
//                     Just swaps textures - NO loading (textures already loaded by background thread)
//
// Original ASI Architecture:
// 1. Startup: Parse JSON -> build hash→path map (fast)
// 2. Start BACKGROUND THREAD to load textures asynchronously
// 3. Game starts rendering immediately
// 4. Background thread loads textures one by one, adds to map
// 5. Hook 2 swaps textures as they become available
// 6. Result: Textures pop in gradually during gameplay!
//
// Our implementation:
// - enable(): Parse JSON, start background thread
// - Background thread: Load textures asynchronously
// - Hook 1: Re-parse JSON when graphics settings change, restart background thread
// - Hook 2: Swap textures (only if loaded by background thread)
// ============================================================================


namespace
{
    // Hash → file path mapping (built during Hook 1)
    std::unordered_map<uint32_t, std::string> g_texturePathMap;

    // Hash → loaded 2D texture mapping (populated by IOCP worker threads)
    std::unordered_map<uint32_t, IDirect3DTexture9*> g_textureMap;

    // Hash → loaded volume texture mapping (for 3D textures like LUTs)
    std::unordered_map<uint32_t, IDirect3DVolumeTexture9*> g_volumeTextureMap;

    // Mutex for thread-safe texture map access
    std::mutex g_textureMutex;

    // CRITICAL: D3DX functions might not be thread-safe even with D3DCREATE_MULTITHREADED!
    // Serialize all D3DX calls to prevent heap corruption
    std::mutex g_d3dxMutex;

    bool g_pathsLoaded = false;
    uint32_t g_swapCallCount = 0;
    uint32_t g_swapSuccessCount = 0;

    // Track which generic textures have been logged (one-time logging)
    std::unordered_set<uint32_t> g_loggedGenericTextures;

    // IOCP-based thread pool for asynchronous texture loading
    HANDLE g_iocp = nullptr;
    std::vector<std::thread> g_workerThreads;
    std::atomic<bool> g_stopLoading{false};
    std::atomic<int> g_texturesLoaded{0};
    std::atomic<int> g_totalTexturesToLoad{0};

    // Global device pointer storage (updated by SetD3DDevice on render thread)
    // IOCP workers will read from this pointer
    IDirect3DDevice9* volatile g_d3dDevice = nullptr;

    // Texture loading request structure (posted to IOCP queue)
    struct TextureLoadRequest
    {
        uint32_t hash;
        std::string path;
        IDirect3DDevice9** ppDevice; // POINTER to device pointer (dereferenced at load time, not post time!)
    };

    // Optimization: Reserve capacity for expected texture count (1000-2000+)
    constexpr size_t EXPECTED_TEXTURE_COUNT = 2000;
    constexpr DWORD NUM_WORKER_THREADS = 4; // Number of IOCP worker threads

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

    // Parse JSON and build hash→path map (FAST - no texture loading!)
    // Matches original ASI behavior: sub_1005DD50 only parses JSON
    void ParseTexturePaths()
    {
        try
        {
            fs::path gameDir = fs::current_path();

            // Optimization: Skip re-parsing if already parsed
            if (g_pathsLoaded)
                return;

            asi_log::Log("CustomTextureLoader: Parsing texture paths...");

            // Optimization: Reserve capacity for expected texture count
            g_texturePathMap.reserve(EXPECTED_TEXTURE_COUNT);

            int totalPaths = 0;
            int skippedMissing = 0;

            // 1. Parse generic/global textures (water, noise, etc.)
            fs::path genericDir = gameDir / "NextGenGraphics" / "GenericTextures";
            if (fs::exists(genericDir))
            {
                for (const auto& entry : fs::directory_iterator(genericDir))
                {
                    if (!entry.is_regular_file())
                        continue;

                    std::string filename = entry.path().stem().string(); // Without extension
                    uint32_t hash = CalcHash(filename);
                    std::string fullPath = entry.path().string();

                    g_texturePathMap[hash] = fullPath;
                    totalPaths++;
                }
            }

            // 2. Parse texture packs (custom textures)
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

                    // Build hash→path map from JSON (NO texture loading!)
                    for (const auto& mapping : packInfo["textureMappings"])
                    {
                        std::string gameId = mapping["gameId"];
                        std::string texturePath = mapping["texturePath"];

                        uint32_t hash = CalcHash(gameId);
                        fs::path fullPath = texturesDir / texturePath;

                        // Skip missing files (don't add to map)
                        if (!fs::exists(fullPath))
                        {
                            skippedMissing++;
                            continue;
                        }

                        g_texturePathMap[hash] = fullPath.string();
                        totalPaths++;
                    }
                }
            }

            asi_log::Log("CustomTextureLoader: Parsed %d texture paths (%d missing files skipped)",
                         totalPaths, skippedMissing);
            g_pathsLoaded = true;
        }
        catch (const std::exception& e)
        {
            asi_log::Log("CustomTextureLoader: Error parsing texture paths: %s", e.what());
        }
    }

    // Lookup texture (called from Hook 2)
    // Returns texture if loaded by background thread, NULL otherwise
    IDirect3DTexture9* GetTexture(uint32_t hash)
    {
        if (hash == 0)
            return nullptr;

        std::lock_guard<std::mutex> lock(g_textureMutex);

        // Just lookup in map - NO on-demand loading!
        auto it = g_textureMap.find(hash);
        if (it != g_textureMap.end())
        {
            IDirect3DTexture9* texture = it->second;
            // DON'T call AddRef() - the game doesn't expect it!
            // The texture is already owned by us (stored in the map)
            // The game will just use it, not take ownership
            return texture;
        }

        return nullptr; // Not loaded yet by background thread
    }

    // Load volume texture on-demand (called from Hook 2)
    // NOTE: Volume textures are loaded synchronously on first use (not via IOCP)
    // because they're rare (only LUTs) and need to be available immediately
    IDirect3DVolumeTexture9* GetVolumeTexture(uint32_t hash, IDirect3DDevice9* device)
    {
        if (hash == 0 || !device)
            return nullptr;

        std::lock_guard<std::mutex> lock(g_textureMutex);

        // Check if already loaded
        auto it = g_volumeTextureMap.find(hash);
        if (it != g_volumeTextureMap.end())
            return it->second;

        // Check if we have a path for this hash
        auto pathIt = g_texturePathMap.find(hash);
        if (pathIt == g_texturePathMap.end())
            return nullptr;

        // Load volume texture on-demand
        IDirect3DVolumeTexture9* volumeTexture = nullptr;
        HRESULT hr = D3DXCreateVolumeTextureFromFileA(
            device,
            pathIt->second.c_str(),
            &volumeTexture
        );

        if (SUCCEEDED(hr) && volumeTexture)
        {
            // Cache the loaded texture
            g_volumeTextureMap[hash] = volumeTexture;
            return volumeTexture;
        }

        // Failed to load - remove from path map to avoid retrying
        g_texturePathMap.erase(pathIt);
        return nullptr;
    }

    // IOCP worker thread function - processes texture loading requests
    // Matches original ASI behavior: uses IOCP thread pool for asynchronous loading
    void IOCPWorkerThread()
    {
        while (!g_stopLoading.load())
        {
            DWORD bytesTransferred = 0;
            ULONG_PTR completionKey = 0;
            LPOVERLAPPED overlapped = nullptr;

            // Wait for work item from IOCP queue (500ms timeout)
            BOOL result = GetQueuedCompletionStatus(
                g_iocp,
                &bytesTransferred,
                &completionKey,
                &overlapped,
                500 // 500ms timeout
            );

            // Check for shutdown signal
            if (!result && overlapped == nullptr)
            {
                // Timeout or error - check if we should stop
                if (g_stopLoading.load())
                    break;
                continue;
            }

            // Shutdown signal (completionKey == 0)
            if (completionKey == 0)
                break;

            // Process texture loading request
            TextureLoadRequest* request = reinterpret_cast<TextureLoadRequest*>(completionKey);
            if (request)
            {
                // CRITICAL: Dereference the device pointer AT LOAD TIME, not at post time!
                // This matches the original ASI's approach - always get the CURRENT device pointer
                IDirect3DDevice9** ppDevice = request->ppDevice;
                if (!ppDevice)
                {
                    asi_log::Log(
                        "CustomTextureLoader: ERROR - Device pointer-to-pointer is NULL! Skipping texture 0x%08X",
                        request->hash);
                    delete request;
                    continue;
                }

                IDirect3DDevice9* device = *ppDevice; // Dereference NOW to get current device
                if (!device)
                {
                    asi_log::Log(
                        "CustomTextureLoader: ERROR - Device pointer (*ppDevice) is NULL! Skipping texture 0x%08X",
                        request->hash);
                    delete request;
                    continue;
                }

                // CRITICAL: AddRef the device to prevent it from being released while we use it!
                // This ensures the device stays valid even if the game releases it
                device->AddRef();

                // Load texture using device pointer from request
                // CRITICAL: D3DX functions are NOT thread-safe! Serialize all D3DX calls!
                IDirect3DTexture9* texture = nullptr;
                HRESULT hr;
                {
                    std::lock_guard<std::mutex> d3dxLock(g_d3dxMutex);
                    hr = D3DXCreateTextureFromFileA(
                        device,
                        request->path.c_str(),
                        &texture
                    );
                }

                // Release the device reference we added
                device->Release();

                if (SUCCEEDED(hr) && texture)
                {
                    // Add to texture map (thread-safe)
                    {
                        std::lock_guard<std::mutex> lock(g_textureMutex);
                        g_textureMap[request->hash] = texture;
                    }

                    int loaded = g_texturesLoaded.fetch_add(1) + 1;
                    int total = g_totalTexturesToLoad.load();

                    // Log progress every 100 textures OR when all textures are loaded
                    if (loaded % 100 == 0 || loaded == total)
                    {
                        asi_log::Log("CustomTextureLoader: IOCP loading progress: %d/%d textures",
                                     loaded, total);
                    }
                }
                else
                {
                    // Remove failed path from map
                    std::lock_guard<std::mutex> lock(g_textureMutex);
                    g_texturePathMap.erase(request->hash);
                }

                // Free the request
                delete request;
            }
        }
    }

    // Stop IOCP worker threads
    void StopIOCPWorkers()
    {
        if (g_iocp)
        {
            g_stopLoading.store(true);

            // Post shutdown signals to all worker threads
            for (size_t i = 0; i < g_workerThreads.size(); i++)
            {
                PostQueuedCompletionStatus(g_iocp, 0, 0, nullptr);
            }

            // Wait for all threads to finish
            for (auto& thread : g_workerThreads)
            {
                if (thread.joinable())
                    thread.join();
            }

            g_workerThreads.clear();
            CloseHandle(g_iocp);
            g_iocp = nullptr;
            g_stopLoading.store(false);

            // Release the device pointer we AddRef'd in StartIOCPLoading
            if (g_d3dDevice)
            {
                g_d3dDevice->Release();
                g_d3dDevice = nullptr;
            }
        }
    }

    // Start IOCP worker threads and post texture loading requests
    void StartIOCPLoading(IDirect3DDevice9* device)
    {
        // Stop any existing workers
        StopIOCPWorkers();

        if (!device)
        {
            asi_log::Log("CustomTextureLoader: Cannot start IOCP loading - device is NULL!");
            return;
        }

        // Store device pointer globally for IOCP workers to access
        // AddRef to keep it alive while workers are using it
        device->AddRef();
        g_d3dDevice = device;

        // Reset counters
        g_texturesLoaded.store(0);
        g_totalTexturesToLoad.store(static_cast<int>(g_texturePathMap.size()));

        if (g_totalTexturesToLoad.load() == 0)
        {
            asi_log::Log("CustomTextureLoader: No textures to load");
            return;
        }

        // Create IOCP
        g_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, NUM_WORKER_THREADS);
        if (!g_iocp)
        {
            asi_log::Log("CustomTextureLoader: Failed to create IOCP!");
            return;
        }

        // Start worker threads
        for (DWORD i = 0; i < NUM_WORKER_THREADS; i++)
        {
            g_workerThreads.emplace_back(IOCPWorkerThread);
        }

        asi_log::Log("CustomTextureLoader: Started %d IOCP worker threads", NUM_WORKER_THREADS);

        // Post texture loading requests to IOCP queue
        int requestsPosted = 0;
        for (const auto& [hash, path] : g_texturePathMap)
        {
            // CRITICAL: Pass POINTER to device pointer, not the device itself!
            // This matches the original ASI - workers dereference at load time to get CURRENT device
            // This prevents crashes if the device is released/reset between post and load
            TextureLoadRequest* request = new TextureLoadRequest{
                hash, path, const_cast<IDirect3DDevice9**>(&g_d3dDevice)
            };

            // Post to IOCP queue
            if (!PostQueuedCompletionStatus(g_iocp, 0, reinterpret_cast<ULONG_PTR>(request), nullptr))
            {
                asi_log::Log("CustomTextureLoader: Failed to post request for hash 0x%08X", hash);
                delete request;
            }
            else
            {
                requestsPosted++;
            }
        }

        asi_log::Log("CustomTextureLoader: Posted %d texture loading requests to IOCP queue", requestsPosted);
    }

    // Optimization: Helper function to set material texture (reduces code duplication)
    // CRITICAL: texPtrStorage must point to STATIC or GLOBAL storage that persists!
    // The game stores the ADDRESS of the texture pointer, not the value!
    // addRefTexture: Whether to call AddRef() on the texture (only for custom textures, not game's original)
    bool SetMaterialTexture(void* material, const char* paramName, IDirect3DTexture9* texture,
                            IDirect3DTexture9** texPtrStorage, bool addRefTexture)
    {
        if (!material)
            return false;

        // CRITICAL: Don't pass NULL textures to the game!
        if (!texture)
            return false;

        // CRITICAL: Only AddRef custom textures, NOT the game's original textures!
        // The game already owns its own textures and manages their lifetime.
        // But our custom textures need an extra reference because the game will Release() them.
        if (addRefTexture)
            texture->AddRef();

        // Store texture in the provided static storage
        *texPtrStorage = texture;

        // Get material vtable
        void** vtable = *reinterpret_cast<void***>(material);
        void* getParameterFn = *reinterpret_cast<void**>(reinterpret_cast<char*>(vtable) + 0x28);
        void* setValueFn = *reinterpret_cast<void**>(reinterpret_cast<char*>(vtable) + 0x50);

        void* param = nullptr;

        // Call GetParameter - custom calling convention:
        // Material pointer is in ECX AND pushed on stack
        // Original ASI: mov ecx, material; push "DiffuseMap"; push 0; push ecx; call
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
            // Failed to get parameter - release the texture if we AddRef'd it
            if (addRefTexture)
                texture->Release();
            return false;
        }

        // Call SetValue - custom calling convention:
        // Material pointer is in ECX AND pushed on stack
        // Original ASI: mov ecx, material; push 4; push &texture; push param; push ecx; call
        // CRITICAL: Pass the address of the STATIC storage, not a local variable!
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

        // CRITICAL DISCOVERY from IDA analysis:
        // The original ASI does: mov eax, [wrapper]; mov ecx, [eax+18h]
        // This reads the GAME's original texture pointer from (wrapper[0])[0x18]
        // Then it reads the hash from wrapper[0x24]
        // Then it looks up a custom texture by hash
        // If found, it REPLACES the texture pointer; otherwise it keeps the original
        // Then it ALWAYS calls SetValue with a valid texture pointer!

        // Read the inner structure pointer from wrapper[0]
        void** innerStruct1 = wrapper1 ? *reinterpret_cast<void***>(wrapper1) : nullptr;
        void** innerStruct2 = wrapper2 ? *reinterpret_cast<void***>(wrapper2) : nullptr;
        void** innerStruct3 = wrapper3 ? *reinterpret_cast<void***>(wrapper3) : nullptr;

        // Read the GAME's original texture pointers from innerStruct[0x18]
        IDirect3DTexture9* gameTex1 = innerStruct1
                                          ? *reinterpret_cast<IDirect3DTexture9**>(reinterpret_cast<char*>(innerStruct1)
                                              + 0x18)
                                          : nullptr;
        IDirect3DTexture9* gameTex2 = innerStruct2
                                          ? *reinterpret_cast<IDirect3DTexture9**>(reinterpret_cast<char*>(innerStruct2)
                                              + 0x18)
                                          : nullptr;
        IDirect3DTexture9* gameTex3 = innerStruct3
                                          ? *reinterpret_cast<IDirect3DTexture9**>(reinterpret_cast<char*>(innerStruct3)
                                              + 0x18)
                                          : nullptr;

        // CRITICAL: The original ASI handles each texture INDEPENDENTLY!
        // If texture 1 is NULL, it still processes textures 2 and 3!
        // So we DON'T return early here - we process each texture separately below.

        // Read hashes from the wrapper structures at offset +0x24
        uint32_t hash1 = *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(wrapper1) + 0x24);
        uint32_t hash2 = wrapper2 ? *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(wrapper2) + 0x24) : 0;
        uint32_t hash3 = wrapper3 ? *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(wrapper3) + 0x24) : 0;

        // Lookup custom textures (loaded by background thread)
        // Returns NULL if texture not loaded yet - textures pop in gradually!
        IDirect3DTexture9* customTex1 = GetTexture(hash1);
        IDirect3DTexture9* customTex2 = GetTexture(hash2);
        IDirect3DTexture9* customTex3 = GetTexture(hash3);

        // CRITICAL: Use custom texture if available, otherwise use game's original texture
        // The original ASI does: cmovnz ecx, eax (conditional move if custom texture found)
        IDirect3DTexture9* finalTex1 = customTex1 ? customTex1 : gameTex1;
        IDirect3DTexture9* finalTex2 = customTex2 ? customTex2 : gameTex2;
        IDirect3DTexture9* finalTex3 = customTex3 ? customTex3 : gameTex3;

        // CRITICAL: Use HEAP-ALLOCATED storage for texture pointers!
        // The game stores the ADDRESS of these pointers, not the value!
        // Static storage might get reused/overwritten, so use heap instead.
        // These are intentionally leaked - the game needs them to persist forever!
        static IDirect3DTexture9** s_texPtr1 = new IDirect3DTexture9*(nullptr);
        static IDirect3DTexture9** s_texPtr2 = new IDirect3DTexture9*(nullptr);
        static IDirect3DTexture9** s_texPtr3 = new IDirect3DTexture9*(nullptr);

        // Parameter name strings
        static const char* diffuseMapStr = "DiffuseMap";
        static const char* normalMapStr = "NormalMapTexture";
        static const char* specularMapStr = "SPECULARMAPTEXTURE";

        // CRITICAL: ALWAYS call SetMaterialTexture with a valid texture!
        // The original ASI always calls SetValue, even when there's no custom texture.
        // It just passes the game's original texture back in that case.
        // If we skip calling SetValue, the game's internal state gets corrupted!

        // Set diffuse texture (ALWAYS - either custom or game's original)
        if (finalTex1 && SetMaterialTexture(material, diffuseMapStr, finalTex1, s_texPtr1, customTex1 != nullptr))
        {
            if (customTex1) // Only count as success if we actually swapped
                g_swapSuccessCount++;
        }

        // Set normal texture (ALWAYS - either custom or game's original)
        if (finalTex2 && SetMaterialTexture(material, normalMapStr, finalTex2, s_texPtr2, customTex2 != nullptr))
        {
            if (customTex2) // Only count as success if we actually swapped
                g_swapSuccessCount++;
        }

        // Set specular texture (ALWAYS - either custom or game's original)
        if (finalTex3 && SetMaterialTexture(material, specularMapStr, finalTex3, s_texPtr3, customTex3 != nullptr))
        {
            if (customTex3) // Only count as success if we actually swapped
                g_swapSuccessCount++;
        }
    }

    // Helper function to handle Hook 1 logic (called from naked function)
    void HandleHookLoad()
    {
        // Log that Hook 1 was called
        static int hookCallCount = 0;
        hookCallCount++;
        asi_log::Log("CustomTextureLoader: Hook 1 called (count: %d) - re-parsing texture paths...", hookCallCount);

        // Re-parse texture paths (fast - no texture loading!)
        g_pathsLoaded = false; // Force re-parse
        ParseTexturePaths();

        // NOTE: We do NOT start IOCP loading here!
        // IOCP loading is started from SetD3DDevice() where we have a valid device pointer.
        // Hook 1 is called when graphics settings change, which might happen before device creation.

        asi_log::Log("CustomTextureLoader: Texture paths re-parsed - %d textures found", g_texturePathMap.size());
    }

    // Hook 1: Parse texture paths (called when graphics settings change)
    // Re-parses JSON in case files changed - starts IOCP loading
    __declspec(naked) void HookLoad()
    {
        __asm
            {
            pushad
            call HandleHookLoad
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

// Set D3D device (called from dllmain.cpp on every frame)
// NOTE: With IOCP approach, we don't store the device globally!
// Instead, we pass it WITH EACH TEXTURE LOADING REQUEST.
void CustomTextureLoader::SetD3DDevice(IDirect3DDevice9* device)
{
    if (!device || !g_pathsLoaded)
        return;

    // Track if we need to (re)start IOCP loading
    static size_t lastTextureCount = 0;
    size_t currentTextureCount = g_texturePathMap.size();

    // Start IOCP loading if:
    // 1. First time device is set, OR
    // 2. Texture paths changed (Hook 1 was called and re-parsed paths)
    if (lastTextureCount != currentTextureCount)
    {
        asi_log::Log("CustomTextureLoader: Starting IOCP loading (%d textures)...", currentTextureCount);
        StartIOCPLoading(device);
        lastTextureCount = currentTextureCount;
    }
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

    // Parse texture paths at startup (fast - no texture loading!)
    asi_log::Log("CustomTextureLoader: Parsing texture paths at startup...");
    ParseTexturePaths();
    asi_log::Log("CustomTextureLoader: Texture path parsing complete - %d textures found",
                 g_texturePathMap.size());

    // IOCP loading will start when SetD3DDevice is called for the first time
    asi_log::Log("CustomTextureLoader: Waiting for D3D device to start IOCP loading...");
}

// Disable feature (cleanup)
void CustomTextureLoader::disable()
{
    Feature::disable();

    // Stop IOCP worker threads
    asi_log::Log("CustomTextureLoader: Stopping IOCP worker threads...");
    StopIOCPWorkers();

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
