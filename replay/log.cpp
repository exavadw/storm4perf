#include "log.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <mutex>

namespace ssr::log {
namespace {
std::mutex g_mu;
FILE* g_file = nullptr;
}  // namespace

void init(const std::filesystem::path& file) {
  std::scoped_lock lk(g_mu);
  if (g_file) return;
  std::error_code ec;
  std::filesystem::create_directories(file.parent_path(), ec);
  g_file = _wfsopen(file.c_str(), L"w", _SH_DENYWR);
  if (!g_file) {  // another game instance holds runtime.log
    auto alt = file;
    alt.replace_filename(file.stem().wstring() + L"-" + std::to_wstring(GetCurrentProcessId()) + file.extension().wstring());
    g_file = _wfsopen(alt.c_str(), L"w", _SH_DENYWR);
  }
}

void write(std::string_view level, std::string_view msg) {
  const auto now = std::chrono::system_clock::now();
  const auto line = std::format("{:%H:%M:%S} [{}] [t{}] {}\n", std::chrono::floor<std::chrono::milliseconds>(now), level,
                                GetCurrentThreadId(), msg);
  std::scoped_lock lk(g_mu);
  if (g_file) {
    std::fputs(line.c_str(), g_file);
    std::fflush(g_file);
  }
  OutputDebugStringA(line.c_str());
}

}  // namespace ssr::log
