#pragma once
// Controller access for the replay UI, through storm4perf's own XInputGetState export (pad 0 = the player).
#include <cstdint>

namespace ssr::input {
void install();                                // start filtering pad reads
void press(uint16_t xinput_buttons, int ms);   // add these buttons to pad 0 for ms (e.g. a Down to move a menu cursor)
void set_reserved(uint16_t xinput_buttons);    // pad 0 buttons the game must not see (our controls use them)
uint16_t real_buttons();                       // pad 0 buttons physically held at the last read
uint16_t virtual_buttons();                    // buttons press() is adding right now
}  // namespace ssr::input
