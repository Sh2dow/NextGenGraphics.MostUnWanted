// Minimal reimplementation of the NextGenGraphics.MostWanted plugin.
#include <windows.h>
#include <vector>
#include <memory>
#include <atomic>
#include "NFSMW_PreFEngHook.h"
#include "WriteProtectScope.h"
#include "features.h"
#include "CustomTextureLoader.h"
#include "Log.h"
#include "includes/injector/injector.hpp"

using namespace ngg::mw;

// TODO: Port full initialization logic from sub_10077220
static std::vector<std::unique_ptr<ngg::common::Feature>> g_features;

// Original Present function pointer
typedef HRESULT(APIENTRY* PresentFn)(IDirect3DDevice9*, CONST RECT*, CONST RECT*, HWND, CONST RGNDATA*);
PresentFn g_originalPresent = nullptr;
static std::atomic g_vtableHooked{ false };
static void** g_patchedVTableEntry = nullptr; // pointer to the vtable slot we patched
static void* g_savedOriginalPtr = nullptr;    // original pointer saved

bool triedInit = false;


static void Initialize()
{
    using namespace ngg::mw::features;

    g_features.emplace_back(std::make_unique<CustomTextureLoader>());

    asi_log::Log("Setting up hooks\n");

    for (const auto& feature : g_features)
    {
        asi_log::Log(feature->name());
        asi_log::Log(" initialized\n");
        feature->enable();
    }
}

void OnPresent()
{
    if (triedInit)
        return;

    auto feManager = *reinterpret_cast<void**>(FEMANAGER_INSTANCE_ADDR);
    if (feManager && !IsBadReadPtr(feManager, 0x40))
    {
        triedInit = true;
        Initialize();
    }
}

// Hooked Present (unchanged behaviour)
HRESULT APIENTRY HookedPresent(IDirect3DDevice9* device, CONST RECT* src, CONST RECT* dest, HWND wnd, CONST RGNDATA* dirty)
{
    OnPresent(); // trigger Initialize logic
    CustomTextureLoader::SetD3DDevice(device);

    // Call the original Present if we have it; fallback safe behavior if not
    if (g_originalPresent)
        return g_originalPresent(device, src, dest, wnd, dirty);

    // If no original, call through device's vtable (best-effort)
    typedef HRESULT(APIENTRY* PresentLocalFn)(IDirect3DDevice9*, CONST RECT*, CONST RECT*, HWND, CONST RGNDATA*);
    void** vtable = *reinterpret_cast<void***>(device);
    PresentLocalFn presentFn = reinterpret_cast<PresentLocalFn>(vtable[17]);
    if (presentFn && presentFn != &HookedPresent)
        return presentFn(device, src, dest, wnd, dirty);

    return D3D_OK;
}

// Patch Present by replacing vtable[17]
void HookPresent()
{
    // Create dummy device to get vtable
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d)
        return;

    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = GetForegroundWindow();

    IDirect3DDevice9* dummyDevice = nullptr;
    if (FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, pp.hDeviceWindow,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dummyDevice)))
    {
        d3d->Release();
        return;
    }

    // Get vtable from dummyDevice
    void** vtable = *reinterpret_cast<void***>(dummyDevice);
    if (!vtable)
    {
        dummyDevice->Release();
        d3d->Release();
        return;
    }

    // Save original Present (index 17)
    void* originalPtr = vtable[17];
    g_originalPresent = reinterpret_cast<PresentFn>(originalPtr);

    // Install hook using helper
    if (MakeVTableHook(vtable, 17, reinterpret_cast<void*>(&HookedPresent), &g_savedOriginalPtr))
    {
        g_vtableHooked.store(true);
        // store pointer to the actual slot we patched for later restore
        g_patchedVTableEntry = &vtable[17];
        asi_log::Log("HookPresent: vtable[17] patched successfully\n");
    }
    else
    {
        asi_log::Log("HookPresent: Failed to patch vtable[17]\n");
    }

    // Clean up dummy objects
    dummyDevice->Release();
    d3d->Release();
}

// Restore hook (call on DLL unload)
void UnhookPresent()
{
    if (g_vtableHooked.load() && g_patchedVTableEntry && g_savedOriginalPtr)
    {
        void** slot = g_patchedVTableEntry;
        void** vtable = slot - 17; // reverse offset

        if (UnmakeVTableHook(vtable, 17, g_savedOriginalPtr))
            asi_log::Log("UnhookPresent: vtable slot restored\n");
        else
            asi_log::Log("UnhookPresent: Failed to restore vtable slot\n");

        g_vtableHooked.store(false);
        g_patchedVTableEntry = nullptr;
        g_savedOriginalPtr = nullptr;
    }

    g_originalPresent = nullptr;
}

// Updated DllMain to clean up on detach
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);

        CreateThread(nullptr, 0, [](LPVOID) -> DWORD
        {
            HookPresent(); // install vtable patching on a background thread
            return 0;
        }, nullptr, 0, nullptr);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        // NOTE: We don't call feature->disable() here because:
        // 1. The game may have already destroyed D3D resources
        // 2. Calling Release() on already-freed D3D objects causes crashes
        // 3. When the process exits, the OS cleans up all resources anyway
        // 4. The original ASI doesn't do cleanup on exit either
        //
        // If we need to support runtime DLL unloading (hot reload), we would need
        // to add proper NULL checks and error handling in all disable() methods.

        // Unhook Present (this is safe because we control the vtable)
        UnhookPresent();
    }

    return TRUE;
}
