#pragma once
// Language adapters. An adapter splits a source file into definitions (with
// nested child definitions) by byte range, and analyzes a definition's own text
// for calls, signature, docstring and a normalized token hash.
//
// Split ranges are always whole lines, so everything between definitions is
// "glue" and slicing the original bytes reproduces the file exactly.
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/hash.h"

namespace hs {

struct Chunk {
  std::string kind;  // function, method, class, struct, ...
  std::string name;
  size_t begin = 0;  // byte range in the source, line-aligned
  size_t end = 0;
  std::vector<Chunk> children;  // nested definitions, within [begin, end)
};

struct CallSite {
  std::string name;  // as written: "foo", "self.bar", "os.path.join"
  int line = 0;      // 1-based within the analyzed text
};

// "import a.b as c"      -> {module "a.b", name "",  alias "c", level 0}
// "from ..a import b"    -> {module "a",   name "b", alias "",  level 2}
// "#include \"x/y.h\""   -> {module "x/y.h", ...}  (C/C++, later)
struct Import {
  std::string module;
  std::string name;
  std::string alias;
  int level = 0;
};

struct Analysis {
  Hash norm;
  std::string signature;
  std::string doc;
  std::vector<CallSite> calls;
  std::vector<Import> imports;
};

class Language {
 public:
  virtual ~Language() = default;
  virtual std::string name() const = 0;
  virtual bool handles(const std::string& path) const = 0;
  virtual std::string comment_prefix() const = 0;
  virtual std::vector<Chunk> split(std::string_view src) const = 0;
  virtual Analysis analyze(std::string_view text) const = 0;
};

const Language* language_for_path(const std::string& path);
const Language* language_by_name(const std::string& name);
const std::vector<std::unique_ptr<Language>>& languages();

// Shared helpers
std::vector<size_t> line_starts(std::string_view src);
std::string dedent(std::string_view text, std::string* removed_prefix = nullptr);
std::string indent(std::string_view text, const std::string& prefix);
std::string collapse_ws(std::string_view s);

}  // namespace hs
