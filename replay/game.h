#pragma once
// The game executable as it is loaded: exe virtual addresses (as in the 1.09 disassembly, base 0x140000000) mapped to
// live addresses, and whether this is the build the replay addresses were taken from.
#include <cstdint>
#include <string>

namespace ssr::game {
struct Info {
  bool supported = false;  // NSUNS4.exe is Steam 1.09 (SHA-256 below)
  std::string sha256;      // of the running exe, lowercase hex
};
inline constexpr const char* kSteam109 = "147e52a3e0d7382501a6777e158b37908e0b69d4e902079dc6e62dcae69bc257";

void init();    // remember the image base (any thread, early)
void verify();  // hash the exe file and fill info()
const Info& info();
uint8_t* image();

// at(0x1415F82E0) -> the live address of that exe VA, as T (a data pointer or a function pointer type).
template <class T = void*> T at(uintptr_t va) { return reinterpret_cast<T>(image() + (va - 0x140000000ull)); }
}  // namespace ssr::game
