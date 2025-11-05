#pragma once
#include <windows.h>

namespace ngg
{
    namespace mw
    {
        inline uintptr_t GetExeBase()
        {
            static uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(NULL));
            return base;
        }

        inline uintptr_t GetModuleBase()
        {
            static uintptr_t base = 0;
            if (!base)
            {
                HMODULE hModule = nullptr;
                GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                   reinterpret_cast<LPCSTR>(&GetModuleBase),
                                   &hModule);
                base = reinterpret_cast<uintptr_t>(hModule);
            }
            return base;
        }

        constexpr uintptr_t kStaticExeBase = 0x400000;
        // Static image base of the plugin as observed in the disassembly
        // (addresses such as 0x10112277 are relative to this base).
        // Image base of the released plugin as reported by PE headers
        constexpr uintptr_t kStaticModuleBase = 0x10000000;

        inline uintptr_t AdjustAddress(uintptr_t staticAddress)
        {
            uintptr_t calcAddr = staticAddress - kStaticExeBase + GetExeBase();
            return calcAddr;
        }

        inline uintptr_t AdjustModuleAddress(uintptr_t staticAddress)
        {
            uintptr_t calcAddr = staticAddress - kStaticModuleBase + GetModuleBase();
            return calcAddr;
        }
        
        // Game addresses
        constexpr uintptr_t HOOK_LOAD_ADDR = 0x6C3A30;      // nullsub_33
        constexpr uintptr_t HOOK_SWAP_ADDR = 0x6C6C97;      // Epilogue of sub_6C68B0

        // Game texture wrapper addresses
        constexpr uintptr_t GAME_TEX_WRAPPER_1 = 0x982CB4;  // Pointer to diffuse texture wrapper
        constexpr uintptr_t GAME_TEX_WRAPPER_2 = 0x982CB8;  // Pointer to normal texture wrapper
        constexpr uintptr_t GAME_TEX_WRAPPER_3 = 0x982CC0;  // Pointer to specular texture wrapper

        // Game context for material API
        constexpr uintptr_t GAME_CONTEXT_PTR = 0x982C80;  // Pointer to game context/state object

    }
}
