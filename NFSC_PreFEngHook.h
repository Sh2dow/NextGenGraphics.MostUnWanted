#pragma once

// ============================================================================
// NFS Carbon Address Mapping for CustomTextureLoader
// ============================================================================
// This file maps Most Wanted addresses to their Carbon equivalents.
//
// ANALYSIS NOTES:
// - Carbon's rendering architecture is similar to MW but with different addresses
// - Main rendering function: sub_730CB0 (called during FE rendering)
// - At 0x731138: call to sub_5915D0 - this is the HOOK_SWAP_ADDR
// - At 0x730D60: writes to dword_AB0BA4 (GAME_CONTEXT_PTR) - stores shader context
//
// REMAINING WORK:
// 1. Find HOOK_LOAD_ADDR - need to locate a nullsub or placeholder function
//    called during graphics initialization (similar to MW's nullsub_33 at 0x6C3A30)
// 2. Find GAME_TEX_WRAPPER_1/2/3 - need to find global pointers to texture wrappers
//    (similar to MW's 0x982CB4, 0x982CB8, 0x982CC0)
//    These are likely set dynamically during rendering and may require runtime analysis
// ============================================================================

namespace ngg
{
    namespace carbon
    {
        constexpr uintptr_t gDevice = 0x00AB0ABC;
        constexpr uintptr_t pVisualTreatmentPlat = 0x00AB0B80;

        constexpr uintptr_t FEMANAGER_INSTANCE_ADDR = 0x00A97A7C;
#define USERPROFILE_POINTER (*(int*)(*(int*)((*(int*)FEMANAGER_INSTANCE_ADDR) + 0xD4)))
        constexpr uintptr_t USERPROFILE_OFFSET = 0x0C; // needs confirmation via IDA
        #define TIMEOFDAY_OFFSET (USERPROFILE_POINTER + 0x241EC); // already known from Carbon
        // constexpr uintptr_t VTABLE_VOLevelOfDetail = 0x0089BB4C;
        #define SetTimeOfDayAddress (USERPROFILE_POINTER + 0x241EC); // already known from Carbon
        
        constexpr uintptr_t LoadedFlagMaybe = 0x00AB0B25;    // reinit_renderer from hyperlinked codebase

        // Frontend rendering addresses (already known)
        constexpr uintptr_t FEMANAGER_RENDER_HOOKADDR1 = 0x00731138;  // Call to sub_5915D0 in sub_730CB0
        constexpr uintptr_t FEMANAGER_RENDER_HOOKADDR2 = 0x00731138;  // Same as above
        constexpr uintptr_t FEMANAGER_RENDER_ADDRESS = 0x005915D0;    // FE rendering function

        // CustomTextureLoader hook addresses (Carbon equivalents of MW addresses)
        // MW: HOOK_LOAD_ADDR = 0x6C3A30 (nullsub_33 - just "retn") - called during graphics init from sub_6BD7C0
        // Carbon: TBD - need to find equivalent placeholder function or nullsub
        // NOTE: May not exist in Carbon - might need to hook a different location or skip this hook entirely
        constexpr uintptr_t HOOK_LOAD_ADDR = 0x00000000;      // TODO: Find Carbon equivalent of nullsub_33

        // MW: HOOK_SWAP_ADDR = 0x6C6C8D (before epilogue of sub_6C68B0) - material rendering hook
        // Carbon: At 0x731138 in sub_730CB0, there's a call to sub_5915D0 (FE rendering)
        // This is the equivalent location for texture swapping during FE rendering
        // Confirmed by IDA analysis and hyperlinked project (effect.hpp shows current_effect_ at 0xAB0BA4)
        constexpr uintptr_t HOOK_SWAP_ADDR = 0x731138;        // CONFIRMED: Call to sub_5915D0 in sub_730CB0

        constexpr uintptr_t GAME_GET_TEXTURE_INFO_ADDR = 0x0055CFD0;

        // Game texture addresses
        // MW: Uses wrapper structures at 0x982CB4, 0x982CB8, 0x982CC0 with hash at offset +0x24
        // Carbon: Uses DIRECT texture pointers (no wrapper structures!)
        // From hyperlinked/renderer/effect.hpp - these are IDirect3DTexture9** pointers
        // Carbon stores the ACTUAL texture pointers here, not wrapper structures
        constexpr uintptr_t GAME_TEX_WRAPPER_1 = 0x00B1DB78;  // last_submitted_diffuse_map_
        constexpr uintptr_t GAME_TEX_WRAPPER_2 = 0x00B1DB7C;  // last_submitted_normal_map_
        constexpr uintptr_t GAME_TEX_WRAPPER_3 = 0x00B1DB84;  // last_submitted_specular_map_

        // Game context for material API
        // MW: dword_982C80 - global pointer to game context/state object
        // Carbon: dword_AB0BA4 - stores current effect/shader pointer
        // CONFIRMED by hyperlinked/renderer/effect.hpp:
        //   static inline effect*& current_effect_ = *reinterpret_cast<effect**>(0x00AB0BA4);
        // Written at 0x730D60 in sub_730CB0: mov dword_AB0BA4, ecx (where ecx = ShaderLib::pFE)
        // Also written at 0x730E85, 0x730F02, 0x730F29, 0x731025, 0x7310ED, 0x731164 with different shader pointers
        constexpr uintptr_t GAME_CONTEXT_PTR = 0x00AB0BA4;    // CONFIRMED: Pointer to current effect/shader (effect*)
    }
}
