#pragma once
#include <filesystem>
#include <format>
#include <string>
#include <string_view>

namespace ssr::log {

void init(const std::filesystem::path& file);
void write(std::string_view level, std::string_view msg);

template <class... A> void info(std::format_string<A...> f, A&&... a) { write("INFO", std::format(f, std::forward<A>(a)...)); }
template <class... A> void warn(std::format_string<A...> f, A&&... a) { write("WARN", std::format(f, std::forward<A>(a)...)); }
template <class... A> void error(std::format_string<A...> f, A&&... a) { write("ERROR", std::format(f, std::forward<A>(a)...)); }

}  // namespace ssr::log
