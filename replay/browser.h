#pragma once
#include <functional>
#include <string>

// Replay browser in the game's own select window, opened from the Free Battle menu (button guide entry, Y).
namespace ssr::browser {
void install();
void tick();  // game thread, every frame
void open();  // game thread (also: pipe "browser")
// Starts the armed replay's battle without character select; set once direct launch is available. Returns false if
// it cannot launch from the current screen.
void set_launcher(std::function<bool()> launch);
std::string status();
std::string dump(bool recent);
}  // namespace ssr::browser
