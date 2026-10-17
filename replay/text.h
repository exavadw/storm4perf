#pragma once
#include <string>

// Message text: one hook on ccUiGetMessage 0x140AB9320 (every C++ and AS3 lookup goes through it) serves our "rpl_*"
// ids and, while enabled, replay wording for a few game ids (Retry → Restart Replay, …).
namespace ssr::text {
void install();
void set(const char* id, std::string utf8);        // "rpl_*" id
void set_override(const char* game_id, std::string utf8);
void enable_overrides(bool on);
}  // namespace ssr::text
