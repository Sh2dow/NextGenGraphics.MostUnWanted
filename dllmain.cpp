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

HRESULT STDMETHODCALLTYPE HookedSetTexture(IDirect3DDevice9* device, DWORD stage, IDirect3DBaseTexture9* texture);
HRESULT APIENTRY HookedPresent(IDirect3DDevice9* device, CONST RECT* src, CONST RECT* dest, HWND wnd, CONST RGNDATA* dirty);

// Crash logging (addresses only, no symbolization)
static void* g_exceptionHandler = nullptr;

static LONG WINAPI VectoredExceptionLogger(_EXCEPTION_POINTERS* info)
{
    static std::atomic<bool> inHandler{false};
    if (inHandler.exchange(true))
        return EXCEPTION_CONTINUE_SEARCH;

    if (!info || !info->ExceptionRecord)
    {
        inHandler.store(false);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
    {
        void* frames[32] = {};
        USHORT count = CaptureStackBackTrace(0, 32, frames, nullptr);

        char header[256] = {};
        _snprintf_s(header, sizeof(header), _TRUNCATE,
                    "Crash: AV at %p, access type=%lu, address=%p, stack frames=%u\n",
                    info->ExceptionRecord->ExceptionAddress,
                    info->ExceptionRecord->ExceptionInformation[0],
                    reinterpret_cast<void*>(info->ExceptionRecord->ExceptionInformation[1]),
                    count);
        OutputDebugStringA(header);

        for (USHORT i = 0; i < count; ++i)
        {
            HMODULE module = nullptr;
            char modulePath[MAX_PATH] = {};
            uintptr_t offset = 0;

            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                   reinterpret_cast<LPCSTR>(frames[i]),
                                   &module))
            {
                GetModuleFileNameA(module, modulePath, MAX_PATH);
                offset = reinterpret_cast<uintptr_t>(frames[i]) - reinterpret_cast<uintptr_t>(module);
            }

            char line[512] = {};
            if (modulePath[0])
            {
                _snprintf_s(line, sizeof(line), _TRUNCATE, "  #%02u %p (%s + 0x%X)\n",
                            i, frames[i], modulePath, static_cast<unsigned>(offset));
            }
            else
            {
                _snprintf_s(line, sizeof(line), _TRUNCATE, "  #%02u %p\n", i, frames[i]);
            }
            OutputDebugStringA(line);
        }
    }

    inHandler.store(false);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void InstallDeviceHooks(IDirect3DDevice9* device)
{
    if (!device)
        return;

    void** deviceVTable = *reinterpret_cast<void***>(device);
    if (!deviceVTable)
        return;

    // Hook Present (index 17) using MinHook - hook based on the actual game device
    if (!g_vtableHooked.load())
    {
        void* presentTarget = deviceVTable[17];
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
    }

    // Hook SetTexture (index 65) if needed
    if (!g_setTextureHooked.load())
    {
        void* setTextureTarget = deviceVTable[65];
        MH_STATUS status = MH_CreateHook(setTextureTarget, &HookedSetTexture, reinterpret_cast<LPVOID*>(&g_originalSetTexture));
        if (status == MH_OK)
        {
            status = MH_EnableHook(setTextureTarget);
            if (status == MH_OK)
            {
                g_setTextureHooked.store(true);
                asi_log::Log("HookPresent: IDirect3DDevice9::SetTexture hooked successfully via MinHook\n");
            }
            else
            {
                asi_log::Log("HookPresent: Failed to enable SetTexture hook: %s\n", MH_StatusToString(status));
            }
        }
        else
        {
            asi_log::Log("HookPresent: Failed to create SetTexture hook: %s\n", MH_StatusToString(status));
        }
    }
}

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

    // Device created successfully - install hooks on the actual game device
    if (SUCCEEDED(result) && device && *device)
        InstallDeviceHooks(*device);

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

        if (!g_exceptionHandler)
            g_exceptionHandler = AddVectoredExceptionHandler(1, VectoredExceptionLogger);

        {
            HMODULE module = nullptr;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                   reinterpret_cast<LPCSTR>(&HookedPresent),
                                   &module))
            {
                auto base = reinterpret_cast<uintptr_t>(module);
                asi_log::Log("DllMain: Module base=%p", module);
                asi_log::Log("DllMain: HookedPresent=%p (offset=0x%X)",
                             reinterpret_cast<void*>(&HookedPresent),
                             static_cast<unsigned>(reinterpret_cast<uintptr_t>(&HookedPresent) - base));
                asi_log::Log("DllMain: HookedSetTexture=%p (offset=0x%X)",
                             reinterpret_cast<void*>(&HookedSetTexture),
                             static_cast<unsigned>(reinterpret_cast<uintptr_t>(&HookedSetTexture) - base));
                asi_log::Log("DllMain: HookedCreateDevice=%p (offset=0x%X)",
                             reinterpret_cast<void*>(&HookedCreateDevice),
                             static_cast<unsigned>(reinterpret_cast<uintptr_t>(&HookedCreateDevice) - base));
                asi_log::Log("DllMain: HookPresent=%p (offset=0x%X)",
                             reinterpret_cast<void*>(&HookPresent),
                             static_cast<unsigned>(reinterpret_cast<uintptr_t>(&HookPresent) - base));
                asi_log::Log("DllMain: InstallDeviceHooks=%p (offset=0x%X)",
                             reinterpret_cast<void*>(&InstallDeviceHooks),
                             static_cast<unsigned>(reinterpret_cast<uintptr_t>(&InstallDeviceHooks) - base));
                asi_log::Log("DllMain: VectoredExceptionLogger=%p (offset=0x%X)",
                             reinterpret_cast<void*>(&VectoredExceptionLogger),
                             static_cast<unsigned>(reinterpret_cast<uintptr_t>(&VectoredExceptionLogger) - base));
                asi_log::Log("DllMain: Initialize=%p (offset=0x%X)",
                             reinterpret_cast<void*>(&Initialize),
                             static_cast<unsigned>(reinterpret_cast<uintptr_t>(&Initialize) - base));
                asi_log::Log("DllMain: UnhookPresent=%p (offset=0x%X)",
                             reinterpret_cast<void*>(&UnhookPresent),
                             static_cast<unsigned>(reinterpret_cast<uintptr_t>(&UnhookPresent) - base));
            }
        }

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

        if (g_exceptionHandler)
        {
            RemoveVectoredExceptionHandler(g_exceptionHandler);
            g_exceptionHandler = nullptr;
        }
    }

    return TRUE;
}
