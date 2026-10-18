#pragma once
// Replay log (StormReplay\logs\replay.log): one line per call, "hh:mm:ss.mmm LEVEL tTID message", flushed at once.
#include <filesystem>
#include <format>
#include <string_view>

namespace ssr::log {
void init(const std::filesystem::path& file);  // creates the folder; the previous log becomes replay.prev.log
void line(const char* level, std::string_view text);

template <class... A> void info(std::format_string<A...> f, A&&... a) { line("INFO", std::format(f, std::forward<A>(a)...)); }
template <class... A> void warn(std::format_string<A...> f, A&&... a) { line("WARN", std::format(f, std::forward<A>(a)...)); }
template <class... A> void error(std::format_string<A...> f, A&&... a) { line("ERROR", std::format(f, std::forward<A>(a)...)); }
}  // namespace ssr::log
