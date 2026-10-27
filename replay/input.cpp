#include "input.h"

#include <windows.h>
#include <xinput.h>

#include <atomic>
#include <chrono>

extern "C" DWORD (*s4p_pad_filter)(DWORD pad, void* state, DWORD result);  // storm4perf.cpp

namespace ssr::input {
namespace {
using Clock = std::chrono::steady_clock;
std::atomic<uint16_t> g_real{0}, g_reserved{0}, g_added{0}, g_real2{0};
std::atomic<bool> g_merge{false}, g_conn2{false};
std::atomic<int64_t> g_added_until{0};  // Clock ticks
DWORD g_packet_bump = 0;                // game thread only (pad reads happen there)
WORD g_last_given = 0;

uint16_t added_now() {
  return Clock::now().time_since_epoch().count() < g_added_until.load() ? g_added.load() : uint16_t{0};
}

DWORD filter(DWORD pad, void* state, DWORD result) {
  if (!state) return result;
  auto* st = static_cast<XINPUT_STATE*>(state);
  if (pad == 1) {  // merged: remember pad 2's buttons for pad 1, show the game a connected pad that holds nothing
    if (!g_merge.load()) return result;
    g_conn2 = result == ERROR_SUCCESS;
    g_real2 = g_conn2.load() ? st->Gamepad.wButtons : 0;
    *st = {};
    return ERROR_SUCCESS;
  }
  if (pad != 0) return result;
  const bool connected = result == ERROR_SUCCESS;
  g_real = static_cast<uint16_t>((connected ? st->Gamepad.wButtons : 0) | (g_merge.load() ? g_real2.load() : 0));
  const uint16_t add = static_cast<uint16_t>(added_now() | (g_merge.load() ? g_real2.load() : 0));
  if (!connected) {
    if (!add && !(g_merge.load() && g_conn2.load())) return result;  // merged: pad 2 alone keeps pad 1 connected
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
void set_merge_pad2(bool on) {
  g_merge = on;
  if (!on) g_real2 = 0, g_conn2 = false;
}
uint16_t real_buttons() { return g_real.load(); }
uint16_t virtual_buttons() { return added_now(); }
}  // namespace ssr::input
