#include "stdafx.h"
#include "TextureLoaderAsync.h"

#include "Log.h"
#include "Core/TextureHashTable.h"
#include "Core/CRC32Manager.h"

#include <windows.h>
#include <d3dx9.h>

namespace ngg { namespace mw { namespace async {

static void IOCPWorkerThread(Context ctx)
{
    // Boost thread priority during texture loading for faster startup
    HANDLE currentThread = GetCurrentThread();
    int originalPriority = GetThreadPriority(currentThread);
    SetThreadPriority(currentThread, THREAD_PRIORITY_ABOVE_NORMAL);

    while (!ctx.stopLoading->load())
    {
        DWORD bytesTransferred = 0;
        ULONG_PTR completionKey = 0;
        LPOVERLAPPED overlapped = nullptr;

        BOOL result = GetQueuedCompletionStatus(
            *ctx.iocp, &bytesTransferred, &completionKey, &overlapped, 500);

        if (!result && overlapped == nullptr)
        {
            if (ctx.stopLoading->load()) break;
            continue;
        }
        if (completionKey == 0) break; // shutdown signal

        if (bytesTransferred == 1)
        {
            // TPF texture request (DDS in memory)
            TPFTextureLoadRequest* req = reinterpret_cast<TPFTextureLoadRequest*>(completionKey);
            if (!req) continue;

            IDirect3DDevice9** ppDevice = req->ppDevice;
            if (!ppDevice) { delete req; continue; }
            IDirect3DDevice9* device = *ppDevice; if (!device) { delete req; continue; }
            device->AddRef();

            IDirect3DTexture9* texture = nullptr;
            HRESULT hr;
            {
                EnterCriticalSection(ctx.d3dxMutex);
                hr = D3DXCreateTextureFromFileInMemory(
                    device, req->ddsData.data(), (UINT)req->ddsData.size(), &texture);
                LeaveCriticalSection(ctx.d3dxMutex);
            }
            device->Release();

            if (SUCCEEDED(hr) && texture)
            {
                // Add to hash table by CRC32
                (*ctx.hashTable)->AddTexture(req->hash, texture);

                // Also add by all mapped game hashes for this CRC32
                EnterCriticalSection(ctx.crc32MapLock);
                if (*ctx.crc32ToGameHashMap)
                {
                    auto it = (*ctx.crc32ToGameHashMap)->find(req->hash);
                    if (it != (*ctx.crc32ToGameHashMap)->end())
                    {
                        for (uint32_t gameHash : it->second)
                            (*ctx.hashTable)->AddTexture(gameHash, texture);
                    }
                }
                LeaveCriticalSection(ctx.crc32MapLock);

                // Update counters and possibly trigger rebuild
                static CRITICAL_SECTION rebuildLock; static bool rebuildInit = false;
                if (!rebuildInit) { InitializeCriticalSection(&rebuildLock); rebuildInit = true; }
                EnterCriticalSection(&rebuildLock);

                int loaded = ctx.texturesLoaded->fetch_add(1) + 1;
                int total  = ctx.totalTexturesToLoad->load();
                int tpfLoaded = ctx.tpfTexturesLoaded->fetch_add(1) + 1;
                int tpfTotal  = ctx.totalTPFTexturesToLoad->load();

                if (loaded % 100 == 0 || loaded == total)
                    asi_log::Log("CustomTextureLoader: IOCP loading progress: %d/%d textures (%d/%d TPF)", loaded, total, tpfLoaded, tpfTotal);

                if (tpfLoaded == tpfTotal && tpfTotal > 0 && ctx.rebuildSwapTable)
                {
                    asi_log::Log("CustomTextureLoader: *** TPF REBUILD TRIGGERED *** All TPF textures loaded (%d/%d)", tpfLoaded, tpfTotal);
                    ctx.rebuildSwapTable(true);
                    asi_log::Log("CustomTextureLoader: *** TPF REBUILD COMPLETE *** Textures will be swapped as you drive around");
                }
                LeaveCriticalSection(&rebuildLock);
            }

            delete req;
        }
        else
        {
            // Regular texture request (load from file path)
            TextureLoadRequest* req = reinterpret_cast<TextureLoadRequest*>(completionKey);
            if (!req) continue;

            IDirect3DDevice9** ppDevice = req->ppDevice;
            if (!ppDevice) { asi_log::Log("CustomTextureLoader: ERROR - NULL ppDevice for 0x%08X", req->hash); delete req; continue; }
            IDirect3DDevice9* device = *ppDevice;
            if (!device) { asi_log::Log("CustomTextureLoader: ERROR - NULL device for 0x%08X", req->hash); delete req; continue; }

            device->AddRef();
            IDirect3DTexture9* texture = nullptr;
            HRESULT hr;
            {
                EnterCriticalSection(ctx.d3dxMutex);
                hr = D3DXCreateTextureFromFileA(device, req->path.c_str(), &texture);
                LeaveCriticalSection(ctx.d3dxMutex);
            }
            device->Release();

            if (SUCCEEDED(hr) && texture)
            {
                uint32_t crc32 = (*ctx.crc32Manager)->CalculateTexmodHash(texture);
                if (crc32 != 0)
                    (*ctx.hashTable)->SetCRC32Hash(req->hash, crc32);

                (*ctx.hashTable)->AddTexture(req->hash, texture);

                int loaded = ctx.texturesLoaded->fetch_add(1) + 1;
                int total  = ctx.totalTexturesToLoad->load();
                if (loaded % 100 == 0 || loaded == total)
                    asi_log::Log("CustomTextureLoader: IOCP loading progress: %d/%d textures", loaded, total);

                if (loaded == total)
                {
                    asi_log::Log("CustomTextureLoader: All textures loaded - building swap table...");
                    if (ctx.rebuildSwapTable) ctx.rebuildSwapTable(false);
                    asi_log::Log("CustomTextureLoader: Textures will be swapped as you drive around");
                }
            }
            else
            {
                static std::atomic<int> failureCount(0);
                int failures = failureCount.fetch_add(1) + 1;
                if (failures <= 10 || failures % 100 == 0)
                    asi_log::Log("CustomTextureLoader: Failed to load texture 0x%08X from '%s' (HRESULT: 0x%08X) - %d failures total",
                                 req->hash, req->path.c_str(), hr, failures);
            }

            delete req;
        }
    }

    SetThreadPriority(currentThread, originalPriority);
}

bool InitializeWorkers(Context& ctx, DWORD workerCount)
{
    if (!ctx.iocp || !ctx.workerThreads) return false;
    if (*ctx.iocp && *ctx.workerThreads) return true; // already initialized

    *ctx.iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, workerCount);
    if (!*ctx.iocp) { asi_log::Log("CustomTextureLoader: Failed to create IOCP!"); return false; }

    *ctx.workerThreads = new std::vector<std::thread>();
    for (DWORD i = 0; i < workerCount; ++i)
        (*ctx.workerThreads)->emplace_back([ctx]() mutable { IOCPWorkerThread(ctx); });

    asi_log::Log("CustomTextureLoader: Created IOCP with %lu worker threads", workerCount);
    return true;
}

void ShutdownWorkers(Context& ctx)
{
    if (!ctx.iocp || !*ctx.iocp) return;

    ctx.stopLoading->store(true);

    if (ctx.workerThreads && *ctx.workerThreads)
    {
        for (size_t i = 0; i < (*ctx.workerThreads)->size(); ++i)
            PostQueuedCompletionStatus(*ctx.iocp, 0, 0, nullptr);

        for (auto& th : **ctx.workerThreads)
            if (th.joinable()) th.join();

        (*ctx.workerThreads)->clear();
    }

    CloseHandle(*ctx.iocp);
    *ctx.iocp = nullptr;
    ctx.stopLoading->store(false);

    if (ctx.globalDevice && *ctx.globalDevice)
    {
        (*ctx.globalDevice)->Release();
        *ctx.globalDevice = nullptr;
    }
}

void StartIOCPLoading(Context& ctx, IDirect3DDevice9* device, TextureHashTable* hashTable, CRC32Manager* crc32Mgr)
{
    if (!device) { asi_log::Log("CustomTextureLoader: Cannot start IOCP loading - device is NULL!"); return; }
    if (!ctx.iocp || !*ctx.iocp || !ctx.workerThreads || !*ctx.workerThreads) { asi_log::Log("CustomTextureLoader: IOCP or worker threads not initialized!"); return; }

    device->AddRef();
    *ctx.globalDevice = device;

    ctx.texturesLoaded->store(0);
    int pathCount = hashTable->CountTexturePaths();
    ctx.totalTexturesToLoad->store(pathCount);
    if (ctx.totalTexturesToLoad->load() == 0) { asi_log::Log("CustomTextureLoader: No textures to load"); return; }

    int posted = 0;
    hashTable->ForEachTexturePath([&](uint32_t hash, const char* path)
    {
        TextureLoadRequest* req = new TextureLoadRequest{ hash, std::string(path), const_cast<IDirect3DDevice9**>(ctx.globalDevice) };
        if (!PostQueuedCompletionStatus(*ctx.iocp, 0, reinterpret_cast<ULONG_PTR>(req), nullptr))
        { asi_log::Log("CustomTextureLoader: Failed to post request for hash 0x%08X", hash); delete req; }
        else { posted++; }
    });

    asi_log::Log("CustomTextureLoader: Posted %d texture loading requests to IOCP queue", posted);
}

bool PostTPFRequest(Context& ctx, uint32_t hash, const std::string& filename, std::vector<uint8_t>&& ddsData)
{
    if (!ctx.iocp || !*ctx.iocp) return false;

    TPFTextureLoadRequest* req = new TPFTextureLoadRequest{ hash, filename, std::move(ddsData), const_cast<IDirect3DDevice9**>(ctx.globalDevice) };
    if (!PostQueuedCompletionStatus(*ctx.iocp, 1, reinterpret_cast<ULONG_PTR>(req), nullptr))
    {
        asi_log::Log("CustomTextureLoader: Failed to post TPF request for %s", filename.c_str());
        delete req; return false;
    }
    return true;
}

} } } // namespaces

