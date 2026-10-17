#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

// Game identity and the 1.09 address table. Addresses are Ghidra/IDA VAs with image base 0x140000000;
// at(va) rebases them onto the loaded module. Every entry was checked against NSUNS4.exe SHA-256 kExeSha256.
namespace ssr::game {

inline constexpr const char* kExeSha256 = "147e52a3e0d7382501a6777e158b37908e0b69d4e902079dc6e62dcae69bc257";
inline constexpr uintptr_t kImageBase = 0x140000000;

namespace va {
inline constexpr uintptr_t tolua_function = 0x1409AB5C0;           // (lua_State*, const char* name, lua_CFunction)
inline constexpr uintptr_t tolua_ccRegistScript_open = 0x14084A668;  // registers all cc* Lua bindings
inline constexpr uintptr_t ChangeBattleById = 0x1404DDC18;           // native of Lua ccChangeBattleById(const char*)
inline constexpr uintptr_t SceneManagerInstance = 0x1416BDA18;       // ccSceneManager* global
}  // namespace va

struct Info {
  uintptr_t base = 0;
  std::filesystem::path exe_dir;
  std::string sha256;
  bool supported = false;  // exe hash matches kExeSha256 → game hooks allowed
};

const Info& info();
void init();    // module base + exe dir (cheap; safe under loader lock)
void verify();  // SHA-256 of the exe → supported (call off the loader lock)

template <class T = void*> T at(uintptr_t va_addr) {
  return reinterpret_cast<T>(info().base + (va_addr - kImageBase));
}

}  // namespace ssr::game
