#include "text.h"

#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include "game.h"
#include "hooks.h"

// Every message lookup (C++ and the Flash menus) goes through ccUiGetMessage 0x140AB9320(id) -> UTF-8 text. One
// detour answers our own "rpl_*" ids and, while overrides are switched on, a few of the game's ids with replay wording.
namespace ssr::text {
namespace {
using GetMessageFn = const char*(__fastcall*)(const char*);
GetMessageFn real_get_message = nullptr;

struct Table {
  std::mutex mu;
  std::unordered_map<std::string, std::string> own, game_ids;
  bool game_ids_active = false;
} g_table;

const char* __fastcall get_message(const char* id) {
  if (id) {
    std::scoped_lock lk(g_table.mu);
    const bool own = std::strncmp(id, "rpl_", 4) == 0;
    if (own || g_table.game_ids_active) {
      auto& map = own ? g_table.own : g_table.game_ids;
      auto hit = map.find(id);
      if (hit != map.end()) return hit->second.c_str();  // stable: entries are never erased
    }
  }
  return real_get_message(id);
}
}  // namespace

void install() {
  hooks::install("ccUiGetMessage", game::at(0x140AB9320), reinterpret_cast<void*>(&get_message),
                 reinterpret_cast<void**>(&real_get_message));
}

void set(const char* id, std::string utf8) {
  std::scoped_lock lk(g_table.mu);
  g_table.own[id] = std::move(utf8);
}

void set_override(const char* game_id, std::string utf8) {
  std::scoped_lock lk(g_table.mu);
  g_table.game_ids[game_id] = std::move(utf8);
}

void enable_overrides(bool on) {
  std::scoped_lock lk(g_table.mu);
  g_table.game_ids_active = on;
}
}  // namespace ssr::text
