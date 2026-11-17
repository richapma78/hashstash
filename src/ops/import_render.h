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

// Deletes whole files from the working namespace: each path is a file, or a folder
// (every file under it). Their definitions, lineage and test links go with them.
// Throws if a path matches nothing. Returns the removed file paths.
std::vector<std::string> remove_files(Repo& repo, const std::vector<std::string>& paths,
                                      const std::string& message = "");

struct RenderResult {
  int written = 0;
  int unchanged = 0;
  int deleted = 0;
  int ignored = 0;   // files left out by skip
  int reused = 0;    // of unchanged: not regenerated at all (same content hash, file untouched since)
};

// Generates source files for the working namespace (or a given namespace) into
// out_dir. Only changed files are rewritten, so a build sees new modification
// times only on files an update touched; files from the previous render that no
// longer exist (or are now skipped) are deleted.
// Delta render: a file whose content hash matches what the last render wrote
// there, and whose size and mtime on disk are still what that render left, is
// not regenerated at all (an edited, replaced or missing file always is).
// full = true regenerates and compares every file.
RenderResult render_to(Repo& repo, const fs::path& out_dir, const Hash& ns_hash = "",
                       const std::function<bool(const std::string& path)>& skip = {}, bool full = false);

std::vector<SizeWarning> size_warnings(const Repo& repo, const std::vector<FlatDef>& defs);

}  // namespace hs
