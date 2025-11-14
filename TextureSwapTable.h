#pragma once

// Extracted swap table builder for CustomTextureLoader
// Header-only, included inside CustomTextureLoader.cpp anonymous namespace
// Relies on globals defined in that file (g_hashTable, g_swapTable, g_swapTableBuilt, g_gameHashToCRC32Map, g_crc32MapLock)
// and GameTextureHashes.h (ngg::mw::g_validGameTextureHashes)

// NOTE: This function is intended to be included within the same TU as the globals.
// Keep it inline to avoid ODR issues if included elsewhere by mistake.
inline void BuildSwapTableEx(bool rebuild = false, CRITICAL_SECTION* swapLock = nullptr)
{
    // If already built and not forcing rebuild, skip
    if (g_swapTableBuilt.load() && !rebuild)
        return;

    // CRITICAL: Build a NEW swap table to avoid race conditions!
    // The render thread is constantly calling SwapTextures() which reads g_swapTable.
    // If we clear g_swapTable and then rebuild it, there's a window where SwapTextures
    // will fail to find any textures (causing the "was working but now fails" errors).
    // Solution: Build a new table, then atomically swap the pointer.
    std::unordered_map<uint32_t, IDirect3DTexture9*>* newSwapTable =
        new std::unordered_map<uint32_t, IDirect3DTexture9*>();

    if (!g_swapTable)
    {
        asi_log::Log("CustomTextureLoader: BuildSwapTable - Creating new swap table (rebuild=%d)", rebuild);
    }
    else if (rebuild)
    {
        asi_log::Log("CustomTextureLoader: BuildSwapTable - Rebuilding swap table (%d old entries)",
                     g_swapTable->size());
    }
    else
    {
        asi_log::Log("CustomTextureLoader: BuildSwapTable - Skipping (already built, rebuild=%d)", rebuild);
    }

    int totalCustomTextures = 0;
    int validatedTextures = 0;
    int invalidTextures = 0;

    // Save pointer to old swap table BEFORE we start building the new one
    std::unordered_map<uint32_t, IDirect3DTexture9*>* oldSwapTable = g_swapTable;

    // First pass: iterate all known valid game hashes (from STREAML2RA)
    asi_log::Log("CustomTextureLoader: Building swap table from hash table (checking %zu valid game hashes)",
                 ngg::mw::g_validGameTextureHashes.size());

    int foundInHashTable = 0;
    int notFoundInHashTable = 0;

    for (uint32_t gameHash : ngg::mw::g_validGameTextureHashes)
    {
        IDirect3DTexture9* texture = g_hashTable->GetTexture(gameHash);
        if (texture && texture != reinterpret_cast<IDirect3DTexture9*>(-1))
        {
            ULONG refCount = texture->AddRef();
            if (refCount > 0)
            {
                texture->Release();
                texture->AddRef();
                (*newSwapTable)[gameHash] = texture;
                validatedTextures++;
                totalCustomTextures++;
                foundInHashTable++;
            }
            else
            {
                invalidTextures++;
            }
        }
        else
        {
            notFoundInHashTable++;
        }
    }

    asi_log::Log("CustomTextureLoader: Hash table scan complete - %d found, %d not found (expected)",
                 foundInHashTable, notFoundInHashTable);

    // Second pass: include TPF game hashes that may not be present in g_validGameTextureHashes
    int addedFromTPFMap = 0;
    int addedFromTPFViaCRC = 0;
    if (g_gameHashToCRC32Map)
    {
        EnterCriticalSection(&g_crc32MapLock);
        for (const auto& kv : *g_gameHashToCRC32Map)
        {
            uint32_t gameHash = kv.first;
            uint32_t crc32 = kv.second;
            if (newSwapTable->find(gameHash) != newSwapTable->end())
                continue; // already added via first pass

            // Try by game hash first
            IDirect3DTexture9* texture = g_hashTable->GetTexture(gameHash);
            bool usedCRC = false;

            // If not present under game hash, try CRC32 (common when IOCP posted before map was stored)
            if (!texture && crc32 != 0)
            {
                IDirect3DTexture9* texByCRC = g_hashTable->GetTexture(crc32);
                if (texByCRC)
                {
                    texture = texByCRC;
                    usedCRC = true;
                }
            }

            if (texture && texture != reinterpret_cast<IDirect3DTexture9*>(-1))
            {
                ULONG refCount = texture->AddRef();
                if (refCount > 0)
                {
                    texture->Release();
                    texture->AddRef();
                    (*newSwapTable)[gameHash] = texture;
                    validatedTextures++;
                    totalCustomTextures++;
                    if (usedCRC) addedFromTPFViaCRC++; else addedFromTPFMap++;
                }
                else
                {
                    invalidTextures++;
                }
            }
        }
        LeaveCriticalSection(&g_crc32MapLock);
    }

    if (addedFromTPFMap > 0 || addedFromTPFViaCRC > 0)
    {
        asi_log::Log("CustomTextureLoader: Added %d entries from TPF game-hash map (+%d via CRC)", addedFromTPFMap, addedFromTPFViaCRC);
    }

    // Atomically replace the old swap table with the new one
    asi_log::Log("CustomTextureLoader: Swap table %s - %d total, %d validated, %d invalid textures",
                 rebuild ? "rebuilt" : "built", totalCustomTextures, validatedTextures, invalidTextures);
    asi_log::Log("CustomTextureLoader: Old swap table: %s, size: %d",
                 oldSwapTable ? "exists" : "null", oldSwapTable ? (int)oldSwapTable->size() : 0);
    asi_log::Log("CustomTextureLoader: New swap table: size: %d", (int)newSwapTable->size());

    // Synchronize with render thread: swap pointer and (maybe) delete old under lock
    if (swapLock) EnterCriticalSection(swapLock);

    // Safety: if new table is empty while old existed, keep old to avoid regressions
    if (oldSwapTable && newSwapTable->empty())
    {
        asi_log::Log("CustomTextureLoader: New swap table is empty; keeping old table to avoid fallback-only regressions");
        delete newSwapTable; // discard empty table
        if (swapLock) LeaveCriticalSection(swapLock);
        return;
    }

    // Swap in the new table
    g_swapTable = newSwapTable;
    g_swapTableBuilt.store(true);

    // Release old swap table textures and delete old table
    if (oldSwapTable)
    {
        asi_log::Log("CustomTextureLoader: Releasing %d textures from old swap table", (int)oldSwapTable->size());
        for (auto& pair : *oldSwapTable)
        {
            if (pair.second)
                pair.second->Release();
        }
        delete oldSwapTable;
    }

    if (swapLock) LeaveCriticalSection(swapLock);
}

