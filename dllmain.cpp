// Minimal reimplementation of the NextGenGraphics.TextureLoader plugin.
#include <windows.h>
#include <vector>
#include <memory>
#include <atomic>
#include "Log.h"
#include "includes/minhook/include/MinHook.h"
#include "features.h"
#include "CustomTextureLoader.h"
#include "WriteProtectScope.h"

#ifdef GAME_MW
#include "NFSMW_PreFEngHook.h"
using namespace ngg::mw;
#elif GAME_CARBON
#include "NFSC_PreFEngHook.h"
using namespace ngg::carbon;
#endif


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

// Original Direct3DCreate9 export pointer (hooked via MinHook)
typedef IDirect3D9* (WINAPI* Direct3DCreate9Fn)(UINT);
static Direct3DCreate9Fn g_originalDirect3DCreate9 = nullptr;

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

// Hooked Present - triggers initialization and forwards to original
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

// Hooked CreateDevice - adds D3DCREATE_MULTITHREADED flag and installs Present hook
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

    HRESULT hr = D3DERR_INVALIDCALL;

    // Call original CreateDevice with modified flags
    if (g_originalCreateDevice)
    {
        hr = g_originalCreateDevice(d3d, adapter, deviceType, focusWindow, newBehaviorFlags, presentParams, device);
    }
    else
    {
        // Fallback: call through vtable (should not normally happen)
        typedef HRESULT(APIENTRY* CreateDeviceLocalFn)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
        void** vtable = *reinterpret_cast<void***>(d3d);
        CreateDeviceLocalFn createDeviceFn = reinterpret_cast<CreateDeviceLocalFn>(vtable[16]);
        if (createDeviceFn && createDeviceFn != &HookedCreateDevice)
            hr = createDeviceFn(d3d, adapter, deviceType, focusWindow, newBehaviorFlags, presentParams, device);
    }

    if (SUCCEEDED(hr) && device && *device)
    {
        // Install Present hook on the first created device
        if (!g_vtableHooked.load())
        {
            void** deviceVTable = *reinterpret_cast<void***>(*device);
            if (deviceVTable)
            {
                void* originalPtr = deviceVTable[17];
                g_originalPresent = reinterpret_cast<PresentFn>(originalPtr);

                if (MakeVTableHook(deviceVTable, 17, reinterpret_cast<void*>(&HookedPresent), &g_savedOriginalPtr))
                {
                    g_vtableHooked.store(true);
                    g_patchedVTableEntry = &deviceVTable[17];
                    asi_log::Log("HookedCreateDevice: IDirect3DDevice9::Present hooked successfully\n");
                }
                else
                {
                    asi_log::Log("HookedCreateDevice: Failed to hook IDirect3DDevice9::Present\n");
                }
            }
        }
    }

    return hr;
}

// Hooked Direct3DCreate9  installs CreateDevice hook lazily when D3D9 is created
IDirect3D9* WINAPI HookedDirect3DCreate9(UINT sdkVersion)
{
    if (!g_originalDirect3DCreate9)
        return nullptr;

    IDirect3D9* d3d = g_originalDirect3DCreate9(sdkVersion);
    if (!d3d)
        return nullptr;

    // Install CreateDevice hook once on the first IDirect3D9 we see
    if (!g_createDeviceHooked.load())
    {
        void** vtable = *reinterpret_cast<void***>(d3d);
        if (vtable)
        {
            void* originalCreateDevicePtr = vtable[16];
            g_originalCreateDevice = reinterpret_cast<CreateDeviceFn>(originalCreateDevicePtr);

            if (MakeVTableHook(vtable, 16, reinterpret_cast<void*>(&HookedCreateDevice), &g_savedCreateDevicePtr))
            {
                g_createDeviceHooked.store(true);
                g_createDeviceVTableEntry = &vtable[16];
                asi_log::Log("HookedDirect3DCreate9: IDirect3D9::CreateDevice hooked successfully\n");
            }
            else
            {
                asi_log::Log("HookedDirect3DCreate9: Failed to hook IDirect3D9::CreateDevice\n");
            }
        }
    }

    return d3d;
}

// Install hook on Direct3DCreate9 using MinHook, ReShade-style
void HookPresent()
{
    asi_log::Log("HookPresent: Initializing MinHook and hooking Direct3DCreate9\n");

    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED)
    {
        asi_log::Log("HookPresent: MH_Initialize failed: %d\n", status);
        return;
    }

    // Ensure d3d9.dll is loaded in this process
    HMODULE hD3D9 = LoadLibraryW(L"d3d9.dll");
    if (!hD3D9)
    {
        asi_log::Log("HookPresent: LoadLibraryW(d3d9.dll) failed\n");
        return;
    }

    status = MH_CreateHookApi(L"d3d9.dll", "Direct3DCreate9",
        reinterpret_cast<LPVOID>(&HookedDirect3DCreate9),
        reinterpret_cast<LPVOID*>(&g_originalDirect3DCreate9));

    if (status != MH_OK && status != MH_ERROR_ALREADY_CREATED)
    {
        asi_log::Log("HookPresent: MH_CreateHookApi failed: %d\n", status);
        return;
    }

    status = MH_EnableHook(MH_ALL_HOOKS);
    if (status != MH_OK && status != MH_ERROR_ENABLED)
    {
        asi_log::Log("HookPresent: MH_EnableHook failed: %d\n", status);
        return;
    }

    asi_log::Log("HookPresent: Direct3DCreate9 hook installed\n");
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

        CreateThread(nullptr, 0, [](LPVOID) -> DWORD
        {
            // Wait for game to initialize before hooking Present
            // This prevents crashes with other mods that expect game state to be ready
            // BUT: either doesn't work or crash for teleport hook
            // Sleep(100);
            
            // Install D3D9 hook on a background thread (avoids heavy work in DllMain)
            HookPresent();
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
    }

    return TRUE;
}
