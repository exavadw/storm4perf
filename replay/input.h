#pragma once
#include <cstdint>
#include <string>

namespace ssr::input {
void install();
void press(uint16_t xinput_buttons, int ms);  // virtual pad 0 buttons held for ms
void set_block_game(bool on);                 // game sees an idle pad while a story screen is up
void set_reserved(uint16_t buttons);          // these buttons never reach the game (our own shortcuts)
uint16_t real_buttons();                      // last physical pad 0 buttons (for story UI)
uint16_t virtual_buttons();                   // currently injected buttons (so automation also drives story UI)
uint16_t parse_button(const std::string&);    // "A", "START", "DOWN", ... → XINPUT mask (0 if unknown)
}  // namespace ssr::input
