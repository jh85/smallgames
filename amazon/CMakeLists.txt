cmake_minimum_required(VERSION 3.18)
project(amazons LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -march=native")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -fsanitize=address")

# ---- Core library ----
add_library(amazons_core STATIC
  src/board.cc
  src/zdd.cc
  src/eval.cc
  src/solver.cc
  src/bruteforce.cc
)
target_include_directories(amazons_core PUBLIC src)

# ---- CLI ----
add_executable(amazons src/main.cc)
target_link_libraries(amazons PRIVATE amazons_core)

# ---- Layer-size measurement tool (5x5 WDL feasibility) ----
add_executable(layercount tools/layercount.cc)
target_link_libraries(layercount PRIVATE amazons_core pthread)

# ---- External-memory WDL table builder (5x5 retrograde) ----
add_executable(wdlretro tools/wdlretro.cc)
target_link_libraries(wdlretro PRIVATE amazons_core pthread)

# ---- Exhaustive consistency checker for wdlretro tables ----
add_executable(wdlcheck tools/wdlcheck.cc)
target_link_libraries(wdlcheck PRIVATE amazons_core)

# ---- Tests ----
enable_testing()
foreach(t IN ITEMS test_board test_arith test_zdd test_solver)
  add_executable(${t} test/${t}.cc)
  target_link_libraries(${t} PRIVATE amazons_core)
  add_test(NAME ${t} COMMAND ${t})
endforeach()
