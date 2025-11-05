#pragma once
#include <Windows.h>
#include <vector>

class Patch
{
public:
    void* address = nullptr;
    std::vector<BYTE> originalBytes;
    std::vector<BYTE> patchedBytes;

    Patch() = default;

    Patch(void* addr, const void* data, size_t size)
        : address(addr), originalBytes(size), patchedBytes((BYTE*)data, (BYTE*)data + size)
    {
        // Backup original bytes
        std::memcpy(originalBytes.data(), addr, size);

        // Apply patch
        DWORD oldProtect;
        VirtualProtect(addr, size, PAGE_EXECUTE_READWRITE, &oldProtect);
        std::memcpy(addr, data, size);
        VirtualProtect(addr, size, oldProtect, &oldProtect);
    }

    void Restore() const
    {
        if (address && !originalBytes.empty())
        {
            DWORD oldProtect;
            VirtualProtect(address, originalBytes.size(), PAGE_EXECUTE_READWRITE, &oldProtect);
            std::memcpy(address, originalBytes.data(), originalBytes.size());
            VirtualProtect(address, originalBytes.size(), oldProtect, &oldProtect);
        }
    }
};

class CPatchMod
{
public:
    inline static Patch PatchMemory(void* address, const void* data, int size)
    {
        return Patch(address, data, size);
    }

    inline static Patch SetBytes(int address, const void* data, int size)
    {
        return Patch((void*)address, data, size);
    }

    inline static Patch Nop(int address, int size)
    {
        std::vector<BYTE> nops(size, 0x90);
        return Patch((void*)address, nops.data(), size);
    }

    inline static Patch FillWithZeroes(int address, int size)
    {
        std::vector<BYTE> zeroes(size, 0x00);
        return Patch((void*)address, zeroes.data(), size);
    }

    inline static Patch SetChar(int address, char value)
    {
        return Patch((void*)address, &value, sizeof(value));
    }

    inline static Patch SetUChar(int address, unsigned char value)
    {
        return Patch((void*)address, &value, sizeof(value));
    }

    inline static Patch SetShort(int address, short value)
    {
        return Patch((void*)address, &value, sizeof(value));
    }

    inline static Patch SetUShort(int address, unsigned short value)
    {
        return Patch((void*)address, &value, sizeof(value));
    }

    inline static Patch SetInt(int address, int value)
    {
        return Patch((void*)address, &value, sizeof(value));
    }

    inline static Patch SetUInt(int address, unsigned int value)
    {
        return Patch((void*)address, &value, sizeof(value));
    }

    inline static Patch SetFloat(int address, float value)
    {
        return Patch((void*)address, &value, sizeof(value));
    }

    inline static Patch SetDouble(int address, double value)
    {
        return Patch((void*)address, &value, sizeof(value));
    }

    inline static Patch SetPointer(int address, void* value)
    {
        return Patch((void*)address, &value, sizeof(value));
    }

    inline static Patch RedirectCall(int address, void* func)
    {
        BYTE opcode = 0xE8;
        Patch p1 = Patch((void*)address, &opcode, 1);

        int rel = (int)func - (address + 5);
        Patch p2 = Patch((void*)(address + 1), &rel, 4);

        return p1; // optionally return both if needed
    }

    inline static Patch RedirectJump(int address, void* func)
    {
        BYTE opcode = 0xE9;
        Patch p1 = Patch((void*)address, &opcode, 1);

        int rel = (int)func - (address + 5);
        Patch p2 = Patch((void*)(address + 1), &rel, 4);

        return p1; // optionally return both if needed
    }

    inline static void Unprotect(int address, int size)
    {
        DWORD protect;
        VirtualProtect((void*)address, size, PAGE_EXECUTE_READWRITE, &protect);
    }

    inline static bool FileExists(const TCHAR* fileName)
    {
        DWORD fileAttr = GetFileAttributes(fileName);
        return !(fileAttr == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND);
    }

    // Optional: AdjustPointer retained for compatibility
    inline static void AdjustPointer(int address, void* value, DWORD offset, DWORD end)
    {
        int result;
        for (int i = 0; i < 5; ++i)
        {
            DWORD* ptr = (DWORD*)(address + i);
            DWORD val = *ptr;
            if (val >= offset && val <= end)
            {
                result = (DWORD)value + val - offset;
                Patch((void*)ptr, &result, 4);
                break;
            }
        }
    }
};
