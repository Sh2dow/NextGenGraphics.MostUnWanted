#include "CustomTextureHooks.h"

#include <windows.h>
#include "includes/minhook/include/MinHook.h"
#include "Log.h"

#ifdef GAME_MW
#include "NFSMW_PreFEngHook.h"
namespace ngg {
    namespace mw {
#elif GAME_CARBON
#include "NFSC_PreFEngHook.h"
    namespace ngg {
        namespace carbon {
#endif

static bool g_hookLoadInstalled_state = false;
static bool g_hookSwapInstalled_state = false;

static const char* MH_StatusToStringLocal(MH_STATUS s)
{
    switch (s) {
    case MH_OK: return "MH_OK";
    case MH_ERROR_ALREADY_INITIALIZED: return "ALREADY_INITIALIZED";
    case MH_ERROR_NOT_INITIALIZED: return "NOT_INITIALIZED";
    case MH_ERROR_ALREADY_CREATED: return "ALREADY_CREATED";
    case MH_ERROR_NOT_CREATED: return "NOT_CREATED";
    case MH_ERROR_ENABLED: return "ENABLED";
    case MH_ERROR_DISABLED: return "DISABLED";
    case MH_ERROR_NOT_EXECUTABLE: return "NOT_EXECUTABLE";
    case MH_ERROR_UNSUPPORTED_FUNCTION: return "UNSUPPORTED_FUNCTION";
    case MH_ERROR_MEMORY_ALLOC: return "MEMORY_ALLOC";
    case MH_ERROR_MEMORY_PROTECT: return "MEMORY_PROTECT";
    case MH_ERROR_MODULE_NOT_FOUND: return "MODULE_NOT_FOUND";
    case MH_ERROR_FUNCTION_NOT_FOUND: return "FUNCTION_NOT_FOUND";
    default: return "UNKNOWN";
    }
}

bool InstallTextureHooks(void* pHookLoad, void* pHookSwap)
{
    asi_log::Log("CustomTextureHooks: Installing hooks...");

    MH_STATUS status = MH_CreateHook(reinterpret_cast<void*>(HOOK_LOAD_ADDR), pHookLoad, nullptr);
    if (status == MH_OK) {
        status = MH_EnableHook(reinterpret_cast<void*>(HOOK_LOAD_ADDR));
        if (status == MH_OK) g_hookLoadInstalled_state = true;
    }
    if (status != MH_OK) {
        asi_log::Log("CustomTextureHooks: FAILED to hook HOOK_LOAD_ADDR: %s", MH_StatusToStringLocal(status));
    }

    status = MH_CreateHook(reinterpret_cast<void*>(HOOK_SWAP_ADDR), pHookSwap, nullptr);
    if (status == MH_OK) {
        status = MH_EnableHook(reinterpret_cast<void*>(HOOK_SWAP_ADDR));
        if (status == MH_OK) g_hookSwapInstalled_state = true;
    }
    if (status != MH_OK) {
        asi_log::Log("CustomTextureHooks: FAILED to hook HOOK_SWAP_ADDR: %s", MH_StatusToStringLocal(status));
    }

    const bool ok = g_hookLoadInstalled_state && g_hookSwapInstalled_state;
    asi_log::Log(ok ? "CustomTextureHooks: Hooks installed" : "CustomTextureHooks: One or more hooks failed");
    return ok;
}

void UninstallTextureHooks()
{
    // Always try to unhook; MinHook functions are idempotent for already-removed hooks.
    if (g_hookLoadInstalled_state) {
        MH_DisableHook(reinterpret_cast<void*>(HOOK_LOAD_ADDR));
        MH_RemoveHook(reinterpret_cast<void*>(HOOK_LOAD_ADDR));
        g_hookLoadInstalled_state = false;
        asi_log::Log("CustomTextureHooks: Hook 1 (HOOK_LOAD_ADDR) removed");
    } else {
        // Attempt removal anyway to be safe in case state desync
        MH_DisableHook(reinterpret_cast<void*>(HOOK_LOAD_ADDR));
        MH_RemoveHook(reinterpret_cast<void*>(HOOK_LOAD_ADDR));
    }

    if (g_hookSwapInstalled_state) {
        MH_DisableHook(reinterpret_cast<void*>(HOOK_SWAP_ADDR));
        MH_RemoveHook(reinterpret_cast<void*>(HOOK_SWAP_ADDR));
        g_hookSwapInstalled_state = false;
        asi_log::Log("CustomTextureHooks: Hook 2 (HOOK_SWAP_ADDR) removed");
    } else {
        MH_DisableHook(reinterpret_cast<void*>(HOOK_SWAP_ADDR));
        MH_RemoveHook(reinterpret_cast<void*>(HOOK_SWAP_ADDR));
    }
}

} // namespace mw
} // namespace ngg

