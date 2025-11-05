#pragma once

#include "features.h"
#include <d3d9.h>

struct IDirect3DDevice9; // Forward declaration
struct IDirect3DBaseTexture9; // Forward declaration

// Function pointer type for SetTexture
typedef HRESULT(STDMETHODCALLTYPE* SetTextureFn)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);

class CustomTextureLoader : public ngg::common::Feature
{
public:
    const char* name() const override { return "CustomTextureLoader"; }

    void enable() override;
    void disable() override;

    // Called from HookedPresent to provide the D3D device
    static void SetD3DDevice(IDirect3DDevice9* device);
};

