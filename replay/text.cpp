#include "text.h"

#include <atomic>
#include <cstring>
#include <map>
#include <mutex>

#include "game.h"
#include "hooks.h"

namespace ssr::text {
namespace {
constexpr uintptr_t kMsgToString = 0x140AB9320;  // const char* ccUiGetMessage(const char* id)
using MsgFn = const char*(__fastcall*)(const char*);
MsgFn o_msg = nullptr;
std::mutex g_mu;
std::map<std::string, std::string> g_text;      // our "rpl_*" ids
std::map<std::string, std::string> g_override;  // game ids, while enabled
std::atomic<bool> g_override_on{false};

const char* __fastcall hk_msg(const char* id) {
  if (id) {
    std::scoped_lock lk(g_mu);
    if (std::strncmp(id, "rpl_", 4) == 0) {
      if (auto it = g_text.find(id); it != g_text.end()) return it->second.c_str();
    } else if (g_override_on) {
      if (auto it = g_override.find(id); it != g_override.end()) return it->second.c_str();
    }
  }
  return o_msg(id);
}
}  // namespace

void install() {
  hooks::install("ccUiGetMessage", game::at(kMsgToString), reinterpret_cast<void*>(&hk_msg), reinterpret_cast<void**>(&o_msg));
}
void set(const char* id, std::string utf8) {
  std::scoped_lock lk(g_mu);
  g_text[id] = std::move(utf8);
}
void set_override(const char* game_id, std::string utf8) {
  std::scoped_lock lk(g_mu);
  g_override[game_id] = std::move(utf8);
}
void enable_overrides(bool on) { g_override_on = on; }

}  // namespace ssr::text
