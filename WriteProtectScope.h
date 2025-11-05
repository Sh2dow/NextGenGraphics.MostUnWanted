#include <d3d9.h>

// RAII memory protection wrapper
class WriteProtectScope
{
public:
    WriteProtectScope(void* target, size_t size)
        : m_target(target), m_size(size), m_ok(false)
    {
        m_ok = VirtualProtect(target, size, PAGE_EXECUTE_READWRITE, &m_oldProtect);
    }

    bool ok() const { return m_ok; }

    ~WriteProtectScope()
    {
        if (m_ok)
        {
            DWORD tmp;
            VirtualProtect(m_target, m_size, m_oldProtect, &tmp);
            FlushInstructionCache(GetCurrentProcess(), m_target, m_size);
        }
    }

private:
    void*  m_target;
    size_t m_size;
    DWORD  m_oldProtect{};
    bool   m_ok;
};

// Patch helper
static bool WriteProtectPatch(void* target, const void* data, size_t size)
{
    WriteProtectScope protect(target, size);
    if (!protect.ok())
        return false;

    memcpy(target, data, size);
    return true;
}

// vtable[x] → hook
static bool MakeVTableHook(void** vtable, int index, void* hook, void** outOriginal = nullptr)
{
    if (!vtable || index < 0)
        return false;

    void** slot = &vtable[index];
    void* original = *slot;

    if (outOriginal)
        *outOriginal = original;

    if (original == hook)
        return true;

    return WriteProtectPatch(slot, &hook, sizeof(void*));
}

// OVERLOAD: device → auto vtable detection
static bool MakeVTableHook(IDirect3DDevice9* device, int index, void* hook, void** outOriginal = nullptr)
{
    if (!device)
        return false;

    void** vtable = *reinterpret_cast<void***>(device);
    if (!vtable)
        return false;

    return MakeVTableHook(vtable, index, hook, outOriginal);
}

// Matching unhook
static bool UnmakeVTableHook(void** vtable, int index, void* original)
{
    if (!vtable || index < 0)
        return false;

    void** slot = &vtable[index];

    return WriteProtectPatch(slot, &original, sizeof(void*));
}

static bool UnmakeVTableHook(IDirect3DDevice9* device, int index, void* original)
{
    if (!device)
        return false;

    void** vtable = *reinterpret_cast<void***>(device);
    if (!vtable)
        return false;

    return UnmakeVTableHook(vtable, index, original);
}
