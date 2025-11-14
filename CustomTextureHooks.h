#pragma once

namespace ngg {
namespace mw {

// Hook installation API
bool InstallTextureHooks(void* pHookLoad, void* pHookSwap);
void UninstallTextureHooks();

} // namespace mw
} // namespace ngg
