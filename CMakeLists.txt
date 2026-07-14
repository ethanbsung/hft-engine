cmake_minimum_required(VERSION 3.20)
project(hft_core LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Toolchain / standard
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # for clangd / editor tooling

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# Platform detection
# ---------------------------------------------------------------------------
# This project is developed on macOS (Apple Silicon) and performance-tested on
# Linux (x86_64). We keep the SAME portable code building on both, and only the
# optimization flags differ.
message(STATUS "hft_core: ${CMAKE_SYSTEM_NAME} / ${CMAKE_SYSTEM_PROCESSOR}")

# ---------------------------------------------------------------------------
# Optimization flags
# ---------------------------------------------------------------------------
# HFT_NATIVE controls -march=native. It tunes the binary for the CPU doing the
# build, which is what you want for real perf runs (build == run machine), but
# it makes a binary that MAY NOT run on a different CPU and makes cross-machine
# numbers incomparable. So:
#   - ON  by default: right for both "develop on my Mac" and "perf-test on my
#     Linux box", because in both cases you run where you built.
#   - Turn it OFF (-DHFT_NATIVE=OFF) for CI or if you ever ship a binary to a
#     different machine than the one that compiled it.
option(HFT_NATIVE "Tune for the building CPU (-march=native)" ON)

# LTO gives a real, measurable win for a latency-sensitive release binary but
# slows the build. On by default for Release; harmless to leave on.
option(HFT_LTO "Link-time optimization for Release builds" ON)

set(HFT_ARCH_FLAGS "")
if(HFT_NATIVE)
  include(CheckCXXCompilerFlag)
  check_cxx_compiler_flag("-march=native" HFT_HAS_MARCH_NATIVE)
  if(HFT_HAS_MARCH_NATIVE)
    list(APPEND HFT_ARCH_FLAGS "-march=native")
  else()
    # Apple clang on Apple Silicon historically preferred -mcpu=apple-m1 etc.;
    # modern toolchains accept -march=native, but fall back gracefully.
    check_cxx_compiler_flag("-mcpu=native" HFT_HAS_MCPU_NATIVE)
    if(HFT_HAS_MCPU_NATIVE)
      list(APPEND HFT_ARCH_FLAGS "-mcpu=native")
    endif()
  endif()
endif()

# Release: fast. Debug: sanitizers. These apply to every target below.
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG   "-O0 -g -fsanitize=address,undefined -fno-omit-frame-pointer")
set(CMAKE_EXE_LINKER_FLAGS_DEBUG "-fsanitize=address,undefined")

add_compile_options(-Wall -Wextra -Wpedantic ${HFT_ARCH_FLAGS})

if(HFT_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT HFT_IPO_OK OUTPUT HFT_IPO_MSG)
  if(HFT_IPO_OK)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
  else()
    message(STATUS "hft_core: LTO not supported (${HFT_IPO_MSG})")
  endif()
endif()

# ---------------------------------------------------------------------------
# The core library (engine internals under src/)
# ---------------------------------------------------------------------------
# No JSON dependency: the feed is Nasdaq ITCH 5.0, a fixed-layout binary
# protocol. Prices arrive as uint32 with 4 implied decimals, so there is no
# string->number conversion anywhere on the hot path. (nlohmann/json was
# removed with the Coinbase WebSocket handler -- see dead/README.md.)

# GLOB is convenient while the file set is churning early on. Re-run cmake
# after adding a .cpp (CONFIGURE_DEPENDS makes Ninja/Make re-glob automatically).
file(GLOB HFT_CORE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(hft_core ${HFT_CORE_SOURCES})
target_include_directories(hft_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

# Threads: needed for pthread_setaffinity_np on Linux; harmless on macOS.
find_package(Threads REQUIRED)
target_link_libraries(hft_core PUBLIC Threads::Threads)

# ---------------------------------------------------------------------------
# Main executable
# ---------------------------------------------------------------------------
add_executable(hft_system "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp")
target_link_libraries(hft_system PRIVATE hft_core)

# ---------------------------------------------------------------------------
# Tests (GoogleTest)
# ---------------------------------------------------------------------------
# Locate GoogleTest robustly across platforms:
#   - macOS: Homebrew (find_package works once CMAKE_PREFIX_PATH sees the cellar,
#     which brew's cmake shim handles; we also nudge it below).
#   - Linux: distro package (libgtest-dev) or a vendored copy.
# If it isn't found we simply skip the test target rather than fail the build.
enable_testing()

# Help find_package locate Homebrew's googletest on macOS without hardcoding.
if(APPLE)
  execute_process(
    COMMAND brew --prefix googletest
    OUTPUT_VARIABLE HFT_BREW_GTEST
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  if(HFT_BREW_GTEST)
    list(APPEND CMAKE_PREFIX_PATH "${HFT_BREW_GTEST}")
  endif()
endif()

find_package(GTest QUIET)

if(GTest_FOUND)
  file(GLOB HFT_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")
  if(HFT_TEST_SOURCES)
    add_executable(hft_tests ${HFT_TEST_SOURCES})
    target_link_libraries(hft_tests PRIVATE hft_core GTest::gtest GTest::gtest_main)
    include(GoogleTest)
    gtest_discover_tests(hft_tests)
  endif()
else()
  if(APPLE)
    message(STATUS "GoogleTest not found; skipping tests. brew install googletest")
  else()
    message(STATUS "GoogleTest not found; skipping tests. sudo apt install libgtest-dev")
  endif()
endif()

# ---------------------------------------------------------------------------
# Benchmarks (bench/*.cpp) — one executable per file, opt-in.
# ---------------------------------------------------------------------------
# Benchmarks only mean something on the Linux perf box, but they must COMPILE on
# the Mac so you don't discover breakage 7 weeks later. Build them with
# -DHFT_BUILD_BENCH=ON.
option(HFT_BUILD_BENCH "Build microbenchmarks under bench/" OFF)
if(HFT_BUILD_BENCH)
  file(GLOB HFT_BENCH_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/bench/*.cpp")
  foreach(bench_src ${HFT_BENCH_SOURCES})
    get_filename_component(bench_name "${bench_src}" NAME_WE)
    add_executable(${bench_name} "${bench_src}")
    target_link_libraries(${bench_name} PRIVATE hft_core)
  endforeach()
endif()

# ---------------------------------------------------------------------------
# Tools (tools/*.cpp) — standalone utilities, not part of hft_core.
# ---------------------------------------------------------------------------
# One executable per .cpp, same pattern as bench/. Currently empty: the
# Coinbase capture tool (and its Boost + OpenSSL dependency) retired with the
# WebSocket feed -- see dead/README.md.
#
# The feed simulator (BinaryFILE -> MoldUDP64 -> UDP, with injectable gaps)
# belongs here when written: it is a separate PROCESS, not a thread of the
# engine, which is what makes the socket read real. Plain UDP sockets, no
# Boost, no TLS.
option(HFT_BUILD_TOOLS "Build standalone tools under tools/" OFF)
if(HFT_BUILD_TOOLS)
  file(GLOB HFT_TOOL_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/*.cpp")
  foreach(tool_src ${HFT_TOOL_SOURCES})
    get_filename_component(tool_name "${tool_src}" NAME_WE)
    add_executable(${tool_name} "${tool_src}")
    target_link_libraries(${tool_name} PRIVATE hft_core)
  endforeach()
endif()
