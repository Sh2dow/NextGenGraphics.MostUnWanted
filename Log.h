#pragma once
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <fstream>
#include <mutex>
#include <string>

namespace asi_log
{
    class Logger
    {
    public:
        Logger()
        {
            // log_file_.open("scripts\\shaderlog.txt", std::ios::app);
        }

        ~Logger()
        {
            if (log_file_.is_open())
                log_file_.close();
        }

        void log(const char* fmt, ...)
        {
            char buffer[2048];

            va_list args;
            va_start(args, fmt);
            vsnprintf_s(buffer, sizeof(buffer), fmt, args);
            va_end(args);

            std::string message = std::string("[NextGenGraphics.TextureLoader] ") + buffer;

            {
                std::lock_guard<std::mutex> lock(mutex_);

                // Console output
                // IMPORTANT: Use %s to prevent format string interpretation
                printf("%s\n", message.c_str());
                fflush(stdout);

                // OutputDebugString
                OutputDebugStringA((message + "\n").c_str());

                // File output
                if (log_file_.is_open())
                {
                    log_file_ << message << std::endl;
                    log_file_.flush();
                }
            }
        }

    private:
        std::mutex mutex_;
        std::ofstream log_file_;
    };

    // Global logger instance
    static Logger logger;

    // Helper macro-style function for easier usage
    inline void Log(const char* fmt, ...)
    {
        char buffer[2048];

        va_list args;
        va_start(args, fmt);
        vsnprintf_s(buffer, sizeof(buffer), fmt, args);
        va_end(args);

        logger.log("%s", buffer);
    }
}
