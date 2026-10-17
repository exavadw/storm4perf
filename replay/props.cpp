#include "props.h"

#include "game.h"

namespace ssr::props {
namespace {
constexpr uintptr_t kPropManager = 0x1416BDA28;  // ccGameProperty* global
constexpr uintptr_t kFind = 0x140AB7930;         // holder* Find(mgr, const char*)
using FindFn = uint8_t*(__fastcall*)(void* mgr, const char* name);
}  // namespace

void* storage(const char* name) {
  void* mgr = *game::at<void**>(kPropManager);
  if (!mgr) return nullptr;
  uint8_t* holder = game::at<FindFn>(kFind)(mgr, name);
  if (!holder) return nullptr;
  auto* value = *reinterpret_cast<uint8_t**>(holder + 8);
  if (!value) return nullptr;
  return *reinterpret_cast<void**>(value + 0x10);
}
}  // namespace ssr::props
