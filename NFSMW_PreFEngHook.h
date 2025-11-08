#pragma once

namespace ngg
{
    namespace mw
    {
        constexpr uintptr_t FEMANAGER_INSTANCE_ADDR = 0x0091CAE0;
        // constexpr uintptr_t USERPROFILE_PTR = (*(int*)(*(int*)((*(int*)FEMANAGER_INSTANCE_ADDR) + 0xD4))); // NFSC style
        constexpr uintptr_t PROFILEMANAGER_INSTANCE_ADDR = 0x00925E5C;
        constexpr uintptr_t USERPROFILE_OFFSET = 0x0C; // needs confirmation via IDA
        constexpr uintptr_t MAINPROFILEMANAGER_VTBL = 0x0089C21C;
        constexpr uintptr_t TIMEOFDAY_OFFSET = 0x241EC; // already known from Carbon
        constexpr uintptr_t VTABLE_VOLevelOfDetail = 0x0089BB4C;
        constexpr uintptr_t SetTimeOfDayAddress = 0x7696B0;
        
        constexpr uintptr_t kHudLeft = 0x5D35C0A8;
        constexpr uintptr_t kHudTop = 0x5D35C0B0;

        constexpr uintptr_t kEnableFeShadowsFlag = 0x10112277;
        constexpr uintptr_t kFrontEndVehicleFlaresFlag = 0x10112367;
        constexpr uintptr_t kTimeOfDaySliderFlag = 0x101125D7;

        constexpr uintptr_t kMotionBlur_BE79 = 0x006DBE79;
        constexpr uintptr_t kMotionBlur_BE7B = 0x006DBE7B;
        constexpr uintptr_t kMotionBlur_F1D2 = 0x006DF1D2;

        constexpr uintptr_t LoadedFlagMaybe = 0x982C39;

        
        
        // Game addresses
        constexpr uintptr_t HOOK_LOAD_ADDR = 0x6C3A30;      // nullsub_33
        constexpr uintptr_t HOOK_SWAP_ADDR = 0x6C6C8D;      // BEFORE epilogue of sub_6C68B0
        constexpr uintptr_t GAME_GET_TEXTURE_INFO_ADDR = 0x00503400;

        // Game texture wrapper addresses
        constexpr uintptr_t GAME_TEX_WRAPPER_1 = 0x982CB4;  // Pointer to diffuse texture wrapper
        constexpr uintptr_t GAME_TEX_WRAPPER_2 = 0x982CB8;  // Pointer to normal texture wrapper
        constexpr uintptr_t GAME_TEX_WRAPPER_3 = 0x982CC0;  // Pointer to specular texture wrapper

        // Game context for material API
        constexpr uintptr_t GAME_CONTEXT_PTR = 0x982C80;  // Pointer to game context/state object
    }
}
