#include "stdafx.h"
#include "CustomTextureLoader.h"
#include "GameTextureHashes.h"  // Auto-generated hash validation (vanilla Carbon TRACKS)
#include "W2CTextureHashes.h"   // Auto-generated hash validation (W2C mod TRACKS)
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

// Conditional compilation for MW vs Carbon
#include "NFSC_PreFEngHook.h"

#include "Log.h"
#include "includes/minhook/include/MinHook.h"
#include "includes/json/include/nlohmann/json.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

// ============================================================================
// CUSTOMTEXTURELOADER RE-IMPLEMENTATION FOR NFS MOST WANTED & CARBON
// ============================================================================
// CRITICAL DISCOVERY: Original ASI uses BACKGROUND THREAD for texture loading!
//
// HOOKING IMPLEMENTATION:
// - Uses MinHook library for robust function hooking with trampolines
// - MinHook provides automatic instruction boundary detection (HDE32/64)
// - Trampolines allow calling original functions safely
// - Better error handling and debugging compared to manual memory patching
//
// MOST WANTED HOOKS:
// Hook 1 at 0x6C3A30: Parse JSON (sub_1005DD50)
//                     Called ONLY when graphics settings change (NOT at startup!)
//                     nullsub_33 is just a "retn" instruction - a placeholder
// Hook 2 at 0x6C6C8D: Swap textures every frame (sub_1005F060/sub_1005F070)
//                     Just swaps textures - NO loading (textures already loaded by background thread)
//
// CARBON HOOKS:
// Hook 1 at 0x000000: NOT FOUND - Carbon may not have equivalent nullsub
//                     Skipped for now - texture paths parsed at startup instead
// Hook 2 at 0x55CFD0: TextureInfo::Get hook for texture swapping
//                     Intercepts texture lookups and swaps D3D texture pointers
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
// - enable(): Initialize MinHook, install hooks, parse JSON, start background thread
// - Background thread: Load textures asynchronously via IOCP
// - Hook 1: Re-parse JSON when graphics settings change (MW only - Carbon skips this)
// - Hook 2: Swap textures (only if loaded by background thread)
//
// CARBON-SPECIFIC NOTES:
// - GAME_CONTEXT_PTR (0xAB0BA4) stores current effect* (shader pointer)
// - Material API should work similarly to MW (same vtable offsets)
// - Texture wrapper addresses are different (0x9EBxxx range vs MW's 0x982xxx)
// ============================================================================


namespace ngg
{
    namespace carbon
    {
        // ============================================================================
        // SHUTDOWN SAFETY
        // ============================================================================
        // CRITICAL: Flag to prevent cleanup operations during DLL unload
        // During DLL unload, the CRT and D3D are being torn down, and ANY cleanup
        // operations (even texture->Release()) can trigger crashes.
        static bool g_isShuttingDown = false;

        // ============================================================================
        // MEMORY MANAGEMENT - Use pointers to prevent destructor crashes
        // ============================================================================
        // CRITICAL: Use raw pointers instead of direct STL objects to prevent
        // destructor crashes during CRT shutdown. These are NEVER deleted - the OS
        // cleans them up when the process exits.

        // Hash → file path mapping (built during Hook 1)
        std::unordered_map<uint32_t, std::string>* g_texturePathMap = nullptr;

        // Hash → loaded 2D texture mapping (populated by IOCP worker threads)
        std::unordered_map<uint32_t, IDirect3DTexture9*>* g_textureMap = nullptr;

        // Hash → loaded volume texture mapping (for 3D textures like LUTs)
        std::unordered_map<uint32_t, IDirect3DVolumeTexture9*>* g_volumeTextureMap = nullptr;

        // CARBON-SPECIFIC: Texture replacement via TextureInfo::Get hook
        //
        // CRITICAL DISCOVERY from ExtendedCustomization SDK:
        // - TextureInfo::Get(hash, defaultIfNotFound, includeUnloaded) at 0x55CFD0
        // - TextureInfo has TextureInfoPlatInfo* PlatInfo member
        // - TextureInfoPlatInfo has IDirect3DTexture9* pD3DTexture member
        //
        // Strategy: Hook TextureInfo::Get, call original, then swap pD3DTexture if we have custom texture

        // TextureInfo structures (from ExtendedCustomization SDK)
        struct RenderState
        {
            unsigned int z_write_enabled : 1;
            unsigned int is_backface_culled : 1;
            unsigned int alpha_test_enabled : 1;
            unsigned int alpha_blend_enabled : 1;
            unsigned int alpha_blend_src : 4;
            unsigned int alpha_blend_dest : 4;
            unsigned int texture_address_u : 2;
            unsigned int texture_address_v : 2;
            unsigned int has_texture_animation : 1;
            unsigned int is_additive_blend : 1;
            unsigned int wants_auxiliary_textures : 1;
            unsigned int bias_level : 2;
            unsigned int multi_pass_blend : 1;
            unsigned int colour_write_alpha : 1;
            unsigned int sub_sort_key : 1;
            unsigned int alpha_test_ref : 4;
            unsigned int padding : 4;
        };

        struct TextureInfoPlatInfo
        {
            void* pNext;                        // +0x00: bNode next pointer
            void* pPrev;                        // +0x04: bNode prev pointer
            RenderState state;                  // +0x08: Render state
            int type;                           // +0x0C: Type
            IDirect3DTexture9* pD3DTexture;     // +0x10: D3D texture pointer (THIS IS WHAT WE SWAP!)
            unsigned short punchthruValue;      // +0x14: Punchthru value
            unsigned short format;              // +0x16: Format
        };

        struct TextureInfo
        {
            TextureInfoPlatInfo* PlatInfo;      // +0x00: Platform info pointer
            void* pNext;                        // +0x04: bNode next pointer
            void* pPrev;                        // +0x08: bNode prev pointer
            uint32_t key;                       // +0x0C: Texture hash
            // ... rest of structure not needed for our purposes
        };

        // Carbon's texture::info structure (from ExtendedCustomization/TextureInfo.h)
        struct platform_info
        {
            uint32_t state;           // +0x00: render_state
            uint32_t type;            // +0x04: type
            IDirect3DTexture9* texture; // +0x08: D3D texture pointer
            uint16_t punchthru_value; // +0x0C: punchthru_value
            uint16_t format;          // +0x0E: format
        };

        struct texture_info
        {
            platform_info* pinfo;     // +0x00: Pointer to platform_info
            void* next;               // +0x04: linked_node next
            void* prev;               // +0x08: linked_node prev
            uint32_t key;             // +0x0C: Hash
            // ... rest of structure (we don't need to fill it all)
        };



        // ============================================================================
        // THREAD SYNCHRONIZATION - Use CRITICAL_SECTION instead of std::mutex
        // ============================================================================
        // CRITICAL: Use raw Windows CRITICAL_SECTION instead of std::mutex to avoid
        // CRT crashes during DLL unload. These are NEVER deleted - OS cleans them up.

        // Mutex for thread-safe texture map access
        CRITICAL_SECTION g_textureMutex;

        // CRITICAL: D3DX functions might not be thread-safe even with D3DCREATE_MULTITHREADED!
        // Serialize all D3DX calls to prevent heap corruption
        CRITICAL_SECTION g_d3dxMutex;

        // Track if mutexes have been initialized
        bool g_mutexesInitialized = false;

        // TextureInfo::Get function (0x0055CFD0)
        // Signature: TextureInfo* __cdecl Get(uint32_t hash, bool defaultIfNotFound, bool includeUnloaded)
        typedef TextureInfo* (__cdecl *GetTextureInfoFn)(uint32_t hash, bool defaultIfNotFound, bool includeUnloaded);

        // MinHook trampoline to call original GetTextureInfo function
        GetTextureInfoFn g_gameGetTextureInfo = nullptr;

        // MinHook trampoline for HookLoad (if HOOK_LOAD_ADDR is available)
        typedef void (__cdecl *HookLoadFn)();
        HookLoadFn g_originalHookLoad = nullptr;

        bool g_pathsLoaded = false;
        uint32_t g_swapCallCount = 0;
        uint32_t g_swapSuccessCount = 0;

        // Track which generic textures have been logged (one-time logging)
        // Use pointer to avoid destructor during CRT shutdown
        std::unordered_set<uint32_t>* g_loggedGenericTextures = nullptr;

        // Pre-built swap table: hash -> custom texture (only for size-matched textures)
        // This is built once after all textures are loaded
        std::unordered_map<uint32_t, IDirect3DTexture9*>* g_swapTable = nullptr;
        std::atomic<bool> g_swapTableBuilt{false};

        // IOCP-based thread pool for asynchronous texture loading
        HANDLE g_iocp = nullptr;
        // Use pointer to avoid destructor during CRT shutdown
        std::vector<std::thread>* g_workerThreads = nullptr;
        std::atomic<bool> g_stopLoading{false};
        std::atomic<int> g_texturesLoaded{0};
        std::atomic<int> g_totalTexturesToLoad{0};
        std::atomic<bool> g_iocpStarted{false}; // Prevent duplicate StartIOCPLoading calls

        // Global device pointer storage (updated by SetD3DDevice on render thread)
        // IOCP workers will read from this pointer
        IDirect3DDevice9* volatile g_d3dDevice = nullptr;

        // W2C (World to Carbon) mod detection
        // If hyperlinked.asi is found, use W2C hashmap instead of vanilla Carbon hashmap
        std::atomic<bool> g_isW2CMod{false};
        std::atomic<bool> g_w2cDetectionDone{false};

        // Forward declaration
        void BuildSwapTable();

        // Detect if W2C (World to Carbon) mod is installed by checking for hyperlinked.asi
        inline bool DetectW2CMod()
        {
            if (g_w2cDetectionDone.load())
                return g_isW2CMod.load();

            try
            {
                fs::path gameDir = fs::current_path();
                fs::path hyperlinkedPath = gameDir / "scripts" / "hyperlinked.asi";

                bool exists = fs::exists(hyperlinkedPath);
                g_isW2CMod.store(exists);
                g_w2cDetectionDone.store(true);

                if (exists)
                {
                    asi_log::Log("CustomTextureLoader: W2C (World to Carbon) mod detected - using W2C texture hashmap (%zu textures)",
                               TOTAL_W2C_TEXTURES);
                }
                else
                {
                    asi_log::Log("CustomTextureLoader: Vanilla Carbon detected - using default texture hashmap (%zu textures)",
                               TOTAL_GAME_TEXTURES);
                }

                return exists;
            }
            catch (...)
            {
                g_isW2CMod.store(false);
                g_w2cDetectionDone.store(true);
                return false;
            }
        }

        // Unified hash validation function - uses appropriate hashmap based on W2C detection
        inline bool ValidateTextureHash(uint32_t hash)
        {
            DetectW2CMod(); // Ensure detection has run

            if (g_isW2CMod.load())
            {
                // Use W2C hashmap
                return IsValidW2CTextureHash(hash);
            }
            else
            {
                // Use vanilla Carbon hashmap
                return ngg::carbon::IsValidGameTextureHash(hash);
            }
        }

        // Game's memory allocator functions (from NFSC SDK / NFSCBulbToys)
        // These are faster and more compatible than C++ new/delete
        inline void* GameMalloc(size_t size)
        {
            return reinterpret_cast<void*(__cdecl*)(size_t)>(0x6A1560)(size);
        }

        inline void GameFree(void* ptr)
        {
            reinterpret_cast<void(__cdecl*)(void*)>(0x6A1590)(ptr);
        }

        // FastMem allocator (pool allocator for small objects)
        // NOTE: We don't know if there's a FastMem free function, so we use game malloc/free instead
        // FastMem is likely a frame/level allocator that's cleared in bulk, not suitable for IOCP requests
        inline void* FastMemAlloc(size_t size, const char* debug_name = nullptr)
        {
            return reinterpret_cast<void*(__thiscall*)(uintptr_t, size_t, const char*)>(0x60BA70)(0xA99720, size, debug_name);
        }

        // Texture loading request structure (posted to IOCP queue)
        // NOTE: We use game malloc/free for these instead of C++ new/delete
        // This avoids CRT heap fragmentation and is more compatible with game's memory management
        struct TextureLoadRequest
        {
            uint32_t hash;
            std::string path;
            IDirect3DDevice9** ppDevice; // POINTER to device pointer (dereferenced at load time, not post time!)

            // Custom allocator using game's malloc
            static void* operator new(size_t size)
            {
                return GameMalloc(size);
            }

            static void operator delete(void* ptr)
            {
                GameFree(ptr);
            }
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
                if (g_texturePathMap)
                    g_texturePathMap->reserve(EXPECTED_TEXTURE_COUNT);

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

                        if (g_texturePathMap)
                            (*g_texturePathMap)[hash] = fullPath;
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

                            if (g_texturePathMap)
                                (*g_texturePathMap)[hash] = fullPath.string();
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
            if (hash == 0 || !g_textureMap)
                return nullptr;

            EnterCriticalSection(&g_textureMutex);

            // Just lookup in map - NO on-demand loading!
            auto it = g_textureMap->find(hash);
            if (it != g_textureMap->end())
            {
                IDirect3DTexture9* texture = it->second;
                LeaveCriticalSection(&g_textureMutex);
                // DON'T call AddRef() - the game doesn't expect it!
                // The texture is already owned by us (stored in the map)
                // The game will just use it, not take ownership
                return texture;
            }

            LeaveCriticalSection(&g_textureMutex);
            return nullptr; // Not loaded yet by background thread
        }

        // Load volume texture on-demand (called from Hook 2)
        // NOTE: Volume textures are loaded synchronously on first use (not via IOCP)
        // because they're rare (only LUTs) and need to be available immediately
        IDirect3DVolumeTexture9* GetVolumeTexture(uint32_t hash, IDirect3DDevice9* device)
        {
            if (hash == 0 || !device || !g_volumeTextureMap || !g_texturePathMap)
                return nullptr;

            EnterCriticalSection(&g_textureMutex);

            // Check if already loaded
            auto it = g_volumeTextureMap->find(hash);
            if (it != g_volumeTextureMap->end())
            {
                IDirect3DVolumeTexture9* texture = it->second;
                LeaveCriticalSection(&g_textureMutex);
                return texture;
            }

            // Check if we have a path for this hash
            auto pathIt = g_texturePathMap->find(hash);
            if (pathIt == g_texturePathMap->end())
            {
                LeaveCriticalSection(&g_textureMutex);
                return nullptr;
            }

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
                (*g_volumeTextureMap)[hash] = volumeTexture;
                LeaveCriticalSection(&g_textureMutex);
                return volumeTexture;
            }

            LeaveCriticalSection(&g_textureMutex);

            // Failed to load - remove from path map to avoid retrying
            if (g_texturePathMap)
                g_texturePathMap->erase(pathIt);
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
                        EnterCriticalSection(&g_d3dxMutex);
                        hr = D3DXCreateTextureFromFileA(
                            device,
                            request->path.c_str(),
                            &texture
                        );
                        LeaveCriticalSection(&g_d3dxMutex);
                    }

                    // Release the device reference we added
                    device->Release();

                    if (SUCCEEDED(hr) && texture && g_textureMap)
                    {
                        // CARBON: Store D3D texture in map for GetTextureInfo hook
                        bool textureStored = false;
                        {
                            EnterCriticalSection(&g_textureMutex);

                            // Check if already loaded (prevents memory leak from duplicate loads)
                            auto it = g_textureMap->find(request->hash);
                            if (it != g_textureMap->end())
                            {
                                // Already loaded - release the new texture and keep the old one
                                texture->Release();
                                asi_log::Log("CustomTextureLoader: WARNING - Duplicate load for hash 0x%08X, keeping existing texture", request->hash);
                            }
                            else
                            {
                                // First time loading this hash - store it
                                (*g_textureMap)[request->hash] = texture;
                                textureStored = true;

                                // Log first 20 loaded hashes
                                static int logCount = 0;
                                if (logCount < 20)
                                {
                                    asi_log::Log("CustomTextureLoader: Loaded texture hash=0x%08X, D3D texture=%p, path=%s",
                                               request->hash, texture, request->path.c_str());
                                    logCount++;
                                }
                            }

                            LeaveCriticalSection(&g_textureMutex);
                        }

                        // Only increment counter if texture was actually stored (not a duplicate)
                        if (textureStored)
                        {
                            int loaded = g_texturesLoaded.fetch_add(1) + 1;
                            int total = g_totalTexturesToLoad.load();

                            // Log progress every 100 textures OR when all textures are loaded
                            if (loaded % 100 == 0 || loaded == total)
                            {
                                asi_log::Log("CustomTextureLoader: IOCP loading progress: %d/%d textures",
                                             loaded, total);
                            }

                            // When all textures are loaded, log completion and build swap table
                            if (loaded == total && g_textureMap)
                            {
                                asi_log::Log("CustomTextureLoader: All textures loaded - %d D3D textures ready", loaded);
                                asi_log::Log("CustomTextureLoader: Custom texture map built - %d entries", g_textureMap->size());

                                // Build the swap table (validates sizes and builds fast lookup)
                                BuildSwapTable();

                                asi_log::Log("CustomTextureLoader: Textures will be swapped via TextureInfo::Get hook as you drive around the world");
                            }
                        }
                    }
                    else
                    {
                        // DON'T erase from g_texturePathMap - this changes the size and triggers reload loop!
                        // Just log the error and continue
                        asi_log::Log("CustomTextureLoader: ERROR - Failed to load texture hash=0x%08X, path=%s, HRESULT=0x%08X",
                                   request->hash, request->path.c_str(), hr);
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
                if (g_workerThreads)
                {
                    for (size_t i = 0; i < g_workerThreads->size(); i++)
                    {
                        PostQueuedCompletionStatus(g_iocp, 0, 0, nullptr);
                    }

                    // Wait for all threads to finish
                    for (auto& thread : *g_workerThreads)
                    {
                        if (thread.joinable())
                            thread.join();
                    }

                    g_workerThreads->clear();
                }

                CloseHandle(g_iocp);
                g_iocp = nullptr;
                g_stopLoading.store(false);
                g_iocpStarted.store(false); // Allow restart after stop

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
            // Prevent duplicate calls (fixes texture reload loop bug)
            if (g_iocpStarted.load())
            {
                asi_log::Log("CustomTextureLoader: IOCP loading already started, ignoring duplicate call");
                return;
            }

            // Stop any existing workers
            StopIOCPWorkers();

            if (!device)
            {
                asi_log::Log("CustomTextureLoader: Cannot start IOCP loading - device is NULL!");
                return;
            }

            // Mark as started BEFORE posting requests (prevents race condition)
            g_iocpStarted.store(true);

            // Store device pointer globally for IOCP workers to access
            // AddRef to keep it alive while workers are using it
            device->AddRef();
            g_d3dDevice = device;

            // Reset counters
            g_texturesLoaded.store(0);
            if (g_texturePathMap)
                g_totalTexturesToLoad.store(static_cast<int>(g_texturePathMap->size()));

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
            if (g_workerThreads)
            {
                for (DWORD i = 0; i < NUM_WORKER_THREADS; i++)
                {
                    g_workerThreads->emplace_back(IOCPWorkerThread);
                }
            }

            asi_log::Log("CustomTextureLoader: Started %d IOCP worker threads", NUM_WORKER_THREADS);

            // Post texture loading requests to IOCP queue
            int requestsPosted = 0;
            if (g_texturePathMap)
            {
                for (const auto& [hash, path] : *g_texturePathMap)
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

        // CARBON-SPECIFIC: Build reverse map from game textures to hashes
        // Scans texture::loaded_table to map D3D texture pointers to their hashes
        void BuildGameTextureMap()
        {
            asi_log::Log("CustomTextureLoader: BuildGameTextureMap() - Scanning texture::loaded_table...");

            __try
            {
                // texture::loaded_table is at 0x00A921B0
                // It's a loader::table structure which is a linked list of texture::info nodes

                // Structure layout (from hyperlinked project):
                // struct loader::table {
                //     linked_node<texture::info>* head;  // +0x00
                //     linked_node<texture::info>* tail;  // +0x04
                //     uint32_t count;                    // +0x08
                // };

                struct LoaderTable {
                    void* head;
                    void* tail;
                    uint32_t count;
                };

                LoaderTable* table = reinterpret_cast<LoaderTable*>(0x00A921B0);

                if (!table || !table->head)
                {
                    asi_log::Log("CustomTextureLoader: BuildGameTextureMap() - Table is NULL or empty!");
                    return;
                }

                asi_log::Log("CustomTextureLoader: BuildGameTextureMap() - Found %d textures in table", table->count);

                // Iterate through linked list
                // struct texture::info : public linked_node<info> {
                //     linked_node<info>* next;  // +0x00 (from linked_node)
                //     linked_node<info>* prev;  // +0x04 (from linked_node)
                //     void* vtable;             // +0x08 (from platform_interface)
                //     uint32_t key;             // +0x0C - HASH!
                //     ...
                //     texture::platform_info* pinfo;  // Need to find offset
                // };

                void* current = table->head;
                int scanned = 0;
                int mapped = 0;

                while (current && scanned < 10000)  // Safety limit
                {
                    scanned++;

                    // Read hash at offset +0x0C
                    uint32_t hash = *reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(current) + 0x0C);

                    // Try to find platform_info pointer
                    // It should be somewhere in the structure
                    // Let's try common offsets: 0x14, 0x18, 0x1C, 0x20, 0x24, 0x28

                    for (int offset = 0x14; offset <= 0x40; offset += 4)
                    {
                        void* potentialPinfo = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(current) + offset);

                        if (!potentialPinfo)
                            continue;

                        // platform_info has D3D texture at offset +0x08
                        IDirect3DTexture9* d3dTexture = *reinterpret_cast<IDirect3DTexture9**>(
                            reinterpret_cast<uintptr_t>(potentialPinfo) + 0x08);

                        if (d3dTexture)
                        {
                            // Validate it's a real D3D texture by checking vtable
                            void* vtable = *reinterpret_cast<void**>(d3dTexture);

                            // D3D texture vtable should be in a reasonable memory range
                            if (reinterpret_cast<uintptr_t>(vtable) > 0x10000000 &&
                                reinterpret_cast<uintptr_t>(vtable) < 0x80000000)
                            {
                                // Store in reverse map (DISABLED - using TexWizard approach instead)
                                // g_gameTextureToHash[d3dTexture] = hash;
                                mapped++;

                                // Log first 5 mappings
                                if (mapped <= 5)
                                {
                                    asi_log::Log("CustomTextureLoader: Mapped texture %p -> hash 0x%08X (pinfo offset +0x%02X)",
                                                 d3dTexture, hash, offset);
                                }

                                break;  // Found it, move to next texture::info
                            }
                        }
                    }

                    // Move to next node
                    current = *reinterpret_cast<void**>(current);  // next pointer at +0x00
                }

                asi_log::Log("CustomTextureLoader: BuildGameTextureMap() - Scanned %d nodes, mapped %d textures",
                             scanned, mapped);
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                asi_log::Log("CustomTextureLoader: BuildGameTextureMap() - Exception during scanning!");
            }
        }

        // CARBON: Hook AttachReplacementTextureTable to inject our custom texture replacements
        // This is the OFFICIAL way Carbon swaps textures - much better than hooking GetTextureInfo!
        //
        // The game calls eModel::AttachReplacementTextureTable (0x005588D0) to apply texture replacements.
        // We hook BEFORE this call (at 0x007D5743) and inject our own replacement entries.
        //
        // IMPORTANT: ExtendedCustomization uses __fastcall, so we'll do the same!
        // At hook point 0x007D5743:
        //   ECX = model pointer (fastcall first param)
        //   EDX = unused (fastcall second param)
        //   [esp+0x20] = some parameter from stack
        //   EBX = count

        // Texture type based on filename suffix
        enum class TextureType
        {
            Unknown = -1,
            Diffuse = 0,   // _D suffix, stage 0
            Normal = 1,    // _N suffix, stage 1
            Specular = 2   // _S suffix, stage 2
        };

        // Map (game_texture, hash) pairs to custom textures
        // We need BOTH because the game reuses the same D3D texture pointer for different hashes!
        struct TextureKey
        {
            IDirect3DTexture9* gameTexture;
            uint32_t hash;

            bool operator==(const TextureKey& other) const
            {
                return gameTexture == other.gameTexture && hash == other.hash;
            }
        };

        struct TextureKeyHash
        {
            size_t operator()(const TextureKey& key) const
            {
                // Combine pointer and hash using XOR
                return std::hash<void*>()(key.gameTexture) ^ std::hash<uint32_t>()(key.hash);
            }
        };

        struct CustomTextureInfo
        {
            IDirect3DTexture9* texture;
            TextureType type;
        };

        // Map from hash to custom texture info
        // Use pointer to avoid destructor during CRT shutdown
        std::unordered_map<uint32_t, CustomTextureInfo>* g_hashToCustomTextureMap = nullptr;

        // Map from game D3D texture pointer to hash (reverse lookup)
        // This is populated in HookGetTextureInfo when the game loads textures
        // Use pointer to avoid destructor during CRT shutdown
        std::unordered_map<IDirect3DTexture9*, uint32_t>* g_gameTextureToHash = nullptr;

        CRITICAL_SECTION g_d3dSwapMutex;
        bool g_d3dSwapMutexInitialized = false;

        // Flag to disable hook during shutdown to prevent crashes
        std::atomic<bool> g_hookEnabled{true};

        // Original SetTexture function pointer
        typedef HRESULT(__stdcall* SetTexture_t)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
        SetTexture_t g_originalSetTexture = nullptr;

        // Hook IDirect3DDevice9::SetTexture to swap textures at D3D level
        HRESULT __stdcall HookSetTexture(IDirect3DDevice9* device, DWORD stage, IDirect3DBaseTexture9* pTexture)
        {
            // Track hook calls
            static std::atomic<int> hookCallCount{0};
            static bool loggedHookCalls = false;
            int callCount = hookCallCount.fetch_add(1) + 1;
            if (!loggedHookCalls && callCount >= 100)
            {
                asi_log::Log("CustomTextureLoader: HookSetTexture called %d times - hook is working!", callCount);
                loggedHookCalls = true;
            }

            // If hook is disabled or no texture, just call original
            if (!g_hookEnabled.load() || !pTexture)
                return g_originalSetTexture(device, stage, pTexture);

            // Check if this is a texture we want to swap
            IDirect3DTexture9* gameTexture = reinterpret_cast<IDirect3DTexture9*>(pTexture);
            IDirect3DTexture9* customTexture = nullptr;
            uint32_t hash = 0;

            if (g_gameTextureToHash && g_hashToCustomTextureMap)
            {
                EnterCriticalSection(&g_d3dSwapMutex);

                // Step 1: Look up which hash this game texture corresponds to
                auto hashIt = g_gameTextureToHash->find(gameTexture);
                if (hashIt != g_gameTextureToHash->end())
                {
                    hash = hashIt->second;

                    // Step 2: Look up custom texture by hash
                    auto texIt = g_hashToCustomTextureMap->find(hash);
                    if (texIt != g_hashToCustomTextureMap->end())
                    {
                        // Step 3: Check if texture type matches the stage
                        const CustomTextureInfo& info = texIt->second;

                        // Log first few type mismatches for debugging
                        static std::atomic<int> mismatchCount{0};
                        if (static_cast<int>(info.type) != static_cast<int>(stage))
                        {
                            int count = mismatchCount.fetch_add(1) + 1;
                            if (count <= 5)
                            {
                                asi_log::Log("CustomTextureLoader: Type mismatch #%d - hash=0x%08X, texture type=%d, stage=%d",
                                           count, hash, static_cast<int>(info.type), stage);
                            }
                        }
                        else
                        {
                            customTexture = info.texture;
                        }
                    }
                }

                LeaveCriticalSection(&g_d3dSwapMutex);
            }

            // If we have a custom texture, use it instead
            if (customTexture)
            {
                static std::atomic<int> swapCount{0};
                int count = swapCount.fetch_add(1) + 1;
                if (count <= 20)
                {
                    asi_log::Log("CustomTextureLoader: SetTexture swap #%d - stage=%d, hash=0x%08X, game texture=%p -> custom texture=%p",
                               count, stage, hash, gameTexture, customTexture);
                }
                return g_originalSetTexture(device, stage, customTexture);
            }

            // No custom texture, use original
            return g_originalSetTexture(device, stage, pTexture);
        }

        // Determine texture type from hash by looking up the path
        TextureType GetTextureTypeFromHash(uint32_t hash)
        {
            if (!g_texturePathMap)
                return TextureType::Unknown;

            EnterCriticalSection(&g_textureMutex);
            auto it = g_texturePathMap->find(hash);
            if (it != g_texturePathMap->end())
            {
                const std::string& path = it->second;
                // Check the last characters before .dds extension
                if (path.length() >= 6)
                {
                    std::string suffix = path.substr(path.length() - 6, 2);  // Get "_X" before ".dds"
                    if (suffix == "_D")
                    {
                        LeaveCriticalSection(&g_textureMutex);
                        return TextureType::Diffuse;
                    }
                    if (suffix == "_N")
                    {
                        LeaveCriticalSection(&g_textureMutex);
                        return TextureType::Normal;
                    }
                    if (suffix == "_S")
                    {
                        LeaveCriticalSection(&g_textureMutex);
                        return TextureType::Specular;
                    }
                }
            }
            LeaveCriticalSection(&g_textureMutex);
            return TextureType::Unknown;
        }

        // Build the swap table by validating all custom textures against game textures
        // This is called once after all textures are loaded
        void BuildSwapTable()
        {
            if (g_swapTableBuilt.load() || !g_textureMap || !g_gameGetTextureInfo)
                return;

            // Allocate swap table
            if (!g_swapTable)
                g_swapTable = new std::unordered_map<uint32_t, IDirect3DTexture9*>();

            EnterCriticalSection(&g_textureMutex);

            int totalCustomTextures = 0;
            int validatedTextures = 0;
            int invalidHashes = 0;

            // Iterate through all custom textures - NO SIZE VALIDATION, swap everything!
            for (const auto& pair : *g_textureMap)
            {
                uint32_t hash = pair.first;
                IDirect3DTexture9* customTexture = pair.second;
                totalCustomTextures++;

                // CRITICAL: Validate custom texture pointer
                if (!customTexture || customTexture == reinterpret_cast<IDirect3DTexture9*>(-1))
                {
                    invalidHashes++;
                    continue;
                }

                // Skip if not a valid game texture hash
                if (!ValidateTextureHash(hash))
                {
                    invalidHashes++;
                    continue;
                }

                // CRITICAL: Final validation - try to AddRef/Release to ensure texture is valid
                ULONG refCount = customTexture->AddRef();
                if (refCount > 0)
                {
                    customTexture->Release();
                    (*g_swapTable)[hash] = customTexture;
                    validatedTextures++;
                }
                else
                {
                    // Texture is invalid (refcount was 0)
                    invalidHashes++;
                }
            }

            LeaveCriticalSection(&g_textureMutex);

            g_swapTableBuilt.store(true);

            asi_log::Log("CustomTextureLoader: Swap table built - %d total, %d validated, %d invalid hashes",
                       totalCustomTextures, validatedTextures, invalidHashes);
        }

        // Track which textures we've already swapped to avoid swapping multiple times
        std::unordered_set<uint32_t>* g_swappedHashes = nullptr;

        // Hook TextureInfo::Get to swap textures using the pre-built swap table
        // Original function: TextureInfo* __cdecl Get(uint32_t hash, bool defaultIfNotFound, bool includeUnloaded)
        TextureInfo* __cdecl HookGetTextureInfo(uint32_t hash, bool defaultIfNotFound, bool includeUnloaded)
        {
            // Call original function to get the game's TextureInfo
            TextureInfo* textureInfo = g_gameGetTextureInfo(hash, defaultIfNotFound, includeUnloaded);

            // If hook is disabled (during shutdown), just return original
            if (!g_hookEnabled.load())
                return textureInfo;

            // If swap table is built, use it for fast lookup
            if (g_swapTableBuilt.load() && g_swapTable && textureInfo &&
                textureInfo != reinterpret_cast<TextureInfo*>(-1) && textureInfo->PlatInfo)
            {
                EnterCriticalSection(&g_textureMutex);

                // Check if we've already swapped this texture
                if (!g_swappedHashes)
                    g_swappedHashes = new std::unordered_set<uint32_t>();

                // Only swap if we haven't swapped this hash before
                if (g_swappedHashes->find(hash) == g_swappedHashes->end())
                {
                    auto it = g_swapTable->find(hash);
                    if (it != g_swapTable->end())
                    {
                        // CRITICAL: Only swap if the custom texture is valid
                        IDirect3DTexture9* customTexture = it->second;
                        if (customTexture && customTexture != reinterpret_cast<IDirect3DTexture9*>(-1))
                        {
                            // DIRECT POINTER SWAP - just replace the D3D texture pointer ONCE
                            textureInfo->PlatInfo->pD3DTexture = customTexture;

                            // Mark this hash as swapped so we don't swap it again
                            g_swappedHashes->insert(hash);

                            // Log first 10 swaps
                            static std::atomic<int> swapCount{0};
                            int count = swapCount.fetch_add(1) + 1;
                            if (count <= 10)
                            {
                                asi_log::Log("CustomTextureLoader: GetTextureInfo swap #%d - hash=0x%08X, swapped to %p (PERMANENT)",
                                           count, hash, customTexture);
                            }
                        }
                    }
                }
                else
                {
                    // Log first 10 re-access attempts to verify we're not re-swapping
                    static std::atomic<int> reAccessCount{0};
                    int count = reAccessCount.fetch_add(1) + 1;
                    if (count <= 10)
                    {
                        asi_log::Log("CustomTextureLoader: GetTextureInfo re-access #%d - hash=0x%08X (already swapped, skipping)",
                                   count, hash);
                    }
                }

                LeaveCriticalSection(&g_textureMutex);
            }

            return textureInfo;
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
            void** ptrContext = reinterpret_cast<void**>(ngg::carbon::GAME_CONTEXT_PTR);
            void* context = *ptrContext;

            if (!context)
                return;

            // Get the material from context + 0x48
            void* material = *reinterpret_cast<void**>(reinterpret_cast<char*>(context) + 0x48);

            if (!material)
                return;

            // CARBON: Completely different architecture!
            // Carbon uses texture::info structures that contain texture names and hashes
            // We need to hook at a different location where we have access to texture::info
            //
            // The correct hook location is in effect::set_texture_maps() at line 970:
            //   this->set_diffuse_map(*model.diffuse_texture_info);
            //
            // At that point, model.diffuse_texture_info contains:
            //   - texture::info::key (hash of texture name)
            //   - texture::info::name (texture name string)
            //   - texture::info::pinfo->texture (D3D texture pointer)
            //
            // We should hook BEFORE set_diffuse_map() is called and modify the D3D texture pointers
            // in the rendering_model structure.
            //
            // For now, this hook location (0x731138) is not the right place for Carbon.
            // We need to find and hook effect::set_texture_maps() instead.

            // Read the game's current texture pointers (for debugging)
            IDirect3DTexture9** ptrGameTex1 = reinterpret_cast<IDirect3DTexture9**>(ngg::carbon::GAME_TEX_WRAPPER_1);
            IDirect3DTexture9** ptrGameTex2 = reinterpret_cast<IDirect3DTexture9**>(ngg::carbon::GAME_TEX_WRAPPER_2);
            IDirect3DTexture9** ptrGameTex3 = reinterpret_cast<IDirect3DTexture9**>(ngg::carbon::GAME_TEX_WRAPPER_3);

            IDirect3DTexture9* gameTex1 = *ptrGameTex1;
            IDirect3DTexture9* gameTex2 = *ptrGameTex2;
            IDirect3DTexture9* gameTex3 = *ptrGameTex3;

            // Skip if all textures are NULL (frontend rendering, not world rendering)
            if (!gameTex1 && !gameTex2 && !gameTex3)
                return;

            // OLD APPROACH (disabled): Build reverse map and swap textures
            // Now using TexWizard-style GetTextureInfo hook instead
            /*
            static bool mapBuilt = false;
            if (!mapBuilt)
            {
                asi_log::Log("CustomTextureLoader: *** WORLD RENDERING DETECTED! *** tex1=%p, tex2=%p, tex3=%p",
                             gameTex1, gameTex2, gameTex3);
                asi_log::Log("CustomTextureLoader: Building reverse map NOW (textures should be loaded)...");

                BuildGameTextureMap();

                asi_log::Log("CustomTextureLoader: Reverse map built - size=%zu", g_gameTextureToHash.size());
                mapBuilt = true;
            }
            */

            // TexWizard approach: Texture swapping happens in HookGetTextureInfo()
            // This hook location (0x731138) is not the right place for Carbon
            // We're now hooking GetTextureInfo() calls instead

            static int swapAttempts = 0;
            if (swapAttempts < 5)
            {
                asi_log::Log("CustomTextureLoader: SwapTextures() called - tex1=%p, tex2=%p, tex3=%p",
                             gameTex1, gameTex2, gameTex3);
                swapAttempts++;
            }

            // CARBON SIMPLIFIED APPROACH:
            // Instead of calling material API (which has different vtable offsets),
            // we can directly modify the last_submitted_*_map_ pointers!
            // The game reads from these addresses, so we just swap the pointers directly.

            // For now, we can't swap without hashes, so just return
            // TODO: Implement hash extraction or alternative identification method

            // Placeholder: Would swap like this if we had hashes:
            // IDirect3DTexture9* customTex1 = GetTexture(hash1);
            // if (customTex1) {
            //     *ptrGameTex1 = customTex1;  // Direct pointer swap!
            //     g_swapSuccessCount++;
            // }

            return; // Not fully implemented yet
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

            asi_log::Log("CustomTextureLoader: Texture paths re-parsed - %d textures found", g_texturePathMap->size());
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

        // Hook 2: Texture swapping
        // MW: Hooks before epilogue at 0x6C6C8D, executes epilogue manually
        // Carbon: Hooks at CALL instruction at 0x731138
        //         Original code: mov ecx, [mInstance__9FEManager]; call sub_5915D0
        //         We replace the CALL with JMP to our hook, then execute the CALL ourselves
        __declspec(naked) void HookSwap()
        {
            __asm
                {
                pushad
                call SwapTextures
                popad

                // Carbon: Execute original CALL instruction
                // The instruction at 0x731138 is: call sub_5915D0 (E8 93 04 E6 FF)
                // ECX is already loaded with FEManager::mInstance by previous instruction at 0x731132
                // We just need to execute the call
                push 0x73113D  // Return address (instruction after the original call)
                push 0x5915D0  // Target function address
                ret            // Jump to sub_5915D0, it will return to 0x73113D
                }
        }

        // These allow the override feature to patch the texture map when TexWizard collisions are detected
        std::unordered_map<uint32_t, IDirect3DTexture9*>* GetTextureMap()
        {
            return g_textureMap;
        }

        std::unordered_map<uint32_t, std::string>* GetTexturePathMap()
        {
            return g_texturePathMap;
        }

        CRITICAL_SECTION* GetTextureMutex()
        {
            return &g_textureMutex;
        }
    }
}

// ============================================================================
// CLASS MEMBER FUNCTIONS (outside namespace)
// ============================================================================

// Set D3D device (called from dllmain.cpp on every frame)
// NOTE: With IOCP approach, we don't store the device globally!
// Instead, we pass it WITH EACH TEXTURE LOADING REQUEST.
void CustomTextureLoader::SetD3DDevice(IDirect3DDevice9* device)
{
    if (!device || !ngg::carbon::g_pathsLoaded)
        return;

    // SetTexture hook removed - we swap directly in GetTextureInfo instead
    // This is the CORRECT approach for Carbon!

    // Start IOCP loading only once (on first device set)
    // The g_iocpStarted flag in StartIOCPLoading prevents duplicate calls
    static bool firstCall = true;
    if (firstCall)
    {
        firstCall = false;
        size_t textureCount = ngg::carbon::g_texturePathMap->size();
        asi_log::Log("CustomTextureLoader: Starting IOCP loading (%d textures)...", textureCount);
        ngg::carbon::StartIOCPLoading(device);
    }
}

// Enable feature (install hooks)
void CustomTextureLoader::enable()
{
    Feature::enable();

    asi_log::Log("CustomTextureLoader: Installing hooks...");

    // Initialize critical sections (never deleted - OS cleans up on process exit)
    if (!ngg::carbon::g_mutexesInitialized)
    {
        InitializeCriticalSection(&ngg::carbon::g_textureMutex);
        InitializeCriticalSection(&ngg::carbon::g_d3dxMutex);
        ngg::carbon::g_mutexesInitialized = true;
    }
    if (!ngg::carbon::g_d3dSwapMutexInitialized)
    {
        InitializeCriticalSection(&ngg::carbon::g_d3dSwapMutex);
        ngg::carbon::g_d3dSwapMutexInitialized = true;
    }

    // Initialize heap-allocated containers (never deleted - OS cleans up on process exit)
    if (!ngg::carbon::g_texturePathMap)
        ngg::carbon::g_texturePathMap = new std::unordered_map<uint32_t, std::string>();
    if (!ngg::carbon::g_textureMap)
        ngg::carbon::g_textureMap = new std::unordered_map<uint32_t, IDirect3DTexture9*>();
    if (!ngg::carbon::g_volumeTextureMap)
        ngg::carbon::g_volumeTextureMap = new std::unordered_map<uint32_t, IDirect3DVolumeTexture9*>();
    if (!ngg::carbon::g_loggedGenericTextures)
        ngg::carbon::g_loggedGenericTextures = new std::unordered_set<uint32_t>();
    if (!ngg::carbon::g_workerThreads)
        ngg::carbon::g_workerThreads = new std::vector<std::thread>();
    if (!ngg::carbon::g_hashToCustomTextureMap)
        ngg::carbon::g_hashToCustomTextureMap = new std::unordered_map<uint32_t, ngg::carbon::CustomTextureInfo>();
    if (!ngg::carbon::g_gameTextureToHash)
        ngg::carbon::g_gameTextureToHash = new std::unordered_map<IDirect3DTexture9*, uint32_t>();

    // Initialize MinHook (already done in dllmain.cpp, but check anyway)
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED)
    {
        asi_log::Log("CustomTextureLoader: ERROR - MinHook initialization failed: %s", MH_StatusToString(status));
        return;
    }
    asi_log::Log("CustomTextureLoader: MinHook initialized successfully");

    // Install Hook 1: Load all textures (called when graphics settings change)
    // NOTE: Carbon doesn't have HOOK_LOAD_ADDR (nullsub equivalent not found)

    // Install Hook 2: TextureInfo::Get hook for texture swapping
    // CRITICAL: This is the CORRECT approach for Carbon!
    // - TextureInfo::Get(hash, defaultIfNotFound, includeUnloaded) at 0x55CFD0
    // - We call the original function, then swap the D3D texture pointer in PlatInfo
    // - This works for ALL textures (world, cars, objects, etc.)

    constexpr uintptr_t GET_TEXTURE_INFO_ADDR = 0x0055CFD0;

    // Use MinHook to create the hook and trampoline
    status = MH_CreateHook(
        (LPVOID)GET_TEXTURE_INFO_ADDR,
        (LPVOID)&ngg::carbon::HookGetTextureInfo,
        (LPVOID*)&ngg::carbon::g_gameGetTextureInfo
    );

    if (status != MH_OK)
    {
        asi_log::Log("CustomTextureLoader: ERROR - GetTextureInfo hook creation failed: %s", MH_StatusToString(status));
        return;
    }

    status = MH_EnableHook((LPVOID)GET_TEXTURE_INFO_ADDR);
    if (status != MH_OK)
    {
        asi_log::Log("CustomTextureLoader: ERROR - GetTextureInfo hook enable failed: %s", MH_StatusToString(status));
        return;
    }

    asi_log::Log("CustomTextureLoader: TextureInfo::Get hook installed at 0x%X (MinHook trampoline created)", GET_TEXTURE_INFO_ADDR);

    asi_log::Log("CustomTextureLoader: Hooks installed");

    // Parse texture paths at startup (fast - no texture loading!)
    asi_log::Log("CustomTextureLoader: Parsing texture paths at startup...");
    ngg::carbon::ParseTexturePaths();
    asi_log::Log("CustomTextureLoader: Texture path parsing complete - %d textures found",
                 ngg::carbon::g_texturePathMap->size());

    // NOTE: BuildGameTextureMap() is called during first world rendering
    // when textures are actually loaded in memory (not at startup)

    // IOCP loading will start when SetD3DDevice is called for the first time
    asi_log::Log("CustomTextureLoader: Waiting for D3D device to start IOCP loading...");
}

// Disable feature (cleanup)
void CustomTextureLoader::disable()
{
    Feature::disable();

    // CRITICAL: If we're shutting down (DLL unload), skip ALL cleanup!
    if (ngg::carbon::g_isShuttingDown)
    {
        asi_log::Log("CustomTextureLoader: Skipping cleanup (DLL unload in progress)");
        return;
    }

    // Stop IOCP worker threads
    asi_log::Log("CustomTextureLoader: Stopping IOCP worker threads...");
    ngg::carbon::StopIOCPWorkers();

    // Wait a bit for any in-flight operations to complete
    Sleep(100);

    // Disable MinHook hooks (only if NOT shutting down)
    asi_log::Log("CustomTextureLoader: Disabling MinHook hooks...");
    MH_DisableHook(MH_ALL_HOOKS);

    // Wait a bit for hooks to finish executing
    Sleep(100);

    // Release all textures (only if D3D device is still valid and NOT shutting down)
    if (ngg::carbon::g_d3dDevice != nullptr && ngg::carbon::g_textureMap && ngg::carbon::g_volumeTextureMap)
    {
        EnterCriticalSection(&ngg::carbon::g_textureMutex);

        // Release 2D textures
        for (auto& pair : *ngg::carbon::g_textureMap)
        {
            if (pair.second)
                pair.second->Release();
        }
        ngg::carbon::g_textureMap->clear();

        // Release volume textures
        for (auto& pair : *ngg::carbon::g_volumeTextureMap)
        {
            if (pair.second)
                pair.second->Release();
        }
        ngg::carbon::g_volumeTextureMap->clear();

        LeaveCriticalSection(&ngg::carbon::g_textureMutex);

        asi_log::Log("CustomTextureLoader: Released all D3D9 textures");
    }

    if (ngg::carbon::g_texturePathMap)
        ngg::carbon::g_texturePathMap->clear();
    ngg::carbon::g_pathsLoaded = false;

    // NOTE: We intentionally DO NOT:
    // - Delete critical sections (causes CRT crash during DLL unload)
    // - Delete heap-allocated containers (causes destructor crashes during shutdown)
    // - Call MH_Uninitialize() (done in dllmain.cpp)
    // The OS will clean up all memory when the process exits.

    asi_log::Log("CustomTextureLoader: Disabled");
}

// Called by HookedSetTexture in dllmain.cpp
// Returns a custom texture if we have one for the game's texture, otherwise nullptr
// SCOPE FILTERING: Only swap textures for TRACKS (world/environment), not UI/HUD elements
IDirect3DBaseTexture9* CustomTextureLoader::OnSetTexture(IDirect3DBaseTexture9* gameTexture)
{
    if (!gameTexture)
        return nullptr;

    // Don't swap if paths haven't been loaded yet
    if (!ngg::carbon::g_pathsLoaded)
        return nullptr;

    // Check if maps are initialized
    if (!ngg::carbon::g_gameTextureToHash || !ngg::carbon::g_hashToCustomTextureMap)
        return nullptr;

    // SCOPE FILTER DISABLED: Rely on hash lookup instead
    // If the texture isn't in our TRACKS texture map (g_gameTextureToHash),
    // we won't swap it. This is more reliable than checking game memory addresses.

    // Use the reverse lookup map to find the hash for this game texture
    // This map is populated in HookGetTextureInfo when the game loads textures
    IDirect3DTexture9* gameTexture2D = reinterpret_cast<IDirect3DTexture9*>(gameTexture);

    EnterCriticalSection(&ngg::carbon::g_d3dSwapMutex);

    // Step 1: Look up which hash this game texture corresponds to
    auto hashIt = ngg::carbon::g_gameTextureToHash->find(gameTexture2D);
    if (hashIt == ngg::carbon::g_gameTextureToHash->end())
    {
        LeaveCriticalSection(&ngg::carbon::g_d3dSwapMutex);
        return nullptr; // This texture is not in our map (probably not a TRACKS texture)
    }

    uint32_t hash = hashIt->second;

    // Step 2: Look up custom texture by hash
    auto texIt = ngg::carbon::g_hashToCustomTextureMap->find(hash);
    if (texIt == ngg::carbon::g_hashToCustomTextureMap->end())
    {
        LeaveCriticalSection(&ngg::carbon::g_d3dSwapMutex);
        return nullptr; // No custom texture for this hash
    }

    IDirect3DTexture9* customTexture = texIt->second.texture;
    LeaveCriticalSection(&ngg::carbon::g_d3dSwapMutex);

    // Track successful swaps
    static std::atomic<int> swapCount{0};
    int count = swapCount.fetch_add(1) + 1;

    // Log first 10 swaps for debugging
    if (count <= 10)
    {
        asi_log::Log("CustomTextureLoader: Swapped texture #%d - hash=0x%08X, game=%p, custom=%p",
                     count, hash, gameTexture, customTexture);
    }

    return customTexture;
}

// Set shutdown flag (called from DllMain during DLL_PROCESS_DETACH)
void CustomTextureLoader::SetShuttingDown()
{
    ngg::carbon::g_isShuttingDown = true;
}

// Called from DllMain on DLL_PROCESS_DETACH to clean up before exit
void CustomTextureLoader::Cleanup()
{
    // CRITICAL: If we're shutting down (DLL unload), skip ALL cleanup!
    // During DLL unload, the CRT and D3D are being torn down, and ANY cleanup
    // operations (even texture->Release()) can trigger STATUS_STACK_BUFFER_OVERRUN.
    if (ngg::carbon::g_isShuttingDown)
    {
        asi_log::Log("CustomTextureLoader: Skipping cleanup (DLL unload in progress)");
        return;
    }

    asi_log::Log("CustomTextureLoader: Cleanup - disabling hook and clearing caches");

    // Disable the hook to prevent crashes during shutdown
    ngg::carbon::g_hookEnabled.store(false);

    // Clear the hash->custom texture mapping and game texture->hash mapping
    {
        EnterCriticalSection(&ngg::carbon::g_d3dSwapMutex);
        if (ngg::carbon::g_hashToCustomTextureMap)
            ngg::carbon::g_hashToCustomTextureMap->clear();
        if (ngg::carbon::g_gameTextureToHash)
            ngg::carbon::g_gameTextureToHash->clear();
        LeaveCriticalSection(&ngg::carbon::g_d3dSwapMutex);
    }

    asi_log::Log("CustomTextureLoader: Cleanup complete");
}
