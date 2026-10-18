#pragma once
// Named game properties (scenes register values such as "scene_battle" while they are alive).
namespace ssr::props {
// The value's storage, or nullptr when no live scene has registered `name`. Look it up again each time you need it.
void* storage(const char* name);
}  // namespace ssr::props
