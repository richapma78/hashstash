#pragma once
// The object model. Every object is stored as canonical JSON (sorted keys,
// compact) except raw file blobs, which are stored as their bytes.
//
//   def        one definition; its parts are its text, interleaved with child
//              definitions (a class's methods) and fragments
//   frag       a piece of a large definition, placed back at a crumb
//   file       a source file's crumbtrail: glue text and definitions in order
//   blob       a non-source or non-UTF-8 file, stored as raw bytes
//   namespace  the project tree: files, plus each definition's lineage id
//   commit     namespace + parents + author/time/message
#include <map>
#include <set>
#include <string>
#include <vector>

#include "core/hash.h"
#include "core/store.h"

namespace hs {

struct Part {
  enum Type { Text, Def, Frag } type = Text;
  std::string text;  // Text: the source text
  Hash hash;         // Def/Frag: the referenced object
};

struct Def {
  std::string lang;
  std::string kind;  // function, method, class, ...
  std::string name;  // short name as written: "parse", "Server"
  Hash norm;         // hash of the token stream without comments/whitespace
  int lines = 0;     // lines of its own text (children excluded)
  std::vector<Part> parts;
};

struct Frag {
  std::string id;    // stable fragment id (the crumb id)
  std::string label;
  std::string text;
};

struct FileTrail {
  std::string lang;  // language adapter that split it
  std::vector<Part> parts;
};

struct FileEntry {
  std::string kind;  // "file" or "blob"
  Hash hash;
};

struct Namespace {
  std::map<std::string, FileEntry> files;     // path -> entry
  std::map<std::string, std::string> lineage;  // qualified name -> lineage id
  // Test cases designed for each definition: lineage -> test lineages.
  // Many tests per definition; a test may cover several definitions.
  std::map<std::string, std::set<std::string>> tests;
  // Comment regions and context history per definition: lineage -> attach object.
  std::map<std::string, Hash> attach;
};

// A comment about a region of a definition, kept beside the code, not in it.
// Lines are 1-based in the definition's full text; regions may overlap.
struct Note {
  std::string id;
  int from = 0, to = 0;
  std::string text, author;
  long long time = 0;
  bool stale = false;  // its lines were removed by an edit
};

// Why a definition exists or changed: the original request, design snippets,
// change reasons. Kept in order, so it reads as the definition's story.
struct ContextEntry {
  std::string id;
  std::string kind;    // origin | design | change | note
  std::string text;
  std::string source;  // e.g. "docs/DESIGN.md#locks", a ticket, a chat
  std::string author;
  long long time = 0;
  Hash def_hash;       // the version it was written against
};

struct Attach {
  std::vector<Note> notes;
  std::vector<ContextEntry> context;
};

struct Commit {
  Hash ns;
  std::vector<Hash> parents;
  std::string author;
  long long time = 0;
  std::string message;               // required description of the change
  std::vector<std::string> changes;  // generated: "changed pkg/a.py:f", ...
};

Hash put_def(Store& s, const Def& d);
Hash put_frag(Store& s, const Frag& f);
Hash put_file(Store& s, const FileTrail& f);
Hash put_namespace(Store& s, const Namespace& n);
Hash put_commit(Store& s, const Commit& c);
Hash put_attach(Store& s, const Attach& a);
Attach load_attach(const Store& s, const Hash& h);

Def load_def(const Store& s, const Hash& h);
Frag load_frag(const Store& s, const Hash& h);
FileTrail load_file(const Store& s, const Hash& h);
Namespace load_namespace(const Store& s, const Hash& h);
Commit load_commit(const Store& s, const Hash& h);

// Hashes an object refers to (commit -> namespace + parents, namespace -> files
// and attachments, file/def -> definitions and fragments).
std::vector<Hash> object_refs(const Object& o);

// Full source text, with child definitions and fragments expanded in place.
std::string render_parts(const Store& s, const std::vector<Part>& parts);
std::string render_def(const Store& s, const Hash& h);
std::string render_entry(const Store& s, const FileEntry& e);

// A definition as it appears in a namespace, found by walking the crumbtrails.
struct FlatDef {
  std::string qname;   // "pkg/mod.py:Server.handle"
  std::string file;    // "pkg/mod.py"
  std::string path;    // "Server.handle"
  std::string parent;  // qualified name of the containing definition, or ""
  int seq = 0;         // position within the file, in render order
  Hash hash;
  std::string lineage;
  Def def;
};

std::vector<FlatDef> flatten(const Store& s, const Namespace& n);
std::vector<FlatDef> flatten_file(const Store& s, const Namespace& n, const std::string& file);

std::string qualify(const std::string& file, const std::string& path);
// Repeated names in one scope get "~2", "~3": a property and its setter are
// "Server.port" and "Server.port~2".
std::string member_name(const std::string& name, int occurrence);
int count_lines(const std::string& text);

}  // namespace hs
