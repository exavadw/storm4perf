#pragma once
// Function detours (MinHook). storm4perf's guardfix uses the same MinHook instance, so initialising twice is fine.
namespace ssr::hooks {
bool init();
// Detour `target` to `detour`; *original receives the trampoline that calls the real function. Logs the outcome.
bool install(const char* name, void* target, void* detour, void** original);
}  // namespace ssr::hooks
