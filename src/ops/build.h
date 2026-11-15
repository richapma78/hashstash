#pragma once
// Turns source text into stored objects, and records each new definition's
// analysis (signature, doc, calls, imports) in the index tables.
#include <string>
#include <string_view>
#include <vector>

#include "core/model.h"
#include "lang/lang.h"

namespace hs {

// Parts covering src[begin, end): glue text with the given chunks stored as defs.
std::vector<Part> build_parts(Store& s, const Language& lang, std::string_view src,
                              const std::vector<Chunk>& chunks, size_t begin, size_t end);
Hash build_def(Store& s, const Language& lang, std::string_view src, const Chunk& c);

// Stores a whole file: split into a crumbtrail if a language handles it and it
// is valid UTF-8, otherwise kept as a raw blob.
FileEntry store_file(Store& s, const std::string& path, const std::string& bytes);

// Recomputes a definition's norm/lines from its parts, stores and analyzes it.
Hash finish_def(Store& s, const Language& lang, Def d);

// A definition's own text, with child definitions left out.
std::string own_text(const Def& d);

// Records analysis for a file's glue (module-level code), keyed by the file hash.
void analyze_file(Store& s, const Hash& file_hash, const FileTrail& f);

}  // namespace hs
