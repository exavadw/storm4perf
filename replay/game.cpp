#include "game.h"

#include <windows.h>
#include <bcrypt.h>

#include <cstdio>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace ssr::game {
namespace {
uint8_t* g_image = nullptr;
Info g_info;

std::string sha256_of_file(const wchar_t* path) {
  FILE* f = _wfopen(path, L"rb");
  if (!f) return {};
  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_HASH_HANDLE h = nullptr;
  std::string hex;
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 && BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
    std::vector<unsigned char> chunk(1 << 20);
    for (size_t n; (n = fread(chunk.data(), 1, chunk.size(), f)) > 0;) BCryptHashData(h, chunk.data(), static_cast<ULONG>(n), 0);
    unsigned char digest[32];
    if (BCryptFinishHash(h, digest, sizeof digest, 0) == 0) {
      static const char kHex[] = "0123456789abcdef";
      for (unsigned char b : digest) hex += kHex[b >> 4], hex += kHex[b & 15];
    }
  }
  if (h) BCryptDestroyHash(h);
  if (alg) BCryptCloseAlgorithmProvider(alg, 0);
  fclose(f);
  return hex;
}
}  // namespace

void init() { g_image = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr)); }

uint8_t* image() {
  if (!g_image) init();
  return g_image;
}

void verify() {
  wchar_t path[MAX_PATH];
  if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return;
  g_info.sha256 = sha256_of_file(path);
  g_info.supported = g_info.sha256 == kSteam109;
}

const Info& info() { return g_info; }
}  // namespace ssr::game
