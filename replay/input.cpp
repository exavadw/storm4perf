#include "input.h"

#include <windows.h>
#include <xinput.h>

#include <atomic>
#include <chrono>

extern "C" DWORD (*s4p_pad_filter)(DWORD pad, void* state, DWORD result);  // storm4perf.cpp

namespace ssr::input {
namespace {
using Clock = std::chrono::steady_clock;
std::atomic<uint16_t> g_real{0}, g_reserved{0}, g_added{0};
std::atomic<int64_t> g_added_until{0};  // Clock ticks
DWORD g_packet_bump = 0;                // game thread only (pad reads happen there)
WORD g_last_given = 0;

uint16_t added_now() {
  return Clock::now().time_since_epoch().count() < g_added_until.load() ? g_added.load() : uint16_t{0};
}

DWORD filter(DWORD pad, void* state, DWORD result) {
  if (pad != 0 || !state) return result;
  auto* st = static_cast<XINPUT_STATE*>(state);
  const bool connected = result == ERROR_SUCCESS;
  g_real = connected ? st->Gamepad.wButtons : 0;
  const uint16_t add = added_now();
  if (!connected) {
    if (!add) return result;
    *st = {};  // a virtual press works without a physical pad
    result = ERROR_SUCCESS;
  }
  const WORD given = static_cast<WORD>((st->Gamepad.wButtons | add) & ~g_reserved.load());
  if (given != g_last_given) ++g_packet_bump;  // the game sees a new packet whenever the buttons change
  g_last_given = given;
  st->Gamepad.wButtons = given;
  st->dwPacketNumber += g_packet_bump;
  return result;
}
}  // namespace

void install() { s4p_pad_filter = &filter; }

void press(uint16_t buttons, int ms) {
  g_added = buttons;
  g_added_until = (Clock::now() + std::chrono::milliseconds(ms)).time_since_epoch().count();
}

void set_reserved(uint16_t buttons) { g_reserved = buttons; }
uint16_t real_buttons() { return g_real.load(); }
uint16_t virtual_buttons() { return added_now(); }
}  // namespace ssr::input
