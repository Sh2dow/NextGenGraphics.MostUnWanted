#pragma once

#include <windows.h>
#include <d3d9.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <functional>

namespace ngg {
namespace mw {

// TPF (TexMod Package File) Loader
// TPF files are encrypted ZIP archives containing DDS textures
//
// DESIGN: Fast extraction on render thread, parallel texture creation on IOCP workers
// 1. Render thread: XOR decrypt + parse ZIP (fast)
// 2. Render thread: Post DDS entries to IOCP queue
// 3. Worker threads: ZipCrypto decrypt + DEFLATE decompress + create D3D9 texture (parallel)
class TPFLoader
{
public:
    // Callback function type for processing each extracted DDS entry
    // Parameters: hash, filename, dds_data, dds_size
    using DDSEntryCallback = std::function<void(uint32_t, const std::string&, const uint8_t*, size_t)>;

    TPFLoader();
    ~TPFLoader();

    // Load a TPF file and call callback for each DDS entry
    // This is FAST - only does XOR decryption and ZIP parsing on render thread
    // The callback should post entries to IOCP queue for parallel processing
    // Returns number of DDS entries found (not including texmod.def)
    int LoadTPFAndPostToIOCP(const std::wstring& tpfPath, DDSEntryCallback callback);

    // Get texmod.def content (if present in TPF)
    const std::string& GetTexmodDef() const { return m_texmodDef; }

    // Get game hash → CRC32 hash mapping from texmod.def
    const std::unordered_map<uint32_t, uint32_t>& GetGameHashToCRC32Map() const { return m_gameHashToCRC32; }

private:
    // Parse CRC32 from filename (e.g., "0x12345678.dds" -> 0x12345678)
    // For non-hex filenames (e.g., "specroad.dds"), computes DJB hash
    uint32_t ParseCRC32FromFilename(const std::string& filename);

    std::string m_texmodDef;
    std::unordered_map<uint32_t, uint32_t> m_gameHashToCRC32; // game hash → CRC32 hash
};

} // namespace mw
} // namespace ngg

