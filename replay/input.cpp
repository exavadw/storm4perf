// XInput hook: (1) lets story screens read the pad and hide it from the game, (2) lets the automation pipe press
// virtual buttons (pad 0) so tests can drive menus without focus or a physical controller.
#include <windows.h>
#include <xinput.h>

#include <atomic>
#include <chrono>
#include <mutex>

#include "hooks.h"
#include "input.h"
#include "log.h"

namespace ssr::input {
namespace {
using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
GetStateFn o_get_state = nullptr;

std::mutex g_mu;
WORD g_virtual = 0;
std::chrono::steady_clock::time_point g_virtual_until{};
DWORD g_packet = 1;
bool g_virtual_seen = false;  // after the first virtual press pad 0 stays "connected" so the game keeps polling it
std::atomic<bool> g_block{false};
XINPUT_GAMEPAD g_last_real{};
std::atomic<uint16_t> g_reserved{0};  // buttons the game must not see (handled by our UI)

DWORD WINAPI hk_get_state(DWORD idx, XINPUT_STATE* st) {
  DWORD r = o_get_state(idx, st);
  if (idx != 0) return g_block ? ERROR_DEVICE_NOT_CONNECTED : r;
  std::scoped_lock lk(g_mu);
  if (r == ERROR_SUCCESS) g_last_real = st->Gamepad;
  else g_last_real = {};
  const bool virt = g_virtual && std::chrono::steady_clock::now() < g_virtual_until;
  if (g_block) {
    // Story UI owns input: the game sees a connected, idle pad.
    *st = {};
    st->dwPacketNumber = ++g_packet;
    return ERROR_SUCCESS;
  }
  if (virt || (g_virtual_seen && r != ERROR_SUCCESS)) {
    if (r != ERROR_SUCCESS) *st = {};
    if (virt) st->Gamepad.wButtons |= g_virtual;
    st->dwPacketNumber = ++g_packet;
    r = ERROR_SUCCESS;
  }
  if (r == ERROR_SUCCESS && (st->Gamepad.wButtons & g_reserved)) {
    static int logged = 0;
    if (logged++ < 3) log::info("input: reserved buttons 0x{:x} hidden from the game", static_cast<unsigned>(g_reserved.load()));
    st->Gamepad.wButtons &= static_cast<WORD>(~g_reserved);
  }
  return r;
}
}  // namespace

void install() {
  HMODULE m = GetModuleHandleW(L"xinput9_1_0.dll");
  if (!m) m = LoadLibraryW(L"xinput9_1_0.dll");
  void* target = m ? reinterpret_cast<void*>(GetProcAddress(m, "XInputGetState")) : nullptr;
  if (!target) {
    log::error("XInputGetState not found");
    return;
  }
  hooks::install("XInputGetState", target, reinterpret_cast<void*>(&hk_get_state), reinterpret_cast<void**>(&o_get_state));
}

void press(uint16_t buttons, int ms) {
  std::scoped_lock lk(g_mu);
  g_virtual = buttons;
  g_virtual_seen = true;
  g_virtual_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
}

void set_block_game(bool on) { g_block = on; }
void set_reserved(uint16_t buttons) { g_reserved = buttons; }

uint16_t real_buttons() {
  std::scoped_lock lk(g_mu);
  return g_last_real.wButtons;
}

uint16_t virtual_buttons() {
  std::scoped_lock lk(g_mu);
  return std::chrono::steady_clock::now() < g_virtual_until ? g_virtual : 0;
}

uint16_t parse_button(const std::string& n) {
  struct B { const char* n; WORD b; };
  static const B tbl[] = {{"A", XINPUT_GAMEPAD_A}, {"B", XINPUT_GAMEPAD_B}, {"X", XINPUT_GAMEPAD_X}, {"Y", XINPUT_GAMEPAD_Y},
                          {"START", XINPUT_GAMEPAD_START}, {"BACK", XINPUT_GAMEPAD_BACK}, {"UP", XINPUT_GAMEPAD_DPAD_UP},
                          {"DOWN", XINPUT_GAMEPAD_DPAD_DOWN}, {"LEFT", XINPUT_GAMEPAD_DPAD_LEFT}, {"RIGHT", XINPUT_GAMEPAD_DPAD_RIGHT},
                          {"LB", XINPUT_GAMEPAD_LEFT_SHOULDER}, {"RB", XINPUT_GAMEPAD_RIGHT_SHOULDER},
                          {"LS", XINPUT_GAMEPAD_LEFT_THUMB}, {"RS", XINPUT_GAMEPAD_RIGHT_THUMB}};
  for (auto& b : tbl)
    if (_stricmp(b.n, n.c_str()) == 0) return b.b;
  return 0;
}

}  // namespace ssr::input
