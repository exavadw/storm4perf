#include "log.h"

#include <windows.h>

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
  auto prev = file;
  prev.replace_extension(L".prev.log");
  std::filesystem::rename(file, prev, ec);  // keep one previous run
  g_file = _wfsopen(file.c_str(), L"w", _SH_DENYWR);
}

void line(const char* level, std::string_view text) {
  SYSTEMTIME t;
  GetLocalTime(&t);
  std::scoped_lock lk(g_mu);
  if (!g_file) return;
  fprintf(g_file, "%02u:%02u:%02u.%03u %-5s t%lu %.*s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, level, GetCurrentThreadId(),
          static_cast<int>(text.size()), text.data());
  fflush(g_file);
}
}  // namespace ssr::log
