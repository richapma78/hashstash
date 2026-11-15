#include "core/util.h"

#include <chrono>
#include <ctime>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace hs {

std::string read_file(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) throw std::runtime_error("cannot read " + to_generic(p));
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void write_file(const fs::path& p, std::string_view bytes) {
  if (p.has_parent_path()) fs::create_directories(p.parent_path());
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  if (!out) throw std::runtime_error("cannot write " + to_generic(p));
  out.write(bytes.data(), (std::streamsize)bytes.size());
  if (!out) throw std::runtime_error("write failed: " + to_generic(p));
}

bool is_utf8(std::string_view s) {
  size_t i = 0, n = s.size();
  while (i < n) {
    unsigned char c = (unsigned char)s[i];
    if (c < 0x80) {
      if (c == 0) return false;  // NUL bytes: treat as binary
      ++i;
      continue;
    }
    int len = (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    if (!len || i + len > n) return false;
    unsigned cp = c & (0x7f >> len);
    for (int k = 1; k < len; ++k) {
      unsigned char d = (unsigned char)s[i + k];
      if ((d >> 6) != 2) return false;
      cp = (cp << 6) | (d & 0x3f);
    }
    if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) ||
        cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
      return false;
    i += len;
  }
  return true;
}

long long now_seconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string format_time(long long t) {
  std::time_t tt = (std::time_t)t;
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &tt);
#else
  localtime_r(&tt, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm);
  return buf;
}

std::vector<std::string> split_lines(std::string_view s) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < s.size()) {
    size_t eol = s.find('\n', i);
    size_t end = eol == std::string_view::npos ? s.size() : eol + 1;
    out.emplace_back(s.substr(i, end - i));
    i = end;
  }
  return out;
}

bool starts_with(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }
bool ends_with(std::string_view s, std::string_view p) {
  return s.size() >= p.size() && s.substr(s.size() - p.size()) == p;
}

std::string trim(std::string_view s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string_view::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return std::string(s.substr(a, b - a + 1));
}

std::string to_generic(const fs::path& p) {
  auto u = p.generic_u8string();
  return std::string(u.begin(), u.end());
}

fs::path from_utf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

bool glob_match(std::string_view pat, std::string_view txt) {
  if (pat.empty()) return txt.empty();
  if (starts_with(pat, "**")) {
    std::string_view rest = pat.substr(2);
    if (starts_with(rest, "/") && glob_match(rest.substr(1), txt)) return true;  // zero folders
    for (size_t i = 0; i <= txt.size(); ++i)
      if (glob_match(rest, txt.substr(i))) return true;
    return false;
  }
  if (pat[0] == '*') {
    for (size_t i = 0; i <= txt.size(); ++i) {
      if (glob_match(pat.substr(1), txt.substr(i))) return true;
      if (i < txt.size() && txt[i] == '/') break;
    }
    return false;
  }
  if (txt.empty()) return false;
  if (pat[0] == '?' ? txt[0] == '/' : pat[0] != txt[0]) return false;
  return glob_match(pat.substr(1), txt.substr(1));
}

}  // namespace hs
