#pragma once

#include <windows.h>
#include <d3dx9.h>
#include <vector>
#include <string>
#include <atomic>
#include <cstdint>

#include <thread>

#include <unordered_map>

namespace ngg {
namespace mw {
class TextureHashTable;
class CRC32Manager;

namespace async {

// Forward declarations
struct Context;

// Callback type for triggering a swap table rebuild from the host TU
using RebuildSwapTableFn = void(*)(bool forceRebuild);

// IOCP request payloads -------------------------------------------------------
struct TextureLoadRequest {
    uint32_t            hash;
    std::string         path;
    IDirect3DDevice9**  ppDevice; // pointer to global device pointer
};

struct TPFTextureLoadRequest {
    uint32_t                   hash;
    std::string                filename;
    std::vector<uint8_t>       ddsData;
    IDirect3DDevice9**         ppDevice; // pointer to global device pointer
};

// Context passed by the host to avoid globals across translation units ----------
struct Context {
    // Lifetime owned by host; module only reads/writes through these pointers
    HANDLE*                            iocp;
    std::vector<std::thread>**         workerThreads;

    std::atomic<bool>*                 stopLoading;
    std::atomic<int>*                  texturesLoaded;
    std::atomic<int>*                  totalTexturesToLoad;

    std::atomic<int>*                  tpfTexturesLoaded;
    std::atomic<int>*                  totalTPFTexturesToLoad;

    IDirect3DDevice9* volatile*        globalDevice;  // pointer to global device pointer

    CRITICAL_SECTION*                  d3dxMutex;
    CRITICAL_SECTION*                  crc32MapLock;

    // Mapping tables (owned by host)
    std::unordered_map<uint32_t, std::vector<uint32_t>>** crc32ToGameHashMap;

    // Core components (owned by host)
    TextureHashTable**                 hashTable;
    CRC32Manager**                     crc32Manager;

    // Callback into host for swap-table rebuild
    RebuildSwapTableFn                 rebuildSwapTable;
};

// API -------------------------------------------------------------------------
// Initialize IOCP and spawn worker threads. Safe to call once.
bool InitializeWorkers(Context& ctx, DWORD workerCount);

// Gracefully stop worker threads, close IOCP, and release held device reference.
void ShutdownWorkers(Context& ctx);

// Post all file-based texture load requests to IOCP.
void StartIOCPLoading(Context& ctx, IDirect3DDevice9* device, TextureHashTable* hashTable, CRC32Manager* crc32Mgr);

// Post one TPF (in-memory DDS) texture request to IOCP.
bool PostTPFRequest(Context& ctx, uint32_t hash, const std::string& filename, std::vector<uint8_t>&& ddsData);

} // namespace async
} // namespace mw
} // namespace ngg

