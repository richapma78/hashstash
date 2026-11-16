#pragma once
#include <functional>
#include <string>
#include <vector>

#include "ops/repo.h"

namespace hs {

struct SizeWarning {
  std::string qname;
  int lines = 0;
  int limit = 0;
  std::string suggestion;
};

struct ImportResult {
  int files = 0;       // source files split into definitions
  int blobs = 0;       // other files kept whole
  int defs = 0;
  int skipped = 0;     // ignored paths
  std::vector<SizeWarning> warnings;
};

// Imports a folder (or single file) into the working namespace under prefix.
// Skips VCS/build/cache folders, .gitignore / .hsignore patterns and
// [import] ignore from the config.
ImportResult import_path(Repo& repo, const fs::path& src, const std::string& prefix = "");

struct RenderResult {
  int written = 0;
  int unchanged = 0;
  int deleted = 0;
  int ignored = 0;  // files left out by skip
};

// Generates source files for the working namespace (or a given namespace) into
// out_dir. Only changed files are rewritten; files from the previous render
// that no longer exist (or are now skipped) are deleted.
RenderResult render_to(Repo& repo, const fs::path& out_dir, const Hash& ns_hash = "",
                       const std::function<bool(const std::string& path)>& skip = {});

std::vector<SizeWarning> size_warnings(const Repo& repo, const std::vector<FlatDef>& defs);

}  // namespace hs
