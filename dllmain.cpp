// D3D9 wrapper-free reimplementation of NextGenGraphics.TextureLoader.
// Reads the IDirect3DDevice9* pointer directly from speed.exe's global
// data at D3DDEVICE_PTR.  A lightweight Direct3DCreate9 → CreateDevice
// hook (MinHook in-process, NOT a d3d9.dll proxy) forces the
// D3DCREATE_MULTITHREADED flag required for thread-safe D3DX usage.
#include <windows.h>
#include <vector>
#include <memory>
#include "Log.h"
#include "includes/minhook/include/MinHook.h"
#include "features.h"
#include "CustomTextureLoader.h"

#ifdef GAME_MW
#include "NFSMW_PreFEngHook.h"
using namespace ngg::mw;
#elif GAME_CARBON
#include "NFSC_PreFEngHook.h"
using namespace ngg::carbon;
#endif

// ---- D3D9 device-creation hooks (force D3DCREATE_MULTITHREADED) ----

typedef IDirect3D9* (WINAPI* Direct3DCreate9Fn)(UINT);
static Direct3DCreate9Fn g_originalDirect3DCreate9 = nullptr;

typedef HRESULT(APIENTRY* CreateDeviceFn)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
static CreateDeviceFn g_originalCreateDevice = nullptr;
static bool g_createDeviceHooked = false;

static HRESULT APIENTRY HookedCreateDevice(
    IDirect3D9* d3d, UINT adapter, D3DDEVTYPE deviceType, HWND focusWindow,
    DWORD behaviorFlags, D3DPRESENT_PARAMETERS* presentParams, IDirect3DDevice9** device)
{
    DWORD newFlags = behaviorFlags | D3DCREATE_MULTITHREADED;
    return g_originalCreateDevice(d3d, adapter, deviceType, focusWindow, newFlags, presentParams, device);
}

static IDirect3D9* WINAPI HookedDirect3DCreate9(UINT sdkVersion)
{
    if (!g_originalDirect3DCreate9)
        return nullptr;

    IDirect3D9* d3d = g_originalDirect3DCreate9(sdkVersion);
    if (!d3d || g_createDeviceHooked)
        return d3d;

    // Hook IDirect3D9::CreateDevice via MinHook on the vtable function.
    // This is an in-process hook — NOT a d3d9.dll proxy wrapper.
    void** vtable = *reinterpret_cast<void***>(d3d);
    if (vtable)
    {
        MH_STATUS s = MH_CreateHook(vtable[16], reinterpret_cast<void*>(&HookedCreateDevice),
                                     reinterpret_cast<void**>(&g_originalCreateDevice));
        if (s == MH_OK)
        {
            s = MH_EnableHook(vtable[16]);
            if (s == MH_OK)
                g_createDeviceHooked = true;
        }
    }

    return d3d;
}

// ---- Feature management ----

static std::vector<std::unique_ptr<ngg::common::Feature>> g_features;

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

// ---- Init thread ----

static HANDLE g_shutdownEvent = nullptr;  // signalled on DLL_PROCESS_DETACH

static void InitThread()
{
    // Step 1: Install Direct3DCreate9 hook as early as possible so we
    //         intercept the game's device creation and force MULTITHREADED.
    //         This is an in-process MinHook, NOT a d3d9.dll proxy.
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED)
    {
        asi_log::Log("InitThread: MH_Initialize failed: %d\n", status);
        return;
    }

    HMODULE hD3D9 = GetModuleHandleW(L"d3d9.dll");
    if (!hD3D9)
        hD3D9 = LoadLibraryW(L"d3d9.dll");
    if (hD3D9)
    {
        MH_CreateHookApi(L"d3d9.dll", "Direct3DCreate9",
            reinterpret_cast<LPVOID>(&HookedDirect3DCreate9),
            reinterpret_cast<LPVOID*>(&g_originalDirect3DCreate9));
        MH_EnableHook(MH_ALL_HOOKS);
    }

    // Step 2: Poll for the D3D device pointer in speed.exe's global data.
    //         Once the game has called Direct3DCreate9 (intercepted above),
    //         the created device will be stored at D3DDEVICE_PTR.
    IDirect3DDevice9* device = nullptr;
    for (int retries = 0; retries < 200; retries++)
    {
        device = *reinterpret_cast<IDirect3DDevice9**>(D3DDEVICE_PTR);
        if (device)
        {
            void** vtable = *reinterpret_cast<void***>(device);
            if (vtable)
                break;
        }
        device = nullptr;

        if (WaitForSingleObject(g_shutdownEvent, 100) == WAIT_OBJECT_0)
            return;  // DLL unloading
    }

    if (!device)
    {
        asi_log::Log("InitThread: Failed to find D3D device at 0x%08X\n", D3DDEVICE_PTR);
        return;
    }

    asi_log::Log("InitThread: D3D device found\n");

    // Step 3: Create features, install game-code hooks, start async loading
    Initialize();
    CustomTextureLoader::SetD3DDevice(device);

    asi_log::Log("InitThread: Initialization complete\n");
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);

        g_shutdownEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

        HANDLE hThread = CreateThread(nullptr, 0, [](LPVOID) -> DWORD
        {
            InitThread();
            return 0;
        }, nullptr, 0, nullptr);

        if (hThread)
        {
            CloseHandle(hThread);  // we only wait via the event, not through the handle
        }
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        // Signal the init thread to bail if it is still running
        if (g_shutdownEvent)
            SetEvent(g_shutdownEvent);

        // CRITICAL: Set shutdown flag FIRST to prevent any cleanup operations
        CustomTextureLoader::SetShuttingDown();

        // Clear features (destructors see g_isShuttingDown=true and skip D3D cleanup)
        g_features.clear();

        if (g_shutdownEvent)
            CloseHandle(g_shutdownEvent);

        // NOTE: Game-code hooks (HookLoad, HookSwap) and the Direct3DCreate9
        // hook installed via MinHook are NOT uninstalled here.  On process
        // exit the OS reclaims everything.  For hot-reload scenarios,
        // feature->disable() should be called, which runs UninstallTextureHooks().
    }

    return TRUE;
}
