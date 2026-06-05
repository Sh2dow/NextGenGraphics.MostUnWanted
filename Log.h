#pragma once
#include <windows.h>
#include <cstdio>
#include <cstdarg>

namespace asi_log
{
    inline void Log(const char* fmt, ...)
    {
        char buffer[2048];

        va_list args;
        va_start(args, fmt);
        _vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, fmt, args);
        va_end(args);

        char final[2200];
        _snprintf_s(final, sizeof(final), _TRUNCATE,
            "[NextGenGraphics.TextureLoader] %s\n", buffer);

        // Console (if attached)
        printf("%s", final);
        fflush(stdout);

        // Debug output (safe in injected DLLs)
        OutputDebugStringA(final);
    }
}
