#pragma once
#include <cstdint>

// ccGameProperty access. The global manager (VA 0x1416BDA28) maps a name to a holder; holder+8 is a typed value
// object whose +0x10 points at the owning scene's storage. Lookup = FUN_140AB7930(mgr, name). Values exist only
// while the scene that registered them is alive, so callers must re-query each time and handle nullptr.
namespace ssr::props {
void* storage(const char* name);  // pointer to the property's storage, or nullptr if not registered right now

template <class T> T* get(const char* name) { return static_cast<T*>(storage(name)); }
}  // namespace ssr::props
