// A tiny test framework in the shape of GoogleTest and Catch2, so hs verify
// can be tested without either installed. Run with no arguments for every
// test, or with one argument: test names separated by "," ("\," inside a
// name), as Catch2 takes them.
#include <cstdio>
#include <string>
#include <vector>

#include "../mathlib.h"

struct Case {
  std::string name;
  void (*fn)();
};
static std::vector<Case>& cases() {
  static std::vector<Case> c;
  return c;
}
struct Reg {
  Reg(std::string n, void (*f)()) { cases().push_back({n, f}); }
};
static int failures = 0;
#define EXPECT(cond) \
  if (!(cond)) { std::printf("  failed: %s\n", #cond); ++failures; }
#define HS_CAT2(a, b) a##b
#define HS_CAT(a, b) HS_CAT2(a, b)
#define TEST(suite, name)                                  \
  static void suite##_##name();                            \
  static Reg reg_##suite##_##name(#suite "." #name, suite##_##name); \
  static void suite##_##name()
#define TEST_CASE(name)                                    \
  static void HS_CAT(tc_, __LINE__)();                     \
  static Reg HS_CAT(reg_, __LINE__)(name, HS_CAT(tc_, __LINE__)); \
  static void HS_CAT(tc_, __LINE__)()

TEST(Math, Add) {
  EXPECT(add(2, 3) == 5);
}

// Catch2 style, with a comma in the name.
TEST_CASE("mul, by adding") {
  EXPECT(mul(4, 3) == 12);
}

TEST(Math, Neg) {
  EXPECT(neg(4) == -4);
}

static std::vector<std::string> split_names(const std::string& s) {
  std::vector<std::string> out(1);
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size()) out.back() += s[++i];
    else if (s[i] == ',') out.emplace_back();
    else out.back() += s[i];
  }
  return out;
}

int main(int argc, char** argv) {
  std::vector<std::string> want;
  if (argc > 1) want = split_names(argv[1]);
  for (auto& w : want) {
    bool found = false;
    for (auto& c : cases()) found = found || w == c.name;
    if (!found) {
      std::printf("no test named \"%s\"\n", w.c_str());
      return 2;
    }
  }
  for (auto& c : cases()) {
    bool run = want.empty();
    for (auto& w : want) run = run || w == c.name;
    if (!run) continue;
    std::printf("%s\n", c.name.c_str());
    c.fn();
  }
  return failures ? 1 : 0;
}
