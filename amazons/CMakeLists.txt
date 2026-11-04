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

# ---- Symbolic (DFA) solver and dense full-envelope tables ----
# Deliberately independent of amazons_core, so they can check it.
add_executable(amzdfa dfa/amzdfa.cc)
target_link_libraries(amzdfa PRIVATE pthread)
add_executable(dense dfa/dense.cc)
target_link_libraries(dense PRIVATE pthread)
find_library(ZSTD_LIBRARY zstd)
if(ZSTD_LIBRARY)
  add_executable(blockzstd dfa/blockzstd.cc)
  target_link_libraries(blockzstd PRIVATE ${ZSTD_LIBRARY} pthread)
endif()

# The bit-parallel solver needs AVX-512 with VBMI and GFNI, and libzstd.
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-march=native")
check_cxx_source_compiles("
#include <immintrin.h>
int main() {
  __m512i a = _mm512_setzero_si512();
  a = _mm512_permutexvar_epi8(a, a);
  a = _mm512_gf2p8affine_epi64_epi8(a, a, 0);
  return _mm512_test_epi64_mask(a, a) != 0;
}" AMAZONS_HAVE_AVX512_VBMI_GFNI)
unset(CMAKE_REQUIRED_FLAGS)
if(ZSTD_LIBRARY AND AMAZONS_HAVE_AVX512_VBMI_GFNI)
  add_executable(slabsolve dfa/slabsolve.cc)
  target_compile_options(slabsolve PRIVATE -march=native)
  target_link_libraries(slabsolve PRIVATE ${ZSTD_LIBRARY} pthread)
endif()

# ---- Tests ----
enable_testing()
foreach(t IN ITEMS test_board test_arith test_zdd test_solver)
  add_executable(${t} test/${t}.cc)
  target_link_libraries(${t} PRIVATE amazons_core)
  add_test(NAME ${t} COMMAND ${t})
endforeach()

find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  if(TARGET slabsolve)
    add_test(NAME dfa_crosscheck
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/dfa/crosscheck.py
                     $<TARGET_FILE:amzdfa> $<TARGET_FILE:dense> $<TARGET_FILE:slabsolve>)
    add_test(NAME slabsolve_selftest COMMAND slabsolve selftest)
  else()
    add_test(NAME dfa_crosscheck
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/dfa/crosscheck.py
                     $<TARGET_FILE:amzdfa> $<TARGET_FILE:dense>)
  endif()
endif()
