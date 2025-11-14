// Example: How to integrate TPFLoader into CustomTextureLoader
// This shows how to load textures from TPF files directly

#include "TPFLoader.h"
#include "CustomTextureLoader.h"
#include <d3d9.h>

namespace ngg {
namespace mw {

// Add this to CustomTextureLoader class:
bool CustomTextureLoader::LoadTexturesFromTPF(const std::wstring& tpfPath, IDirect3DDevice9* device)
{
    asi_log::Log("CustomTextureLoader: Loading textures from TPF: %S", tpfPath.c_str());

    // Create TPF loader
    TPFLoader tpfLoader;
    if (!tpfLoader.LoadTPF(tpfPath))
    {
        asi_log::Log("CustomTextureLoader: Failed to load TPF file");
        return false;
    }

    // Get all textures from TPF
    const auto& textures = tpfLoader.GetTextures();
    asi_log::Log("CustomTextureLoader: Found %zu textures in TPF", textures.size());

    int successCount = 0;
    int failCount = 0;

    // Load each texture into D3D9
    for (const auto& entry : textures)
    {
        // Check if data is valid DDS
        if (entry.data.size() < 4 || memcmp(entry.data.data(), "DDS ", 4) != 0)
        {
            asi_log::Log("CustomTextureLoader: WARNING - %s is not a valid DDS file", entry.filename.c_str());
            failCount++;
            continue;
        }

        // Create D3D9 texture from DDS data
        IDirect3DTexture9* texture = nullptr;
        HRESULT hr = D3DXCreateTextureFromFileInMemory(
            device,
            entry.data.data(),
            static_cast<UINT>(entry.data.size()),
            &texture
        );

        if (FAILED(hr))
        {
            asi_log::Log("CustomTextureLoader: Failed to create texture from %s (HRESULT: 0x%08X)", 
                         entry.filename.c_str(), hr);
            failCount++;
            continue;
        }

        // Store texture by CRC32 hash
        {
            std::lock_guard<std::mutex> lock(g_textureMapMutex);
            
            // Check if texture already exists
            auto it = g_textureMap.find(entry.crc32Hash);
            if (it != g_textureMap.end())
            {
                // Release old texture
                it->second->Release();
                asi_log::Log("CustomTextureLoader: Replaced existing texture for CRC32: 0x%08X", entry.crc32Hash);
            }

            g_textureMap[entry.crc32Hash] = texture;
        }

        successCount++;
        asi_log::Log("CustomTextureLoader: Loaded %s -> CRC32: 0x%08X", 
                     entry.filename.c_str(), entry.crc32Hash);
    }

    asi_log::Log("CustomTextureLoader: TPF loading complete - %d succeeded, %d failed", 
                 successCount, failCount);

    return successCount > 0;
}

// Example usage in Initialize():
void CustomTextureLoader::Initialize(IDirect3DDevice9* device)
{
    // ... existing initialization code ...

    // Option 1: Load from individual DDS files (current approach)
    LoadTexturesFromDirectory(L"d:\\Games\\NFS MW - Vanilla\\CUSTOM_TEXTURES", device);

    // Option 2: Load from TPF file (new approach)
    LoadTexturesFromTPF(L"d:\\Games\\NFS MW - Vanilla\\TexMod\\Aksine's Texmod V0.9 Retexture.tpf", device);

    // Option 3: Load from multiple TPF files
    std::vector<std::wstring> tpfFiles = {
        L"d:\\Games\\NFS MW - Vanilla\\TexMod\\pack1.tpf",
        L"d:\\Games\\NFS MW - Vanilla\\TexMod\\pack2.tpf",
        L"d:\\Games\\NFS MW - Vanilla\\TexMod\\pack3.tpf"
    };

    for (const auto& tpfPath : tpfFiles)
    {
        LoadTexturesFromTPF(tpfPath, device);
    }
}

// Example: Auto-detect and load all TPF files from a directory
void CustomTextureLoader::LoadAllTPFsFromDirectory(const std::wstring& directory, IDirect3DDevice9* device)
{
    WIN32_FIND_DATAW findData;
    std::wstring searchPath = directory + L"\\*.tpf";
    HANDLE hFind = FindFirstFileW(searchPath.c_str(), &findData);

    if (hFind == INVALID_HANDLE_VALUE)
    {
        asi_log::Log("CustomTextureLoader: No TPF files found in: %S", directory.c_str());
        return;
    }

    int tpfCount = 0;
    do
    {
        if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        {
            std::wstring tpfPath = directory + L"\\" + findData.cFileName;
            asi_log::Log("CustomTextureLoader: Loading TPF: %S", findData.cFileName);
            
            if (LoadTexturesFromTPF(tpfPath, device))
            {
                tpfCount++;
            }
        }
    } while (FindNextFileW(hFind, &findData));

    FindClose(hFind);

    asi_log::Log("CustomTextureLoader: Loaded %d TPF files from directory", tpfCount);
}

} // namespace mw
} // namespace ngg

/*
 * INTEGRATION STEPS:
 * 
 * 1. Add TPFLoader.h and TPFLoader.cpp to your project
 * 
 * 2. Add zlib library to your project:
 *    - Download zlib from https://www.zlib.net/
 *    - Add zlib.lib to linker dependencies
 *    - Add zlib include path to project
 * 
 * 3. Add these methods to CustomTextureLoader class in CustomTextureLoader.h:
 *    bool LoadTexturesFromTPF(const std::wstring& tpfPath, IDirect3DDevice9* device);
 *    void LoadAllTPFsFromDirectory(const std::wstring& directory, IDirect3DDevice9* device);
 * 
 * 4. Call LoadTexturesFromTPF() or LoadAllTPFsFromDirectory() in your initialization code
 * 
 * 5. Build and test!
 * 
 * NOTES:
 * - TPF files are loaded AFTER the CRC32 cache is built
 * - Textures from TPF override any existing textures with the same CRC32
 * - You can mix TPF files and individual DDS files
 * - The texmod.def file is parsed but not currently used (can be extended later)
 */

