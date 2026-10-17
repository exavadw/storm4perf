#include "hooks.h"

#include <MinHook.h>

#include "log.h"

namespace ssr::hooks {

bool init() {
  const auto r = MH_Initialize();
  if (r != MH_OK && r != MH_ERROR_ALREADY_INITIALIZED) {
    log::error("MH_Initialize: {}", MH_StatusToString(r));
    return false;
  }
  return true;
}

bool install(const char* name, void* target, void* detour, void** original) {
  auto r = MH_CreateHook(target, detour, original);
  if (r == MH_OK) r = MH_EnableHook(target);
  if (r != MH_OK) {
    log::error("hook {} @{}: {}", name, target, MH_StatusToString(r));
    return false;
  }
  log::info("hooked {} @{}", name, target);
  return true;
}

void shutdown() {
  MH_DisableHook(MH_ALL_HOOKS);
  MH_Uninitialize();
}

}  // namespace ssr::hooks
