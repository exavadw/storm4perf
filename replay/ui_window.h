#pragma once
#include <functional>
#include <string>
#include <vector>

// The game's select window (the one behind the VS end menu, the pause menu and its yes/no questions), opened by us.
// Requests queue up: only one window exists at a time.
namespace ssr::ui {
struct Options {
  std::string title_id;               // message id of the title, "" for none
  bool cancel = true;                 // B closes the window (the callback gets -1)
  std::vector<std::string> desc_ids;  // optional, one per item: message id shown in the help bar while it is selected
  int start_row = 0;                  // row to select once the window has opened
};
void open(std::vector<std::string> item_ids, std::function<void(int choice)> on_choice, Options opts = {});
void yes_no(std::string title_id, std::function<void(bool yes)> on_answer);
void notice(std::string title_id, std::function<void()> on_ok = {});  // a single OK item
bool busy();             // a window is open or closing, or one is queued
bool is_open();          // a window is open and taking input
void dismiss(int code);  // close the open window now; its callback gets `code` (use values below -1)
void tick();             // call once per frame on the game thread
std::string debug();     // state of the open window (dev)
void play_se(int id);    // system sound: 0x14 decide, 0x10 cancel, 0x17 cursor, 0x18 window, 0x0F error
}  // namespace ssr::ui
