#include "stdafx.h"
#include "CustomTextureLoader.h"
#include "TPFLoader.h"

#include "HashMaps/MW_GameTextureHashes.h"  // Auto-generated hash validation (MW TRACKS)

// Core components (Phase 1 refactoring)
#include "Core/TextureHashTable.h"
#include "Core/CRC32Manager.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <thread>
#include <atomic>
#include <vector>

#include <windows.h>
#include <d3dx9.h>

#include "Log.h"
#include "includes/json/include/nlohmann/json.hpp"
#include "CustomTextureHooks.h"

#include "TextureLoaderAsync.h"

#include "TexturePathParser.h"
#include "TextureSwapApply.h"


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
    using ::ngg::mw::SetMaterialTexture;

    // ============================================================================
    // CORE COMPONENTS
    // ============================================================================
    // Centralized storage and CRC32 mapping (see Core/*)
    ngg::mw::TextureHashTable* g_hashTable = nullptr;
    ngg::mw::CRC32Manager* g_crc32Manager = nullptr;


    // CRC32 mapping cache: Maps CRC32 hash <-> Game texture name hash
    // Static mapping loaded from Resources/MW_CRC32Cache.json at startup
    // CRITICAL: One CRC32 can map to MULTIPLE game hashes (same texture reused for different materials)
    std::unordered_map<uint32_t, std::vector<uint32_t>>* g_crc32ToGameHashMap = nullptr;  // CRC32 -> [GameHash1, GameHash2, ...]
    std::unordered_map<uint32_t, uint32_t>* g_gameHashToCRC32Map = nullptr;  // GameHash -> CRC32 (reverse lookup)
    CRITICAL_SECTION g_crc32MapLock;
    bool g_crc32MapInitialized = false;


    // Track installed hooks for cleanup
    bool g_isShuttingDown = false;  // Flag to skip cleanup during DLL unload

    // CRITICAL: Use raw Windows CRITICAL_SECTION instead of std::mutex to avoid CRT crashes during DLL unload
    // These are never deleted - OS cleans them up when process exits
    CRITICAL_SECTION g_d3dxMutex;
    bool g_mutexesInitialized = false;

    // Swap table lock to synchronize pointer swaps and reader access
    CRITICAL_SECTION g_swapTableLock;
    bool g_swapTableLockInitialized = false;

    bool g_pathsLoaded = false;
    uint32_t g_swapCallCount = 0;
    uint32_t g_swapSuccessCount = 0;

    // ============================================================================
    // SWAP TABLE OPTIMIZATION
    // ============================================================================
    // Pre-built swap table: hash -> custom texture (only for validated textures)
    // This is built once after all textures are loaded for fast O(1) lookup
    // NOTE: We do NOT use one-time swap tracking because MW's hook is called
    // every frame and we need to keep providing custom textures. The swap table
    // lookup is already fast enough (O(1) hash map lookup).
    std::unordered_map<uint32_t, IDirect3DTexture9*>* g_swapTable = nullptr;
    std::atomic<bool> g_swapTableBuilt{false};

    // IOCP-based thread pool for asynchronous texture loading
    HANDLE g_iocp = nullptr;
    // Use pointer to avoid destructor during CRT shutdown
    std::vector<std::thread>* g_workerThreads = nullptr;
    std::atomic<bool> g_stopLoading{false};
    std::atomic<int> g_texturesLoaded{0};
    std::atomic<int> g_totalTexturesToLoad{0};

    // Separate counters for TPF textures (to rebuild swap table when TPF finishes)
    std::atomic<int> g_tpfTexturesLoaded{0};
    std::atomic<int> g_totalTPFTexturesToLoad{0};

    // Global device pointer storage (updated by SetD3DDevice on render thread)
    // IOCP workers will read from this pointer
    // Forward declaration for swap table rebuild bridge
    static void RebuildSwapTableBridge(bool force);

    // Async loader context (references host-owned state; no ownership here)
    extern IDirect3DDevice9* volatile g_d3dDevice;

    static ngg::mw::async::Context g_asyncCtx{
        &g_iocp,
        &g_workerThreads,
        &g_stopLoading,
        &g_texturesLoaded,
        &g_totalTexturesToLoad,
        &g_tpfTexturesLoaded,
        &g_totalTPFTexturesToLoad,
        &g_d3dDevice,
        &g_d3dxMutex,
        &g_crc32MapLock,
        &g_crc32ToGameHashMap,
        &g_hashTable,
        &g_crc32Manager,
        &RebuildSwapTableBridge
    };

    IDirect3DDevice9* volatile g_d3dDevice = nullptr;

    // Dynamic IOCP worker thread count based on CPU cores
    // Use all available cores for maximum loading speed
    inline DWORD GetOptimalWorkerThreadCount()
    {
        SYSTEM_INFO sysInfo;
        GetSystemInfo(&sysInfo);
        DWORD numCores = sysInfo.dwNumberOfProcessors;
        
        // Use all cores, minimum 2, maximum 16 (reasonable limit)
        return max(2, min(numCores, 16));
    }

    // ============================================================================
    // TPF (TEXMOD PACKAGE FILE) LOADER
    // ============================================================================
    // TPF loading is now handled by TPFLoader class (see TPFLoader.cpp)
    // TPF files are encrypted ZIP archives containing DDS textures
    // ============================================================================

    int LoadTexturesFromTPF(const std::wstring& tpfPath, IDirect3DDevice9* device)
    {
        if (!device || !g_iocp)
        {
            asi_log::Log("CustomTextureLoader: Cannot load TPF - device or IOCP is NULL");
            return 0;
        }

        asi_log::Log("CustomTextureLoader: Loading TPF file: %S", tpfPath.c_str());

        // Use TPFLoader class to extract TPF and post entries to IOCP queue
        ngg::mw::TPFLoader loader;

        int entriesPosted = loader.LoadTPFAndPostToIOCP(tpfPath,
            [device](uint32_t hash, const std::string& filename, const uint8_t* ddsData, size_t ddsSize)
            {
                // Post to IOCP queue for parallel processing via async module
                std::vector<uint8_t> data(ddsData, ddsData + ddsSize);
                ngg::mw::async::PostTPFRequest(g_asyncCtx, hash, filename, std::move(data));
            });

        if (entriesPosted == 0)
        {
            asi_log::Log("CustomTextureLoader: Failed to load TPF file or no entries found");
            return 0;
        }

        asi_log::Log("CustomTextureLoader: Posted %d TPF texture entries to IOCP queue", entriesPosted);

        // Dynamic TPF mappings disabled — using static CRC32 cache only
        asi_log::Log("CustomTextureLoader: Dynamic TPF mappings disabled (using static CRC32 cache only)");
        // Update total textures to load counter for progress tracking
        g_totalTexturesToLoad.fetch_add(entriesPosted);

        // Update TPF-specific counter
        g_totalTPFTexturesToLoad.fetch_add(entriesPosted);

        return entriesPosted;
    }

    // Parse JSON and build hash→path map (FAST - no texture loading!)
    // Matches original ASI behavior: sub_1005DD50 only parses JSON
    void ParseTexturePaths()
    {
        // Delegated to module for clarity
        ngg::mw::paths::ParseTexturePaths(g_hashTable, g_pathsLoaded);
    }


    // ============================================================================
    // CARBON-STYLE SWAP TABLE OPTIMIZATION
    // ============================================================================
    // Build the swap table by validating all custom textures against game's texture list
    // This is called once after all textures are loaded
    // Provides fast O(1) lookup and filters out invalid hashes
#include "TextureSwapTable.h"
// Legacy BuildSwapTable moved to TextureSwapTable.h
static void RebuildSwapTableBridge(bool force) { BuildSwapTableEx(force, &g_swapTableLock); }

// Keeping the old implementation disabled for reference



    // Bridge to module-based SwapTextures using context
    void SwapTextures()
    {
        ngg::mw::SwapContext ctx{
            &g_pathsLoaded,
            &g_swapTableBuilt,
            &g_texturesLoaded,
            g_hashTable,
            g_crc32Manager,
            &g_swapTable,
            &g_swapTableLock,
            g_gameHashToCRC32Map,
            &g_crc32MapLock,
            &g_swapCallCount,
            &g_swapSuccessCount
        };
        ngg::mw::SwapTextures(ctx);
    }

    // Swap textures (called from Hook 2)

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

        asi_log::Log("CustomTextureLoader: Texture paths re-parsed - %d textures found", g_hashTable->CountTexturePaths());
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

    // Minimal instrumentation with rate limiting (CRT-safe)
    static void __stdcall DbgMarkSwapCall() {
        static volatile LONG s_count = 0;
        LONG c = InterlockedIncrement(&s_count);
        if (c <= 10 || (c % 1000) == 0) {
            OutputDebugStringA("NGG: HookSwap called\n");
        }
    }

    // Hook 2: Texture swapping (BACK to epilogue at 0x6C6C97)
    __declspec(naked) void HookSwap()
    {
        __asm
            {
            pushad
            // call DbgMarkSwapCall  ; disable instrumentation for performance
            call SwapTextures
            popad

            // Re-execute replaced instruction from 0x6C6C8D: mov dword ptr ds:[0x00982CCC], 0
            mov dword ptr ds:[0x00982CCC], 0

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

// Set shutdown flag (called from DllMain during DLL_PROCESS_DETACH)
void CustomTextureLoader::SetShuttingDown()
{
    g_isShuttingDown = true;
}

// Set D3D device (called from dllmain.cpp on every frame)
// NOTE: With IOCP approach, we don't store the device globally!
// Instead, we pass it WITH EACH TEXTURE LOADING REQUEST.
void CustomTextureLoader::SetD3DDevice(IDirect3DDevice9* device)
{
    GetOptimalWorkerThreadCount();
    
    if (!device || !g_pathsLoaded)
        return;

    // Initialize async workers on first device set
    static bool workersInitialized = false;
    if (!workersInitialized)
    {
        SYSTEM_INFO sysInfo; GetSystemInfo(&sysInfo);
        DWORD numWorkerThreads = sysInfo.dwNumberOfProcessors;
        if (numWorkerThreads < 2) numWorkerThreads = 2;
        if (numWorkerThreads > 16) numWorkerThreads = 16;
        if (!ngg::mw::async::InitializeWorkers(g_asyncCtx, numWorkerThreads))
        {
            asi_log::Log("CustomTextureLoader: Failed to initialize async workers!");
            return;
        }
        workersInitialized = true;
    }

    // Load TPF files on first device set (AFTER IOCP is created!)
    static bool tpfFilesLoaded = false;
    if (!tpfFilesLoaded)
    {
        // Ensure async module sees the current device for TPF posting
        *g_asyncCtx.globalDevice = device;

        asi_log::Log("CustomTextureLoader: Scanning for TPF files...");

        // Scan for TPF files in TexMod directory
        fs::path gameDir = fs::current_path();
        fs::path texmodDir = gameDir / "TexMod";

        if (fs::exists(texmodDir) && fs::is_directory(texmodDir))
        {
            int totalTPFsLoaded = 0;
            int totalTexturesLoaded = 0;

            for (const auto& entry : fs::directory_iterator(texmodDir))
            {
                if (!entry.is_regular_file())
                    continue;

                if (entry.path().extension() == ".tpf" || entry.path().extension() == ".TPF")
                {
                    std::wstring tpfPath = entry.path().wstring();
                    asi_log::Log("CustomTextureLoader: Loading TPF: %S", entry.path().filename().c_str());

                    int texturesLoaded = LoadTexturesFromTPF(tpfPath, device);
                    if (texturesLoaded > 0)
                    {
                        totalTPFsLoaded++;
                        totalTexturesLoaded += texturesLoaded;
                    }
                }
            }

            if (totalTPFsLoaded > 0)
            {
                asi_log::Log("CustomTextureLoader: Loaded %d TPF files with %d textures total",
                             totalTPFsLoaded, totalTexturesLoaded);
            }
            else
            {
                asi_log::Log("CustomTextureLoader: No TPF files found in TexMod directory");
            }
        }
        else
        {
            asi_log::Log("CustomTextureLoader: TexMod directory not found: %S", texmodDir.c_str());
        }

        tpfFilesLoaded = true;
    }

    // Track if we need to (re)start IOCP loading for regular textures
    static size_t lastTextureCount = 0;
    size_t currentTextureCount = g_hashTable->CountTexturePaths();

    // Start IOCP loading if:
    // 1. First time device is set, OR
    // 2. Texture paths changed (Hook 1 was called and re-parsed paths)
    if (lastTextureCount != currentTextureCount)
    {
        asi_log::Log("CustomTextureLoader: Starting IOCP loading (%d textures)...", currentTextureCount);
        ngg::mw::async::StartIOCPLoading(g_asyncCtx, device, g_hashTable, g_crc32Manager);
        lastTextureCount = currentTextureCount;
    }
}

// Enable feature (install hooks)
void CustomTextureLoader::enable()
{
    Feature::enable();

    // Initialize critical sections (never deleted - OS cleans up on process exit)
    if (!g_mutexesInitialized)
    {
        InitializeCriticalSection(&g_d3dxMutex);
        g_mutexesInitialized = true;
    }

    // Initialize swap table lock
    if (!g_swapTableLockInitialized)
    {
        InitializeCriticalSection(&g_swapTableLock);
        g_swapTableLockInitialized = true;
    }

    // Initialize CRC32 cache lock
    if (!g_crc32MapInitialized)
    {
        InitializeCriticalSection(&g_crc32MapLock);
        g_crc32MapInitialized = true;
    }

    // Initialize heap-allocated containers (never deleted - OS cleans up on process exit)
    if (!g_swapTable)
        g_swapTable = new std::unordered_map<uint32_t, IDirect3DTexture9*>();
    // NOTE: g_workerThreads is created in SetD3DDevice() when IOCP is initialized

    // Initialize Core components (Phase 2 refactoring)
    asi_log::Log("CustomTextureLoader: Initializing Core components...");

    if (!g_hashTable)
    {
        g_hashTable = new ngg::mw::TextureHashTable();
        g_hashTable->Initialize();
    }

    if (!g_crc32Manager)
    {
        g_crc32Manager = new ngg::mw::CRC32Manager();
        g_crc32Manager->Initialize();
    }

    // Log hashmap statistics
    asi_log::Log("CustomTextureLoader: Loaded game texture hashmap - %zu valid texture hashes from STREAML2RA.BUN",
                 ngg::mw::TOTAL_GAME_TEXTURES);

    asi_log::Log("CustomTextureLoader: Installing hooks...");
    {
        bool ok = ngg::mw::InstallTextureHooks(reinterpret_cast<void*>(&HookLoad),
                                               reinterpret_cast<void*>(&HookSwap));
        if (!ok) {
            asi_log::Log("CustomTextureLoader: One or more hooks failed to install");
        }
    }

    // Load CRC32 cache (Texmod compatibility) - using new CRC32Manager
    asi_log::Log("CustomTextureLoader: Loading CRC32 cache...");
    if (g_crc32Manager)
    {
        g_crc32Manager->LoadCache();
        // Expose the loaded static maps to async loader and swap-table builder
        EnterCriticalSection(&g_crc32MapLock);
        g_crc32ToGameHashMap = g_crc32Manager->GetCrc32ToGameMapPtr();
        g_gameHashToCRC32Map = g_crc32Manager->GetGameToCrc32MapPtr();
        LeaveCriticalSection(&g_crc32MapLock);
    }

    // Parse texture paths at startup (fast - no texture loading!)
    asi_log::Log("CustomTextureLoader: Parsing texture paths at startup...");
    ParseTexturePaths();
    asi_log::Log("CustomTextureLoader: Texture path parsing complete - %d textures found",
                 g_hashTable->CountTexturePaths());

    // NOTE: TPF loading will be done in SetD3DDevice() when device is available
    // We can't load TPF files here because we need a D3D device to create textures

    // IOCP loading will start when SetD3DDevice is called for the first time
    asi_log::Log("CustomTextureLoader: Waiting for D3D device to start IOCP loading...");
}

// Disable feature (cleanup)
void CustomTextureLoader::disable()
{
    Feature::disable();

    // CRITICAL: Stop IOCP worker threads FIRST to prevent new texture loads
    asi_log::Log("CustomTextureLoader: Stopping IOCP worker threads...");
    ngg::mw::async::ShutdownWorkers(g_asyncCtx);

    // Wait a bit for any in-flight operations to complete
    Sleep(100);

    // Unhook installed hooks AFTER stopping workers
    ngg::mw::UninstallTextureHooks();

    // Wait a bit for hooks to finish executing
    Sleep(100);

    // CRC32 cache saving deprecated — using static mapping only
    // No-op

    // Cleanup FastMem hash tables (releases all textures)
    if (g_hashTable)
        g_hashTable->Cleanup();

    g_pathsLoaded = false;

    asi_log::Log("CustomTextureLoader: Disabled");
}
