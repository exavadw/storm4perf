#pragma once
#include <cstdint>

namespace ssr::hooks {
bool init();
// Creates and enables a MinHook detour at `target`; stores the trampoline in *original. Logs failures.
bool install(const char* name, void* target, void* detour, void** original);
void shutdown();
}  // namespace ssr::hooks
