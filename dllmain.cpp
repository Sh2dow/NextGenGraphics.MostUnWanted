// Minimal reimplementation of the NextGenGraphics.Carbon plugin.
#include <windows.h>
#include <vector>
#include <memory>
#include <atomic>
#include "NFSC_PreFEngHook.h"
#include "features.h"
#include "CustomTextureLoader.h"
#include "WriteProtectScope.h"
#include "minhook/include/MinHook.h"

using namespace ngg::carbon;

// TODO: Port full initialization logic from sub_10077220
static std::vector<std::unique_ptr<ngg::common::Feature>> g_features;

// Original function pointers (set by MinHook)
typedef HRESULT (APIENTRY*PresentFn)(IDirect3DDevice9*, CONST RECT*, CONST RECT*, HWND, CONST RGNDATA*);
PresentFn g_originalPresent = nullptr;
static std::atomic g_vtableHooked{false};

typedef HRESULT (APIENTRY*CreateDeviceFn)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*,
                                          IDirect3DDevice9**);
CreateDeviceFn g_originalCreateDevice = nullptr;
static std::atomic g_createDeviceHooked{false};

typedef HRESULT (STDMETHODCALLTYPE*SetTextureFn)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
SetTextureFn g_originalSetTexture = nullptr;
static std::atomic g_setTextureHooked{false};

bool triedInit = false;


static void Initialize()
{
    using namespace ngg::carbon::features;
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
    HRESULT result = D3DERR_INVALIDCALL;
    if (g_originalCreateDevice)
    {
        result = g_originalCreateDevice(d3d, adapter, deviceType, focusWindow, newBehaviorFlags, presentParams, device);
    }
    else
    {
        // Fallback: call through vtable
        typedef HRESULT (APIENTRY*CreateDeviceLocalFn)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*,
                                                       IDirect3DDevice9**);
        void** vtable = *reinterpret_cast<void***>(d3d);
        CreateDeviceLocalFn createDeviceFn = reinterpret_cast<CreateDeviceLocalFn>(vtable[16]);
        if (createDeviceFn && createDeviceFn != &HookedCreateDevice)
            result = createDeviceFn(d3d, adapter, deviceType, focusWindow, newBehaviorFlags, presentParams, device);
    }

    // Device created successfully - CustomTextureLoader will start IOCP loading on first Present call
    // (no need to call SetD3DDevice here)

    return result;
}

// Hooked SetTexture - intercepts all texture sets and swaps with custom textures
HRESULT STDMETHODCALLTYPE HookedSetTexture(IDirect3DDevice9* device, DWORD stage, IDirect3DBaseTexture9* texture)
{
    // Debug: Track call count
    static std::atomic<int> hookCallCount{0};
    static bool loggedHookCallCount = false;

    int currentCount = hookCallCount.fetch_add(1);
    if (!loggedHookCallCount && currentCount >= 1)  // Log immediately on first call!
    {
        asi_log::Log("HookedSetTexture called %d times - HOOK IS WORKING!", currentCount);
        loggedHookCallCount = true;
    }

    // Let CustomTextureLoader check if we have a custom replacement
    IDirect3DBaseTexture9* replacementTexture = CustomTextureLoader::OnSetTexture(texture);

    // Use replacement if available, otherwise use original
    IDirect3DBaseTexture9* finalTexture = replacementTexture ? replacementTexture : texture;

    // Call original SetTexture with potentially swapped texture
    if (g_originalSetTexture)
        return g_originalSetTexture(device, stage, finalTexture);

    // Fallback: call through device's vtable
    typedef HRESULT (STDMETHODCALLTYPE*SetTextureLocalFn)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
    void** vtable = *reinterpret_cast<void***>(device);
    SetTextureLocalFn setTextureFn = reinterpret_cast<SetTextureLocalFn>(vtable[65]);
    if (setTextureFn && setTextureFn != &HookedSetTexture)
        return setTextureFn(device, stage, finalTexture);

    return D3D_OK;
}

// Hooked Present (unchanged behaviour)
HRESULT APIENTRY HookedPresent(IDirect3DDevice9* device, CONST RECT* src, CONST RECT* dest, HWND wnd,
                               CONST RGNDATA* dirty)
{
    OnPresent(); // trigger Initialize logic
    CustomTextureLoader::SetD3DDevice(device);

    // NOTE: SetTexture hook is now installed in CustomTextureLoader.cpp::SetD3DDevice()
    // This is because the hook needs to check texture type vs stage, which is handled there

    // DEBUG: Log what we're about to call
// #if _DEBUG
//     asi_log::Log("HookedPresent: About to call g_originalPresent = %p with device = %p\n", g_originalPresent, device);
// #endif
    
    // Call the original Present if we have it; fallback safe behavior if not
    if (g_originalPresent)
        return g_originalPresent(device, src, dest, wnd, dirty);

    // If no original, call through device's vtable (best-effort)
    typedef HRESULT (APIENTRY*PresentLocalFn)(IDirect3DDevice9*, CONST RECT*, CONST RECT*, HWND, CONST RGNDATA*);
    void** vtable = *reinterpret_cast<void***>(device);
    PresentLocalFn presentFn = reinterpret_cast<PresentLocalFn>(vtable[17]);
    if (presentFn && presentFn != &HookedPresent)
        return presentFn(device, src, dest, wnd, dirty);

    return D3D_OK;
}

// Hook Present and CreateDevice using MinHook
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
        void* createDeviceTarget = d3dVTable[16];

        MH_STATUS status = MH_CreateHook(createDeviceTarget, &HookedCreateDevice, reinterpret_cast<LPVOID*>(&g_originalCreateDevice));
        if (status == MH_OK)
        {
            status = MH_EnableHook(createDeviceTarget);
            if (status == MH_OK)
            {
                g_createDeviceHooked.store(true);
                asi_log::Log("HookPresent: IDirect3D9::CreateDevice hooked successfully via MinHook\n");
            }
            else
            {
                asi_log::Log("HookPresent: Failed to enable CreateDevice hook: %s\n", MH_StatusToString(status));
            }
        }
        else
        {
            asi_log::Log("HookPresent: Failed to create CreateDevice hook: %s\n", MH_StatusToString(status));
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

    // Hook Present (index 17) using MinHook - this will follow any existing hook chains
    void* presentTarget = deviceVTable[17];

    asi_log::Log("HookPresent: deviceVTable = %p, deviceVTable[17] = %p\n", deviceVTable, presentTarget);

    MH_STATUS status = MH_CreateHook(presentTarget, &HookedPresent, reinterpret_cast<LPVOID*>(&g_originalPresent));
    if (status == MH_OK)
    {
        status = MH_EnableHook(presentTarget);
        if (status == MH_OK)
        {
            g_vtableHooked.store(true);
            asi_log::Log("HookPresent: IDirect3DDevice9::Present hooked successfully via MinHook\n");
            asi_log::Log("HookPresent: g_originalPresent = %p\n", g_originalPresent);
        }
        else
        {
            asi_log::Log("HookPresent: Failed to enable Present hook: %s\n", MH_StatusToString(status));
        }
    }
    else
    {
        asi_log::Log("HookPresent: Failed to create Present hook: %s\n", MH_StatusToString(status));
    }

    // NOTE: SetTexture hook is installed dynamically in HookedPresent() when we get the real game device

    // Clean up dummy objects
    dummyDevice->Release();
    d3d->Release();
}

// Restore hooks (call on DLL unload)
void UnhookPresent()
{
    // Unhook all MinHook hooks
    if (g_createDeviceHooked.load() || g_vtableHooked.load() || g_setTextureHooked.load())
    {
        MH_STATUS status = MH_DisableHook(MH_ALL_HOOKS);
        if (status == MH_OK)
        {
            asi_log::Log("UnhookPresent: All hooks disabled successfully\n");
        }
        else
        {
            asi_log::Log("UnhookPresent: Failed to disable hooks: %s\n", MH_StatusToString(status));
        }

        g_createDeviceHooked.store(false);
        g_vtableHooked.store(false);
        g_setTextureHooked.store(false);
    }

    g_originalPresent = nullptr;
    g_originalCreateDevice = nullptr;
    g_originalSetTexture = nullptr;
}

// Updated DllMain to clean up on detach
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);

        // Initialize MinHook FIRST (before any hooks are installed)
        MH_STATUS status = MH_Initialize();
        if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED)
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
        // During DLL unload, the CRT and D3D are being torn down, and ANY cleanup
        // operations (even texture->Release()) can trigger STATUS_STACK_BUFFER_OVERRUN.
        CustomTextureLoader::SetShuttingDown();

        // Wait a bit for any in-flight operations to complete
        Sleep(100);

        // Clear features vector (this calls disable() on each feature, which will skip cleanup due to shutdown flag)
        g_features.clear();

        // Clean up CustomTextureLoader (will skip cleanup due to shutdown flag)
        CustomTextureLoader::Cleanup();

        // Unhook Present (this is safe because we control the vtable)
        UnhookPresent();

        // Uninitialize MinHook LAST (after all hooks are removed)
        MH_Uninitialize();
        asi_log::Log("DllMain: MinHook uninitialized\n");
    }

    return TRUE;
}
