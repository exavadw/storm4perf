// .nsreplay container:
//   "NSRP" u16 version(1) u16 flags(1 = XPRESS_HUFF body) u32 body_size u32 stored_size, then the body.
//   body = chunks { char id[4]; u32 size; u8 data[size] }. Readers skip unknown chunks, so later versions stay readable.
//   META (key=value lines) · BSP_ · GLOB · SEGS · ANC3 · PRE0 · AIT0 · PAD0/PAD1 · PTK0/PTK1 · HASH · JRAW
// Compression uses the Windows Compression API (cabinet.dll), so there is no extra dependency.
#include <windows.h>
#include <compressapi.h>

#include <cstdio>
#include <cstring>
#include <format>
#include <sstream>

#include "replay_data.h"

#pragma comment(lib, "Cabinet.lib")

namespace ssr::replay {
namespace {
constexpr char kMagic[4] = {'N', 'S', 'R', 'P'};
constexpr uint16_t kVersion = 1;

struct Writer {
  std::vector<uint8_t> out;
  void raw(const void* p, size_t n) {
    auto* b = static_cast<const uint8_t*>(p);
    out.insert(out.end(), b, b + n);
  }
  template <class T> void pod(const T& v) { raw(&v, sizeof v); }
  void chunk(const char id[4], const void* p, size_t n) {
    raw(id, 4);
    pod(static_cast<uint32_t>(n));
    raw(p, n);
  }
  template <class T> void vec(const char id[4], const std::vector<T>& v) { chunk(id, v.data(), v.size() * sizeof(T)); }
};

std::string meta_text(const Meta& m) {
  return std::format("created={}\nexe_sha256={}\nfps={}\nmode={}\nstage={}\nstage_name_id={}\nchara0={}\nchara1={}\npreset0={}\npreset1={}\nticks={}\nrounds={}\nresult={}\nstart={}\n",
                     m.created, m.exe_sha256, m.fps, m.mode, m.stage, m.stage_name_id, m.chara[0], m.chara[1], m.preset[0], m.preset[1], m.ticks,
                     m.rounds, m.result, m.start);
}

void parse_meta(const std::string& s, Meta& m) {
  std::istringstream in(s);
  std::string line;
  while (std::getline(in, line)) {
    const auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
    auto num = [&] { return static_cast<int64_t>(std::strtoll(v.c_str(), nullptr, 10)); };
    if (k == "created") m.created = v;
    else if (k == "exe_sha256") m.exe_sha256 = v;
    else if (k == "fps") m.fps = static_cast<uint32_t>(num());
    else if (k == "mode") m.mode = static_cast<uint32_t>(num());
    else if (k == "stage") m.stage = static_cast<uint32_t>(num());
    else if (k == "stage_name_id") m.stage_name_id = v;
    else if (k == "start") m.start = v;
    else if (k == "chara0") m.chara[0] = static_cast<int32_t>(num());
    else if (k == "chara1") m.chara[1] = static_cast<int32_t>(num());
    else if (k == "preset0") m.preset[0] = static_cast<int32_t>(num());
    else if (k == "preset1") m.preset[1] = static_cast<int32_t>(num());
    else if (k == "ticks") m.ticks = static_cast<uint32_t>(num());
    else if (k == "rounds") m.rounds = static_cast<uint32_t>(num());
    else if (k == "result") m.result = static_cast<uint32_t>(num());
  }
}

template <class T> bool take_vec(const uint8_t* p, uint32_t n, std::vector<T>& v) {
  if (n % sizeof(T)) return false;
  v.resize(n / sizeof(T));
  if (n) std::memcpy(v.data(), p, n);
  return true;
}
}  // namespace

bool write_file(const std::wstring& path, const Recording& r, std::string* err) {
  Writer w;
  const std::string meta = meta_text(r.meta);
  w.chunk("META", meta.data(), meta.size());
  w.vec("BSP_", r.bsp);
  w.vec("GLOB", r.globals);
  w.vec("SEGS", r.segs);
  w.vec("ANC3", r.anchors);
  if (!r.pre.empty()) w.vec("PRE0", r.pre);
  if (!r.ai_timers.empty()) w.vec("AIT0", r.ai_timers);
  w.vec("PAD0", r.pads[0]);
  w.vec("PAD1", r.pads[1]);
  w.vec("PTK0", r.pad_tick[0]);
  w.vec("PTK1", r.pad_tick[1]);
  w.vec("HASH", r.hashes);
  w.vec("JRAW", r.judge_raw);

  COMPRESSOR_HANDLE c = nullptr;
  if (!CreateCompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, nullptr, &c)) {
    if (err) *err = std::format("CreateCompressor {}", GetLastError());
    return false;
  }
  SIZE_T need = 0;
  Compress(c, w.out.data(), w.out.size(), nullptr, 0, &need);
  std::vector<uint8_t> packed(need);
  const bool ok = Compress(c, w.out.data(), w.out.size(), packed.data(), packed.size(), &need);
  CloseCompressor(c);
  if (!ok) {
    if (err) *err = std::format("Compress {}", GetLastError());
    return false;
  }
  packed.resize(need);

  const std::wstring tmp = path + L".part";
  std::FILE* f = _wfopen(tmp.c_str(), L"wb");
  if (!f) {
    if (err) *err = "cannot create file";
    return false;
  }
  const uint16_t version = kVersion, flags = 1;
  const uint32_t body = static_cast<uint32_t>(w.out.size()), stored = static_cast<uint32_t>(packed.size());
  std::fwrite(kMagic, 1, 4, f);
  std::fwrite(&version, 2, 1, f);
  std::fwrite(&flags, 2, 1, f);
  std::fwrite(&body, 4, 1, f);
  std::fwrite(&stored, 4, 1, f);
  const bool wrote = std::fwrite(packed.data(), 1, packed.size(), f) == packed.size();
  std::fclose(f);
  if (!wrote || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    DeleteFileW(tmp.c_str());
    if (err) *err = "write failed";
    return false;
  }
  return true;
}

bool read_file(const std::wstring& path, Recording& r, std::string* err) {
  auto fail = [&](const char* why) {
    if (err) *err = why;
    return false;
  };
  std::FILE* f = _wfopen(path.c_str(), L"rb");
  if (!f) return fail("cannot open file");
  char magic[4];
  uint16_t version = 0, flags = 0;
  uint32_t body = 0, stored = 0;
  const bool head = std::fread(magic, 1, 4, f) == 4 && std::fread(&version, 2, 1, f) == 1 && std::fread(&flags, 2, 1, f) == 1 &&
                    std::fread(&body, 4, 1, f) == 1 && std::fread(&stored, 4, 1, f) == 1;
  if (!head || std::memcmp(magic, kMagic, 4) != 0) {
    std::fclose(f);
    return fail("not a .nsreplay file");
  }
  if (version > kVersion) {
    std::fclose(f);
    return fail("made by a newer replay runtime");
  }
  if (body > 512u << 20 || stored > 512u << 20) {
    std::fclose(f);
    return fail("corrupt header");
  }
  std::vector<uint8_t> packed(stored);
  const bool got = std::fread(packed.data(), 1, stored, f) == stored;
  std::fclose(f);
  if (!got) return fail("truncated file");

  std::vector<uint8_t> data(body);
  if (flags & 1) {
    DECOMPRESSOR_HANDLE d = nullptr;
    if (!CreateDecompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, nullptr, &d)) return fail("CreateDecompressor failed");
    SIZE_T n = 0;
    const bool ok = Decompress(d, packed.data(), packed.size(), data.data(), data.size(), &n);
    CloseDecompressor(d);
    if (!ok || n != body) return fail("decompression failed");
  } else {
    data = std::move(packed);
  }

  Recording out;
  size_t pos = 0;
  while (pos + 8 <= data.size()) {
    char id[5]{};
    std::memcpy(id, &data[pos], 4);
    uint32_t n = 0;
    std::memcpy(&n, &data[pos + 4], 4);
    pos += 8;
    if (pos + n > data.size()) return fail("truncated chunk");
    const uint8_t* p = &data[pos];
    const std::string_view k(id, 4);
    bool ok = true;
    if (k == "META") parse_meta(std::string(reinterpret_cast<const char*>(p), n), out.meta);
    else if (k == "BSP_") ok = take_vec(p, n, out.bsp);
    else if (k == "GLOB") ok = take_vec(p, n, out.globals);
    else if (k == "SEGS") ok = take_vec(p, n, out.segs);
    else if (k == "ANC3") ok = take_vec(p, n, out.anchors);
    else if (k == "PRE0") ok = take_vec(p, n, out.pre);
    else if (k == "AIT0") ok = take_vec(p, n, out.ai_timers);
    else if (k == "PAD0") ok = take_vec(p, n, out.pads[0]);
    else if (k == "PAD1") ok = take_vec(p, n, out.pads[1]);
    else if (k == "PTK0") ok = take_vec(p, n, out.pad_tick[0]);
    else if (k == "PTK1") ok = take_vec(p, n, out.pad_tick[1]);
    else if (k == "HASH") ok = take_vec(p, n, out.hashes);
    else if (k == "JRAW") ok = take_vec(p, n, out.judge_raw);
    if (!ok) return fail("bad chunk size");
    pos += n;
  }
  if (out.segs.empty() || out.hashes.empty()) return fail("no battle data");
  if (out.pad_tick[0].size() != out.pads[0].size() || out.pad_tick[1].size() != out.pads[1].size()) return fail("inconsistent pad data");
  r = std::move(out);
  return true;
}

}  // namespace ssr::replay
