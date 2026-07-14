#!/usr/bin/env bash
# Cross-platform build/test helper for hft_core.
#
# Develop on macOS, performance-test on Linux. This script picks sane defaults
# for each platform so you type one command instead of remembering flags.
#
# Usage:
#   core/build.sh              # release build + run tests (default)
#   core/build.sh dev          # portable release (HFT_NATIVE=OFF) + tests
#   core/build.sh debug        # ASan/UBSan debug build + tests
#   core/build.sh bench        # release build with benchmarks, then list them
#   core/build.sh perf         # Linux-only: native+LTO release for real perf runs
#   core/build.sh clean        # remove build dirs
set -euo pipefail

cd "$(dirname "$0")/.."   # repo root
MODE="${1:-release}"
OS="$(uname -s)"

configure() {  # <build_dir> <extra cmake args...>
  local dir="$1"; shift
  # Note: "${arr[@]}" on an empty array trips `set -u` under bash 3.2 (macOS),
  # so build the generator flags as plain args instead.
  if command -v ninja >/dev/null 2>&1; then
    cmake -S core -B "$dir" -G Ninja "$@"
  else
    cmake -S core -B "$dir" "$@"
  fi
}

case "$MODE" in
  release)
    configure core/build -DCMAKE_BUILD_TYPE=Release -DHFT_BUILD_TOOLS=ON
    cmake --build core/build -j
    ctest --test-dir core/build --output-on-failure
    ;;
  dev)
    # Portable: won't tune for this CPU. Matches CI. Numbers here are NOT perf.
    configure core/build -DCMAKE_BUILD_TYPE=Release -DHFT_NATIVE=OFF -DHFT_BUILD_TOOLS=ON
    cmake --build core/build -j
    ctest --test-dir core/build --output-on-failure
    ;;
  debug)
    configure core/build-debug -DCMAKE_BUILD_TYPE=Debug -DHFT_NATIVE=OFF
    cmake --build core/build-debug -j
    ctest --test-dir core/build-debug --output-on-failure
    ;;
  bench)
    configure core/build -DCMAKE_BUILD_TYPE=Release -DHFT_BUILD_BENCH=ON
    cmake --build core/build -j
    echo "--- benchmarks built (run on Linux, pinned) ---"
    find core/build -maxdepth 1 -type f -name 'bench_*' -perm -u+x
    ;;
  perf)
    if [ "$OS" != "Linux" ]; then
      echo "perf mode is Linux-only (macOS can't pin cores / disable freq scaling)." >&2
      echo "Use 'bench' to just compile the benchmarks on your Mac." >&2
      exit 1
    fi
    configure core/build-perf -DCMAKE_BUILD_TYPE=Release \
      -DHFT_NATIVE=ON -DHFT_LTO=ON -DHFT_BUILD_BENCH=ON
    cmake --build core/build-perf -j
    echo "--- perf binaries in core/build-perf. Pin a core, e.g.:"
    echo "    taskset -c 2 ./core/build-perf/bench_clock"
    ;;
  clean)
    rm -rf core/build core/build-debug core/build-perf core/build-asan
    echo "cleaned build dirs"
    ;;
  *)
    echo "unknown mode: $MODE (use: release|dev|debug|bench|perf|clean)" >&2
    exit 1
    ;;
esac
