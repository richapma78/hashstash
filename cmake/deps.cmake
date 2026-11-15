# Third-party sources, downloaded at configure time and built as small static
# libraries with our own flags. SOURCE_SUBDIR points at a folder that doesn't
# exist so FetchContent only downloads and never runs the projects' own CMake.
include(FetchContent)
set(FETCHCONTENT_QUIET ON)

FetchContent_Declare(sqlite
  URL https://www.sqlite.org/2024/sqlite-amalgamation-3460100.zip
  DOWNLOAD_EXTRACT_TIMESTAMP ON SOURCE_SUBDIR _none_)
FetchContent_Declare(blake3
  URL https://github.com/BLAKE3-team/BLAKE3/archive/refs/tags/1.5.4.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP ON SOURCE_SUBDIR _none_)
FetchContent_Declare(treesitter
  URL https://github.com/tree-sitter/tree-sitter/archive/refs/tags/v0.24.7.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP ON SOURCE_SUBDIR _none_)
FetchContent_Declare(ts_python
  URL https://github.com/tree-sitter/tree-sitter-python/archive/refs/tags/v0.23.6.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP ON SOURCE_SUBDIR _none_)
FetchContent_Declare(nljson
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/include.zip
  DOWNLOAD_EXTRACT_TIMESTAMP ON SOURCE_SUBDIR _none_)
FetchContent_MakeAvailable(sqlite blake3 treesitter ts_python nljson)

# SQLite with full-text search
add_library(hs_sqlite STATIC ${sqlite_SOURCE_DIR}/sqlite3.c)
target_include_directories(hs_sqlite PUBLIC ${sqlite_SOURCE_DIR})
target_compile_definitions(hs_sqlite PUBLIC
  SQLITE_ENABLE_FTS5 SQLITE_THREADSAFE=1 SQLITE_DEFAULT_FOREIGN_KEYS=1 SQLITE_OMIT_LOAD_EXTENSION)

# BLAKE3, portable C implementation (SIMD paths can be enabled later)
add_library(hs_blake3 STATIC
  ${blake3_SOURCE_DIR}/c/blake3.c
  ${blake3_SOURCE_DIR}/c/blake3_dispatch.c
  ${blake3_SOURCE_DIR}/c/blake3_portable.c)
target_include_directories(hs_blake3 PUBLIC ${blake3_SOURCE_DIR}/c)
target_compile_definitions(hs_blake3 PRIVATE
  BLAKE3_NO_SSE2 BLAKE3_NO_SSE41 BLAKE3_NO_AVX2 BLAKE3_NO_AVX512 BLAKE3_USE_NEON=0)

# tree-sitter runtime plus grammars
add_library(hs_treesitter STATIC
  ${treesitter_SOURCE_DIR}/lib/src/lib.c
  ${ts_python_SOURCE_DIR}/src/parser.c
  ${ts_python_SOURCE_DIR}/src/scanner.c)
target_include_directories(hs_treesitter
  PUBLIC ${treesitter_SOURCE_DIR}/lib/include
  PRIVATE ${treesitter_SOURCE_DIR}/lib/src ${ts_python_SOURCE_DIR}/src)
if(NOT MSVC)
  target_compile_options(hs_treesitter PRIVATE -w)
endif()

# nlohmann/json (header only)
add_library(hs_json INTERFACE)
target_include_directories(hs_json INTERFACE ${nljson_SOURCE_DIR}/include)
