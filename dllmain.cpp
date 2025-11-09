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
#include "includes/minhook/include/MinHook.h"

using namespace ngg::mw;

// TODO: Port full initialization logic from sub_10077220
static std::vector<std::unique_ptr<ngg::common::Feature>> g_features;

// Original Present function pointer
typedef HRESULT(APIENTRY* PresentFn)(IDirect3DDevice9*, CONST RECT*, CONST RECT*, HWND, CONST RGNDATA*);
PresentFn g_originalPresent = nullptr;
static std::atomic g_vtableHooked{ false };
static void** g_patchedVTableEntry = nullptr; // pointer to the vtable slot we patched
static void* g_savedOriginalPtr = nullptr;    // original pointer saved

// Original CreateDevice function pointer
typedef HRESULT(APIENTRY* CreateDeviceFn)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
CreateDeviceFn g_originalCreateDevice = nullptr;
static std::atomic g_createDeviceHooked{ false };
static void** g_createDeviceVTableEntry = nullptr;
static void* g_savedCreateDevicePtr = nullptr;

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

// Hooked CreateDevice - adds D3DCREATE_MULTITHREADED flag
HRESULT APIENTRY HookedCreateDevice(
    IDirect3D9* d3d,
    UINT adapter,
    D3DDEVTYPE deviceType,
    HWND focusWindow,
    DWORD behaviorFlags,
    D3DPRESENT_PARAMETERS* presentParams,
    IDirect3DDevice9** device)
{
    // CRITICAL: Add D3DCREATE_MULTITHREADED flag to allow D3DX calls from worker threads!
    // Without this flag, D3DX calls from worker threads will corrupt internal state
    // and cause crashes when the game calls D3DX from the main thread.
    DWORD newBehaviorFlags = behaviorFlags | D3DCREATE_MULTITHREADED;

    asi_log::Log("HookedCreateDevice: Original BehaviorFlags = 0x%08X, New BehaviorFlags = 0x%08X\n",
                 behaviorFlags, newBehaviorFlags);

    // Call original CreateDevice with modified flags
    if (g_originalCreateDevice)
        return g_originalCreateDevice(d3d, adapter, deviceType, focusWindow, newBehaviorFlags, presentParams, device);

    // Fallback: call through vtable
    typedef HRESULT(APIENTRY* CreateDeviceLocalFn)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
    void** vtable = *reinterpret_cast<void***>(d3d);
    CreateDeviceLocalFn createDeviceFn = reinterpret_cast<CreateDeviceLocalFn>(vtable[16]);
    if (createDeviceFn && createDeviceFn != &HookedCreateDevice)
        return createDeviceFn(d3d, adapter, deviceType, focusWindow, newBehaviorFlags, presentParams, device);

    return D3DERR_INVALIDCALL;
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

// Patch Present and CreateDevice by replacing vtable entries
void HookPresent()
{
    // Create dummy D3D9 object to get vtables
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d)
        return;

    // Hook IDirect3D9::CreateDevice (index 16) to add D3DCREATE_MULTITHREADED flag
    void** d3dVTable = *reinterpret_cast<void***>(d3d);
    if (d3dVTable)
    {
        void* originalCreateDevicePtr = d3dVTable[16];
        g_originalCreateDevice = reinterpret_cast<CreateDeviceFn>(originalCreateDevicePtr);

        if (MakeVTableHook(d3dVTable, 16, reinterpret_cast<void*>(&HookedCreateDevice), &g_savedCreateDevicePtr))
        {
            g_createDeviceHooked.store(true);
            g_createDeviceVTableEntry = &d3dVTable[16];
            asi_log::Log("HookPresent: IDirect3D9::CreateDevice hooked successfully\n");
        }
        else
        {
            asi_log::Log("HookPresent: Failed to hook IDirect3D9::CreateDevice\n");
        }
    }

    // Now create dummy device to get IDirect3DDevice9 vtable
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
    void** deviceVTable = *reinterpret_cast<void***>(dummyDevice);
    if (!deviceVTable)
    {
        dummyDevice->Release();
        d3d->Release();
        return;
    }

    // Save original Present (index 17)
    void* originalPtr = deviceVTable[17];
    g_originalPresent = reinterpret_cast<PresentFn>(originalPtr);

    // Install Present hook
    if (MakeVTableHook(deviceVTable, 17, reinterpret_cast<void*>(&HookedPresent), &g_savedOriginalPtr))
    {
        g_vtableHooked.store(true);
        g_patchedVTableEntry = &deviceVTable[17];
        asi_log::Log("HookPresent: IDirect3DDevice9::Present hooked successfully\n");
    }
    else
    {
        asi_log::Log("HookPresent: Failed to hook IDirect3DDevice9::Present\n");
    }

    // Clean up dummy objects
    dummyDevice->Release();
    d3d->Release();
}

// Restore hooks (call on DLL unload)
void UnhookPresent()
{
    // Unhook CreateDevice
    if (g_createDeviceHooked.load() && g_createDeviceVTableEntry && g_savedCreateDevicePtr)
    {
        void** slot = g_createDeviceVTableEntry;
        void** vtable = slot - 16; // reverse offset

        if (UnmakeVTableHook(vtable, 16, g_savedCreateDevicePtr))
            asi_log::Log("UnhookPresent: CreateDevice vtable slot restored\n");
        else
            asi_log::Log("UnhookPresent: Failed to restore CreateDevice vtable slot\n");

        g_createDeviceHooked.store(false);
        g_createDeviceVTableEntry = nullptr;
        g_savedCreateDevicePtr = nullptr;
    }

    // Unhook Present
    if (g_vtableHooked.load() && g_patchedVTableEntry && g_savedOriginalPtr)
    {
        void** slot = g_patchedVTableEntry;
        void** vtable = slot - 17; // reverse offset

        if (UnmakeVTableHook(vtable, 17, g_savedOriginalPtr))
            asi_log::Log("UnhookPresent: Present vtable slot restored\n");
        else
            asi_log::Log("UnhookPresent: Failed to restore Present vtable slot\n");

        g_vtableHooked.store(false);
        g_patchedVTableEntry = nullptr;
        g_savedOriginalPtr = nullptr;
    }

    g_originalPresent = nullptr;
    g_originalCreateDevice = nullptr;
}

// Updated DllMain to clean up on detach
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);

        // Initialize MinHook
        MH_STATUS status = MH_Initialize();
        if (status != MH_OK)
        {
            asi_log::Log("DllMain: Failed to initialize MinHook: %s\n", MH_StatusToString(status));
            return FALSE;
        }
        asi_log::Log("DllMain: MinHook initialized successfully\n");

        CreateThread(nullptr, 0, [](LPVOID) -> DWORD
        {
            HookPresent(); // install vtable patching on a background thread
            return 0;
        }, nullptr, 0, nullptr);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        // CRITICAL: Set shutdown flag FIRST to prevent any cleanup operations
        // This tells CustomTextureLoader to skip ALL cleanup (D3D, CRT, etc.)
        CustomTextureLoader::SetShuttingDown();

        // Clear g_features vector to call destructors while CRT is still valid
        // The destructors will see g_isShuttingDown=true and skip cleanup
        g_features.clear();

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

        // Uninitialize MinHook
        MH_Uninitialize();
    }

    return TRUE;
}
