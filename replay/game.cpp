#include "game.h"

#include <windows.h>
#include <bcrypt.h>

#include <cstdio>
#include <vector>

#include "log.h"

#pragma comment(lib, "bcrypt.lib")

namespace ssr::game {
namespace {
Info g_info;

std::string sha256_file(const std::filesystem::path& p) {
  FILE* f = _wfopen(p.c_str(), L"rb");
  if (!f) return {};
  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_HASH_HANDLE h = nullptr;
  std::string out;
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
      BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
    std::vector<unsigned char> buf(1 << 20);
    size_t n;
    while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0) BCryptHashData(h, buf.data(), static_cast<ULONG>(n), 0);
    unsigned char digest[32];
    if (BCryptFinishHash(h, digest, sizeof digest, 0) == 0) {
      static const char* hex = "0123456789abcdef";
      for (unsigned char c : digest) {
        out += hex[c >> 4];
        out += hex[c & 15];
      }
    }
  }
  if (h) BCryptDestroyHash(h);
  if (alg) BCryptCloseAlgorithmProvider(alg, 0);
  std::fclose(f);
  return out;
}
}  // namespace

const Info& info() { return g_info; }

void init() {
  if (g_info.base) return;
  g_info.base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
  wchar_t path[MAX_PATH]{};
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  const std::filesystem::path exe(path);
  g_info.exe_dir = exe.parent_path();
}

void verify() {
  wchar_t path[MAX_PATH]{};
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  const std::filesystem::path exe(path);
  g_info.sha256 = sha256_file(exe);
  g_info.supported = g_info.sha256 == kExeSha256;
  log::info("exe {} sha256={} supported={}", exe.filename().string(), g_info.sha256, g_info.supported);
}

}  // namespace ssr::game
