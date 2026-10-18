#include "props.h"

#include <cstdint>

#include "game.h"

namespace ssr::props {
// Verified in the 1.09 disassembly: the property map lives at *0x1416BDA28; 0x140AB7930(map, name) returns the entry
// for `name`, or a shared empty entry. An entry's +0x08 is the value object (null in the empty entry) and the value's
// +0x10 points at the storage owned by the registering scene.
void* storage(const char* name) {
  void* map = *game::at<void**>(0x1416BDA28);
  if (!map || !name) return nullptr;
  auto* entry = game::at<uint8_t*(__fastcall*)(void*, const char*)>(0x140AB7930)(map, name);
  auto* value = entry ? *reinterpret_cast<uint8_t**>(entry + 0x08) : nullptr;
  return value ? *reinterpret_cast<void**>(value + 0x10) : nullptr;
}
}  // namespace ssr::props
