cmake_minimum_required(VERSION 3.20)
project(hashstash VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(cmake/deps.cmake)

# ---- core library -----------------------------------------------------------
file(GLOB_RECURSE HS_LIB_SOURCES CONFIGURE_DEPENDS
  src/core/*.cpp src/lang/*.cpp src/ops/*.cpp src/mcp/*.cpp src/hub/*.cpp)
add_library(hashstash STATIC ${HS_LIB_SOURCES})
target_include_directories(hashstash PUBLIC src)
target_link_libraries(hashstash PUBLIC hs_sqlite hs_blake3 hs_treesitter hs_json hs_httplib)
target_compile_definitions(hashstash PUBLIC HS_VERSION="${PROJECT_VERSION}")
if(MSVC)
  target_compile_options(hashstash PRIVATE /W4 /utf-8)
else()
  target_compile_options(hashstash PRIVATE -Wall -Wextra)
endif()
if(WIN32)
  target_link_libraries(hashstash PUBLIC ws2_32)
endif()

# ---- programs ---------------------------------------------------------------
add_executable(hs src/cli/main.cpp)
target_link_libraries(hs PRIVATE hashstash)
if(MINGW)
  target_link_options(hs PRIVATE -static)
endif()

add_executable(hshub src/server/main.cpp)
target_link_libraries(hshub PRIVATE hashstash)
if(MINGW)
  target_link_options(hshub PRIVATE -static)
endif()

# ---- tests ------------------------------------------------------------------
enable_testing()
file(GLOB HS_TEST_SOURCES CONFIGURE_DEPENDS tests/*.cpp)
add_executable(hs_tests ${HS_TEST_SOURCES})
target_link_libraries(hs_tests PRIVATE hashstash)
target_compile_definitions(hs_tests PRIVATE HS_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
if(MINGW)
  target_link_options(hs_tests PRIVATE -static)
endif()
add_test(NAME unit COMMAND hs_tests)
