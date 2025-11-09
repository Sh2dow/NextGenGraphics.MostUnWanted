#pragma once
#include <windows.h>

namespace ngg
{
    namespace carbon
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

        // Convenience helpers mirroring the addressing style used in the
        // VerbleHackNFSMW project. `MW_EXE_ADDR`/`MW_MODULE_ADDR` convert the
        // static addresses found in the disassembly into runtime addresses.
#define CARBON_EXE_ADDR(addr) (ngg::mw::AdjustAddress(addr))
#define CARBON_MODULE_ADDR(addr) (ngg::mw::AdjustModuleAddress(addr))
#define CARBON_EXE_PTR(type, addr) reinterpret_cast<type*>(MW_EXE_ADDR(addr))
#define CARBON_MODULE_PTR(type, addr) reinterpret_cast<type*>(MW_MODULE_ADDR(addr))
    }
}
