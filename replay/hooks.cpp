#include "hooks.h"

#include <MinHook.h>

#include "log.h"

namespace ssr::hooks {
bool init() {
  const MH_STATUS s = MH_Initialize();
  if (s == MH_OK || s == MH_ERROR_ALREADY_INITIALIZED) return true;
  log::error("MinHook init failed: {}", MH_StatusToString(s));
  return false;
}

bool install(const char* name, void* target, void* detour, void** original) {
  MH_STATUS s = MH_CreateHook(target, detour, original);
  if (s == MH_OK) s = MH_EnableHook(target);
  if (s != MH_OK) {
    log::error("hook {} at {} failed: {}", name, target, MH_StatusToString(s));
    return false;
  }
  log::info("hook {} at {}", name, target);
  return true;
}
}  // namespace ssr::hooks
