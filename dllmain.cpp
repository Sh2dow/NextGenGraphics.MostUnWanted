// Minimal reimplementation of the NextGenGraphics.MostWanted plugin.
#include <windows.h>
#include <vector>
#include <memory>
#include "features.h"
#include "CustomTextureLoader.h"
#include "NFSMW_PreFEngHook.h"
#include "MinHook.h"

using namespace ngg::mw;

// TODO: Port full initialization logic from sub_10077220
static std::vector<std::unique_ptr<ngg::common::Feature>> g_features;

bool triedInit = false;

// Original Present function pointer
typedef HRESULT(APIENTRY* PresentFn)(IDirect3DDevice9*, CONST RECT*, CONST RECT*, HWND, CONST RGNDATA*);
PresentFn g_originalPresent = nullptr;

static void Initialize()
{
    using namespace ngg::mw::features;

    g_features.emplace_back(std::make_unique<CustomTextureLoader>());

    OutputDebugStringA("Setting up hooks\n");

    for (const auto& feature : g_features)
    {
        OutputDebugStringA(feature->name());
        OutputDebugStringA(" initialized\n");
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

// Hooked Present
HRESULT APIENTRY HookedPresent(IDirect3DDevice9* device, CONST RECT* src, CONST RECT* dest, HWND wnd, CONST RGNDATA* dirty)
{
    OnPresent(); // <- trigger Initialize logic

    // Provide D3D device to CustomTextureLoader
    CustomTextureLoader::SetD3DDevice(device);

    return g_originalPresent(device, src, dest, wnd, dirty);
}

void HookPresent()
{
    // Create dummy device to get vtable
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
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

    void** vtable = *reinterpret_cast<void***>(dummyDevice);
    g_originalPresent = reinterpret_cast<PresentFn>(vtable[17]); // Present is index 17

    MH_Initialize();
    MH_CreateHook(vtable[17], &HookedPresent, reinterpret_cast<void**>(&g_originalPresent));
    MH_EnableHook(vtable[17]);

    dummyDevice->Release();
    d3d->Release();
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);

        CreateThread(nullptr, 0, [](LPVOID) -> DWORD
        {
            HookPresent(); // <--- Just hook Present, defer actual init
            return 0;
        }, nullptr, 0, nullptr);
    }

    return TRUE;
}
