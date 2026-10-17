#pragma once
#include <functional>
#include <string>
#include <vector>

// The game's own select window (ccUiFlashSelectWindow — the one behind the VS end menu, the pause menu and every
// yes/no), opened from anywhere. One window at a time: a request made while one is open or closing waits its turn.
namespace ssr::ui {
struct Options {
  std::string title_id;               // message id shown as the window title ("" = none)
  bool cancel = true;                 // B closes the window (callback gets -1)
  std::vector<std::string> desc_ids;  // per item: message id shown in the native help bar while it is highlighted
  int start_row = 0;                  // highlight this row once the window is open
};
void open(std::vector<std::string> item_ids, std::function<void(int choice)> on_choice, Options opts = {});
void yes_no(std::string title_id, std::function<void(bool yes)> on_answer);
void notice(std::string title_id, std::function<void()> on_ok = {});  // one "OK" item
bool busy();   // a window is open, closing or queued
bool is_open();  // a window is open and taking input
void dismiss(int code);  // close the open window as if decided; its callback gets `code` (use values < -1)
void tick();   // game thread, every frame
std::string debug();  // open window fields (dev)
void play_se(int id);  // system SE: 0x14 decide, 0x10 cancel, 0x17 cursor, 0x18 window, 0x0F error
}  // namespace ssr::ui
