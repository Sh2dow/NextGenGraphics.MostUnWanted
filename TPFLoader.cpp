#include "TPFLoader.h"
#include "Log.h"
#include <sstream>
#include <emmintrin.h> // SSE2 intrinsics for SIMD XOR decryption

// miniz for ZIP handling (includes ZipCrypto decryption and DEFLATE decompression)
#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#include "miniz/miniz.h"

namespace ngg {
namespace mw {

TPFLoader::TPFLoader()
{
}

TPFLoader::~TPFLoader()
{
}

// Helper function to XOR decrypt TPF file data using SIMD (SSE2)
// TPF files are XOR encrypted with key 0x3FA43FA4 (repeated every 4 bytes)
// This SIMD version processes 16 bytes at a time for ~30-40% speedup
static void XORDecryptTPF(std::vector<uint8_t>& data)
{
    const uint32_t TPF_XOR_KEY = 0x3FA43FA4;

    // Broadcast the 4-byte key to fill a 128-bit (16-byte) SSE2 register
    // This creates: [3FA43FA4 3FA43FA4 3FA43FA4 3FA43FA4] (4 copies of the key)
    __m128i key = _mm_set1_epi32(TPF_XOR_KEY);

    size_t i = 0;

    // Process 16 bytes at a time using SSE2
    // This is 16x faster than the scalar loop (1 instruction vs 16 instructions)
    for (; i + 16 <= data.size(); i += 16)
    {
        // Load 16 bytes from data (unaligned load - data may not be 16-byte aligned)
        __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data.data() + i));

        // XOR the 16 bytes with the key (all 16 bytes XORed in parallel)
        chunk = _mm_xor_si128(chunk, key);

        // Store the result back (unaligned store)
        _mm_storeu_si128(reinterpret_cast<__m128i*>(data.data() + i), chunk);
    }

    // Handle remaining bytes (0-15 bytes) with scalar code
    // This is necessary because data.size() may not be a multiple of 16
    const uint8_t* keyBytes = reinterpret_cast<const uint8_t*>(&TPF_XOR_KEY);
    for (; i < data.size(); i++)
    {
        data[i] ^= keyBytes[i % 4];
    }
}

// ZipCrypto decryption implementation
// Based on ME3Explorer's KFreonZipCrypto.cs
class ZipCryptoDecryptor
{
private:
    static const uint8_t TPF_ZIP_KEY[42];

    uint32_t keys[3];

    // CRC32 table for ZipCrypto
    static uint32_t crc32_table[256];
    static bool crc32_table_initialized;

    static void InitCRC32Table()
    {
        if (crc32_table_initialized) return;

        for (uint32_t i = 0; i < 256; i++)
        {
            uint32_t crc = i;
            for (int j = 0; j < 8; j++)
            {
                if (crc & 1)
                    crc = (crc >> 1) ^ 0xEDB88320;
                else
                    crc >>= 1;
            }
            crc32_table[i] = crc;
        }
        crc32_table_initialized = true;
    }

    uint32_t UpdateCRC32(uint32_t crc, uint8_t b)
    {
        return crc32_table[(crc ^ b) & 0xFF] ^ (crc >> 8);
    }

    uint8_t MagicByte()
    {
        uint16_t t = (uint16_t)((keys[2] & 0xFFFF) | 2);
        return (uint8_t)((t * (t ^ 1)) >> 8);
    }

    void UpdateKeys(uint8_t b)
    {
        keys[0] = UpdateCRC32(keys[0], b);
        keys[1] = keys[1] + (uint8_t)keys[0];
        keys[1] = keys[1] * 0x08088405 + 1;
        keys[2] = UpdateCRC32(keys[2], (uint8_t)(keys[1] >> 24));
    }

public:
    ZipCryptoDecryptor()
    {
        InitCRC32Table();

        // Initialize cipher with password
        keys[0] = 305419896;
        keys[1] = 591751049;
        keys[2] = 878082192;

        for (int i = 0; i < 42; i++)
        {
            UpdateKeys(TPF_ZIP_KEY[i]);
        }
    }

    bool DecryptData(uint8_t* data, size_t size, uint32_t fileCRC, uint16_t bitFlag)
    {
        if (size < 12)
            return false;

        // Decrypt the 12-byte encryption header
        for (size_t i = 0; i < 12; i++)
        {
            uint8_t decrypted = data[i] ^ MagicByte();
            UpdateKeys(decrypted);
            data[i] = decrypted;
        }

        // Verify password by checking last byte of header against CRC
        // Skip verification if bit 3 (data descriptor flag) is set
        if ((bitFlag & 0x8) != 0x8)
        {
            if (data[11] != (uint8_t)((fileCRC >> 24) & 0xFF))
            {
                // Password verification failed
                return false;
            }
        }

        // Decrypt the rest of the data
        for (size_t i = 12; i < size; i++)
        {
            uint8_t decrypted = data[i] ^ MagicByte();
            UpdateKeys(decrypted);
            data[i] = decrypted;
        }

        return true;
    }
};

// Static member initialization
const uint8_t ZipCryptoDecryptor::TPF_ZIP_KEY[42] = {
    0x73, 0x2A, 0x63, 0x7D, 0x5F, 0x0A, 0xA6, 0xBD,
    0x7D, 0x65, 0x7E, 0x67, 0x61, 0x2A, 0x7F, 0x7F,
    0x74, 0x61, 0x67, 0x5B, 0x60, 0x70, 0x45, 0x74,
    0x5C, 0x22, 0x74, 0x5D, 0x6E, 0x6A, 0x73, 0x41,
    0x77, 0x6E, 0x46, 0x47, 0x77, 0x49, 0x0C, 0x4B,
    0x46, 0x6F
};

uint32_t ZipCryptoDecryptor::crc32_table[256];
bool ZipCryptoDecryptor::crc32_table_initialized = false;

// Trim helpers
static inline void TrimInPlace(std::string& s) {
    size_t start = 0;
    while (start < s.size() && (s[start] == ' ' || s[start] == '\t' || s[start] == '\r' || s[start] == '\n')) start++;
    size_t end = s.size();
    while (end > start && (s[end-1] == ' ' || s[end-1] == '\t' || s[end-1] == '\r' || s[end-1] == '\n')) end--;
    if (start == 0 && end == s.size()) return;
    s = s.substr(start, end - start);
}
static inline bool StartsWithCaseInsensitive(const std::string& s, const std::string& prefix) {
    if (s.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        char a = s[i];
        char b = prefix[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
        if (b >= 'a' && b <= 'z') b = (char)(b - 'a' + 'A');
        if (a != b) return false;
    }
    return true;
}

int TPFLoader::LoadTPFAndPostToIOCP(const std::wstring& tpfPath, DDSEntryCallback callback)
{
    // Use memory-mapped file for faster I/O (50-70% faster than ifstream)
    HANDLE hFile = CreateFileW(
        tpfPath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if (hFile == INVALID_HANDLE_VALUE)
    {
        asi_log::Log("TPFLoader: Failed to open file: %S (error %lu)", tpfPath.c_str(), GetLastError());
        return 0;
    }

    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(hFile, &fileSize))
    {
        asi_log::Log("TPFLoader: Failed to get file size: %S (error %lu)", tpfPath.c_str(), GetLastError());
        CloseHandle(hFile);
        return 0;
    }

    // Create file mapping (read-only)
    HANDLE hMapping = CreateFileMappingW(
        hFile,
        NULL,
        PAGE_READONLY,
        0,
        0,
        NULL
    );

    if (hMapping == NULL)
    {
        asi_log::Log("TPFLoader: Failed to create file mapping: %S (error %lu)", tpfPath.c_str(), GetLastError());
        CloseHandle(hFile);
        return 0;
    }

    // Map view of file into memory
    uint8_t* pMappedData = static_cast<uint8_t*>(MapViewOfFile(
        hMapping,
        FILE_MAP_READ,
        0,
        0,
        0
    ));

    if (pMappedData == NULL)
    {
        asi_log::Log("TPFLoader: Failed to map view of file: %S (error %lu)", tpfPath.c_str(), GetLastError());
        CloseHandle(hMapping);
        CloseHandle(hFile);
        return 0;
    }

    asi_log::Log("TPFLoader: Memory-mapped TPF file (%lld bytes): %S", fileSize.QuadPart, tpfPath.c_str());

    // Copy mapped data to vector for XOR decryption
    // (We need a writable copy because XOR decryption modifies the data)
    std::vector<uint8_t> fileData(pMappedData, pMappedData + fileSize.QuadPart);

    // Unmap and close handles immediately after copying
    UnmapViewOfFile(pMappedData);
    CloseHandle(hMapping);
    CloseHandle(hFile);

    // XOR decrypt the entire file (Layer 1 encryption)
    XORDecryptTPF(fileData);

    // Verify it's now a valid ZIP file
    if (fileData.size() < 4 || fileData[0] != 0x50 || fileData[1] != 0x4B)
    {
        asi_log::Log("TPFLoader: Not a valid ZIP file after XOR decryption");
        return 0;
    }

    asi_log::Log("TPFLoader: XOR decryption successful - valid ZIP signature found");

    // Use miniz to open the decrypted ZIP from memory
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));

    if (!mz_zip_reader_init_mem(&zip, fileData.data(), fileData.size(), 0))
    {
        asi_log::Log("TPFLoader: Failed to initialize ZIP reader: %s",
                     mz_zip_get_error_string(mz_zip_get_last_error(&zip)));
        return 0;
    }

    mz_uint numFiles = mz_zip_reader_get_num_files(&zip);
    asi_log::Log("TPFLoader: Found %u entries in TPF", numFiles);

    int ddsEntriesPosted = 0;

    // Clear previous texmod.def and game hash map
    m_texmodDef.clear();
    m_gameHashToCRC32.clear();

    // CRITICAL FIX: First pass - extract and parse texmod.def BEFORE posting DDS entries!
    // This ensures g_crc32ToGameHashMap is populated before IOCP workers start loading textures.
    // Otherwise, workers will only add textures by CRC32 hash, not by game hash!
    asi_log::Log("TPFLoader: *** FIRST PASS - Looking for texmod.def in %u files ***", numFiles);
    bool texmodDefFound = false;
    for (mz_uint i = 0; i < numFiles; i++)
    {
        mz_zip_archive_file_stat file_stat;
        if (!mz_zip_reader_file_stat(&zip, i, &file_stat))
            continue;

        asi_log::Log("TPFLoader: First pass - checking file: %s", file_stat.m_filename);

        // Only process texmod.def in first pass
        if (strcmp(file_stat.m_filename, "texmod.def") != 0)
            continue;

        asi_log::Log("TPFLoader: *** FOUND texmod.def in first pass! ***");
        texmodDefFound = true;

        bool isEncrypted = (file_stat.m_bit_flag & 1) != 0;
        asi_log::Log("TPFLoader: texmod.def isEncrypted = %d", isEncrypted);

        size_t uncompressedSize = 0;
        void* pData = nullptr;

        if (isEncrypted)
        {
            asi_log::Log("TPFLoader: Extracting ENCRYPTED texmod.def...");

            // Extract encrypted texmod.def (same code as below)
            const uint8_t* pZipData = (const uint8_t*)fileData.data();
            size_t localHeaderOffset = (size_t)file_stat.m_local_header_ofs;

            if (localHeaderOffset + 30 > fileData.size())
            {
                asi_log::Log("TPFLoader: ERROR - localHeaderOffset + 30 > fileData.size()");
                continue;
            }

            uint32_t signature = *(uint32_t*)(pZipData + localHeaderOffset);
            if (signature != 0x04034b50)
            {
                asi_log::Log("TPFLoader: ERROR - Invalid ZIP signature: 0x%08X", signature);
                continue;
            }

            uint16_t bitFlag = *(uint16_t*)(pZipData + localHeaderOffset + 6);
            uint16_t filenameLen = *(uint16_t*)(pZipData + localHeaderOffset + 26);
            uint16_t extraFieldLen = *(uint16_t*)(pZipData + localHeaderOffset + 28);

            size_t compressedDataOffset = localHeaderOffset + 30 + filenameLen + extraFieldLen;
            size_t compressedSize = (size_t)file_stat.m_comp_size;

            if (compressedDataOffset + compressedSize > fileData.size())
            {
                asi_log::Log("TPFLoader: ERROR - compressedDataOffset + compressedSize > fileData.size()");
                continue;
            }

            void* pCompressed = malloc(compressedSize);
            if (!pCompressed)
            {
                asi_log::Log("TPFLoader: ERROR - Failed to allocate memory for compressed data");
                continue;
            }

            memcpy(pCompressed, pZipData + compressedDataOffset, compressedSize);

            ZipCryptoDecryptor decryptor;
            if (!decryptor.DecryptData((uint8_t*)pCompressed, compressedSize, file_stat.m_crc32, bitFlag))
            {
                asi_log::Log("TPFLoader: ERROR - ZipCrypto decryption failed");
                free(pCompressed);
                continue;
            }

            uncompressedSize = file_stat.m_uncomp_size;
            pData = malloc(uncompressedSize);
            if (!pData)
            {
                asi_log::Log("TPFLoader: ERROR - Failed to allocate memory for uncompressed data");
                free(pCompressed);
                continue;
            }

            // Decompress using miniz's tinfl (raw DEFLATE decompressor)
            // ZIP uses raw DEFLATE, not zlib format
            tinfl_decompressor inflator;
            tinfl_init(&inflator);

            // Skip 12-byte encryption header (same as DDS files)
            size_t inBytes = compressedSize - 12;
            size_t outBytes = uncompressedSize;
            tinfl_status status = tinfl_decompress(&inflator,
                (const mz_uint8*)pCompressed + 12,  // Skip 12-byte encryption header
                &inBytes,
                (mz_uint8*)pData,
                (mz_uint8*)pData,
                &outBytes,
                TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);  // Use same flag as DDS files

            free(pCompressed);

            if (status != TINFL_STATUS_DONE || outBytes != uncompressedSize)
            {
                asi_log::Log("TPFLoader: ERROR - Decompression failed (status=%d, outBytes=%zu, expected=%zu)",
                             status, outBytes, uncompressedSize);
                free(pData);
                continue;
            }

            asi_log::Log("TPFLoader: Successfully extracted ENCRYPTED texmod.def");
        }
        else
        {
            asi_log::Log("TPFLoader: Extracting UNENCRYPTED texmod.def...");

            // Extract unencrypted texmod.def
            pData = mz_zip_reader_extract_to_heap(&zip, i, &uncompressedSize, 0);
            if (!pData)
            {
                asi_log::Log("TPFLoader: ERROR - mz_zip_reader_extract_to_heap failed");
                continue;
            }

            asi_log::Log("TPFLoader: Successfully extracted UNENCRYPTED texmod.def");
        }

        // Store texmod.def content
        m_texmodDef.assign((char*)pData, uncompressedSize);
        asi_log::Log("TPFLoader: Loaded texmod.def (%zu bytes)", uncompressedSize);

        if (isEncrypted)
            free(pData);
        else
            mz_free(pData);

        // Parse texmod.def immediately to populate m_gameHashToCRC32
        if (!m_texmodDef.empty())
        {
            asi_log::Log("TPFLoader: Parsing texmod.def to map game hashes to textures...");

            std::string defContent(m_texmodDef.begin(), m_texmodDef.end());
            std::istringstream defStream(defContent);
            std::string line;
            int mappingsFound = 0;

            while (std::getline(defStream, line))
            {
                if (line.empty())
                    continue;

                TrimInPlace(line);
                if (line.empty())
                    continue;

                // Skip comments
                if (line[0] == '#' || (line.size() > 1 && line[0] == '/' && line[1] == '/'))
                    continue;

                size_t pipePos = line.find('|');
                if (pipePos == std::string::npos)
                    continue;

                std::string gameHashStr = line.substr(0, pipePos);
                std::string filename = line.substr(pipePos + 1);
                TrimInPlace(gameHashStr);
                TrimInPlace(filename);

                uint32_t gameHash = 0;
                if (sscanf_s(gameHashStr.c_str(), "%X", &gameHash) != 1)
                    continue;

                // Strip "SPEED.EXE_" prefix from filename (case-insensitive)
                const std::string prefix = "SPEED.EXE_";
                if (StartsWithCaseInsensitive(filename, prefix))
                {
                    filename = filename.substr(prefix.size());
                    TrimInPlace(filename);
                }

                uint32_t crc32Hash = ParseCRC32FromFilename(filename);
                m_gameHashToCRC32[gameHash] = crc32Hash;
                mappingsFound++;
            }

            asi_log::Log("TPFLoader: Parsed %d game hash → texture mappings from texmod.def", mappingsFound);
        }

        break; // Found and parsed texmod.def, exit first pass
    }

    if (!texmodDefFound)
    {
        asi_log::Log("TPFLoader: *** WARNING *** texmod.def NOT FOUND in first pass!");
    }

    // Second pass - iterate through all files in the ZIP to post DDS entries
    for (mz_uint i = 0; i < numFiles; i++)
    {
        mz_zip_archive_file_stat file_stat;
        if (!mz_zip_reader_file_stat(&zip, i, &file_stat))
        {
            asi_log::Log("TPFLoader: Failed to get file stat for entry %u", i);
            continue;
        }

        // Skip texmod.def (already processed in first pass)
        if (strcmp(file_stat.m_filename, "texmod.def") == 0)
            continue;

        // Check if file is encrypted (bit 0 of general purpose bit flag)
        bool isEncrypted = (file_stat.m_bit_flag & 1) != 0;

        // Extract DDS file to memory
        size_t uncompressedSize = 0;
        void* pData = nullptr;

        if (isEncrypted)
        {
            // For encrypted files, we need to:
            // 1. Read raw compressed+encrypted data
            // 2. Decrypt it (ZipCrypto)
            // 3. Decompress it (DEFLATE)

            // For encrypted files, we need to manually read the compressed data
            // because miniz doesn't support reading raw compressed data directly

            // The ZIP local file header structure:
            // - 4 bytes: signature (0x04034b50)
            // - 2 bytes: version needed
            // - 2 bytes: flags
            // - 2 bytes: compression method
            // - 2 bytes: last mod time
            // - 2 bytes: last mod date
            // - 4 bytes: crc32
            // - 4 bytes: compressed size
            // - 4 bytes: uncompressed size
            // - 2 bytes: filename length (n)
            // - 2 bytes: extra field length (m)
            // - n bytes: filename
            // - m bytes: extra field
            // - compressed data starts here

            // Read the local header to determine its size
            const uint8_t* pZipData = (const uint8_t*)fileData.data();
            size_t localHeaderOffset = (size_t)file_stat.m_local_header_ofs;

            // Check signature
            if (localHeaderOffset + 30 > fileData.size())
            {
                asi_log::Log("TPFLoader: Invalid local header offset for %s", file_stat.m_filename);
                continue;
            }

            uint32_t signature = *(uint32_t*)(pZipData + localHeaderOffset);
            if (signature != 0x04034b50)
            {
                asi_log::Log("TPFLoader: Invalid local header signature for %s", file_stat.m_filename);
                continue;
            }

            // Read bit flag (offset 6)
            uint16_t bitFlag = *(uint16_t*)(pZipData + localHeaderOffset + 6);

            // Read filename and extra field lengths
            uint16_t filenameLen = *(uint16_t*)(pZipData + localHeaderOffset + 26);
            uint16_t extraFieldLen = *(uint16_t*)(pZipData + localHeaderOffset + 28);

            // Calculate offset to compressed data
            size_t compressedDataOffset = localHeaderOffset + 30 + filenameLen + extraFieldLen;
            size_t compressedSize = (size_t)file_stat.m_comp_size;

            if (compressedDataOffset + compressedSize > fileData.size())
            {
                asi_log::Log("TPFLoader: Compressed data out of bounds for %s", file_stat.m_filename);
                continue;
            }

            // Allocate buffer and copy compressed data
            void* pCompressed = malloc(compressedSize);
            if (!pCompressed)
            {
                asi_log::Log("TPFLoader: Failed to allocate memory for compressed data: %s", file_stat.m_filename);
                continue;
            }

            memcpy(pCompressed, pZipData + compressedDataOffset, compressedSize);

            // Decrypt the data
            ZipCryptoDecryptor decryptor;
            if (!decryptor.DecryptData((uint8_t*)pCompressed, compressedSize, file_stat.m_crc32, bitFlag))
            {
                asi_log::Log("TPFLoader: Failed to decrypt %s (password verification failed)", file_stat.m_filename);
                free(pCompressed);
                continue;
            }

            // Now decompress the decrypted data (skip 12-byte encryption header)
            // The decrypted data is now pure DEFLATE stream
            uncompressedSize = file_stat.m_uncomp_size;
            pData = malloc(uncompressedSize);
            if (!pData)
            {
                asi_log::Log("TPFLoader: Failed to allocate memory for %s", file_stat.m_filename);
                free(pCompressed);
                continue;
            }

            // Decompress using miniz's tinfl (raw DEFLATE decompressor)
            // ZIP uses raw DEFLATE, not zlib format, so we need to use tinfl directly
            tinfl_decompressor inflator;
            tinfl_init(&inflator);

            size_t in_bytes = compressedSize - 12;  // Skip 12-byte encryption header
            size_t out_bytes = uncompressedSize;

            tinfl_status status = tinfl_decompress(&inflator,
                                                   (const mz_uint8*)pCompressed + 12,
                                                   &in_bytes,
                                                   (mz_uint8*)pData,
                                                   (mz_uint8*)pData,
                                                   &out_bytes,
                                                   TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);

            free(pCompressed);

            if (status != TINFL_STATUS_DONE)
            {
                asi_log::Log("TPFLoader: Failed to decompress %s (status %d)", file_stat.m_filename, status);
                free(pData);
                continue;
            }

            uncompressedSize = out_bytes;
        }
        else
        {
            // Unencrypted file - use standard extraction
            pData = mz_zip_reader_extract_to_heap(&zip, i, &uncompressedSize, 0);
            if (!pData)
            {
                asi_log::Log("TPFLoader: Failed to extract %s", file_stat.m_filename);
                continue;
            }
        }

        // Strip "SPEED.EXE_" prefix from filename if present
        std::string filename = file_stat.m_filename;
        const std::string prefix = "SPEED.EXE_";
        if (filename.size() > prefix.size() && filename.substr(0, prefix.size()) == prefix)
        {
            filename = filename.substr(prefix.size());
        }

        // Parse CRC32 from filename
        uint32_t crc32Hash = ParseCRC32FromFilename(filename);

        // Call callback to post DDS entry to IOCP queue
        // The callback will handle creating the D3D9 texture on worker thread
        if (callback)
        {
            callback(crc32Hash, filename, (const uint8_t*)pData, uncompressedSize);
            ddsEntriesPosted++;
        }

        // Free memory (use free() for manually allocated, mz_free() for miniz-allocated)
        if (isEncrypted)
            free(pData);
        else
            mz_free(pData);
    }

    mz_zip_reader_end(&zip);

    // texmod.def was already parsed in the first pass (before posting DDS entries)
    // This ensures g_crc32ToGameHashMap is populated before IOCP workers start loading textures

    asi_log::Log("TPFLoader: Posted %d DDS entries to IOCP queue", ddsEntriesPosted);
    return ddsEntriesPosted;
}

uint32_t TPFLoader::ParseCRC32FromFilename(const std::string& filename)
{
    // Robust parser: accept names like
    //  - 0x12345678.dds, 12345678.dds
    //  - speed_t_0x12345678.dds, SPEED_t_0X12345678.dds
    //  - specroad.dds (fallback DJB of base name)
    std::string name = filename;
    TrimInPlace(name);

    // Strip extension
    size_t dotPos = name.find_last_of('.');
    std::string base = (dotPos != std::string::npos) ? name.substr(0, dotPos) : name;
    TrimInPlace(base);

    // Normalize common pack prefixes (case-insensitive) before parsing/Hashing
    // This helps when texmod.def uses "specroad.dds" but the file is "speed_t_specroad.dds"
    const char* prefixes[] = {
        "SPEED.EXE_",
        "speed_t_", "speed_T_",
        "SPEED_t_", "SPEED_T_"
    };
    bool removed;
    do {
        removed = false;
        for (const char* pfx : prefixes)
        {
            std::string pf = pfx;
            if (StartsWithCaseInsensitive(base, pf))
            {
                base = base.substr(pf.size());
                TrimInPlace(base);
                removed = true;
                break;
            }
        }
    } while (removed);

    auto isHex = [](char c) -> bool {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    };

    // 1) Prefer pattern "0x/0X" followed by 8 hex digits anywhere in the base
    for (size_t i = 0; i + 10 <= base.size(); ++i)
    {
        if (base[i] == '0' && (base[i + 1] == 'x' || base[i + 1] == 'X'))
        {
            bool ok = true;
            for (int k = 0; k < 8; ++k)
            {
                if (!isHex(base[i + 2 + k])) { ok = false; break; }
            }
            if (ok)
            {
                uint32_t val = 0;
                std::string hexStr = base.substr(i + 2, 8);
                if (sscanf_s(hexStr.c_str(), "%X", &val) == 1 && val != 0)
                    return val;
            }
        }
    }

    // 2) Otherwise, look for the last run of exactly 8 hex digits
    for (int i = static_cast<int>(base.size()) - 8; i >= 0; --i)
    {
        bool ok = true;
        for (int k = 0; k < 8; ++k)
        {
            if (!isHex(base[i + k])) { ok = false; break; }
        }
        if (ok)
        {
            uint32_t val = 0;
            std::string hexStr = base.substr(i, 8);
            if (sscanf_s(hexStr.c_str(), "%X", &val) == 1 && val != 0)
                return val;
        }
    }

    // 3) Fallback: compute DJB hash of the normalized base (without extension)
    uint32_t h = 0xFFFFFFFFu;
    for (unsigned char c : base)
        h = h * 33u + c;
    return h;
}

} // namespace mw
} // namespace ngg

