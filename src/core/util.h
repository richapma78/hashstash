#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace hs {

namespace fs = std::filesystem;

std::string read_file(const fs::path& p);                    // binary; throws on error
void write_file(const fs::path& p, std::string_view bytes);  // binary; creates parent dirs
bool is_utf8(std::string_view s);
long long now_seconds();
std::string format_time(long long t);  // local "YYYY-MM-DD HH:MM"
std::vector<std::string> split_lines(std::string_view s);  // keeps line endings
bool starts_with(std::string_view s, std::string_view p);
bool ends_with(std::string_view s, std::string_view p);
std::string trim(std::string_view s);
std::string to_generic(const fs::path& p);  // UTF-8, forward slashes
fs::path from_utf8(const std::string& s);
struct RunResult {
  int code = -1;
  std::string output;  // stdout and stderr together
};
// Runs a shell command in dir and waits for it.
RunResult run_command(const fs::path& dir, const std::string& command);
std::string shell_quote(const std::string& s);

std::string base64_encode(std::string_view in);
std::string base64_decode(std::string_view in);

// Glob with * (no slash), ** (anything) and ?.
bool glob_match(std::string_view pattern, std::string_view text);

}  // namespace hs
