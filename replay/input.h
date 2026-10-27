#pragma once
// Controller access for the replay UI, through storm4perf's own XInputGetState export (pad 0 = the player).
#include <cstdint>

namespace ssr::input {
void install();                                // start filtering pad reads
void press(uint16_t xinput_buttons, int ms);   // add these buttons to pad 0 for ms (e.g. a Down to move a menu cursor)
void set_reserved(uint16_t xinput_buttons);    // pad 0 buttons the game must not see (our controls use them)
void set_merge_pad2(bool on);                  // pad 2 drives pad 1 and looks connected and idle to the game
uint16_t real_buttons();                       // pad 0 buttons physically held at the last read (+ pad 2's when merged)
uint16_t virtual_buttons();                    // buttons press() is adding right now
}  // namespace ssr::input
