#pragma once

#include "Log.h"
// REMOVED: This macro was breaking OutputDebugStringA calls during early initialization
// #define OutputDebugStringA(...) asi_log::Log(__VA_ARGS__)

namespace ngg
{
    namespace common
    {
        class Feature
        {
        public:
            virtual ~Feature() = default;
            virtual const char* name() const = 0;

            virtual void enable()
            {
            }

            virtual void disable()
            {
            }
        };
    }
}

namespace ngg
{
    namespace mw
    {
        namespace features
        {
        }
    }
}
