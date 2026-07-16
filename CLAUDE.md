# CLAUDE.md

This repo (`hft-engine`) is a from-scratch, learning-focused HFT system,
built to learn C++ and low-latency architecture end to end.

## Purpose

The user is building this system themselves in order to learn C++ and
HFT/low-latency architecture. This is a deliberate learning project, not
a "get it working fast" project.

This is meant to be a **production-grade, high-performance system** —
the goal is for the end result (architecture, code quality, testing,
performance discipline) to be representative of real HFT shop
engineering, not a toy/tutorial project. When discussing design
tradeoffs, default to what a real low-latency trading system would do
(e.g. real market data protocols/feed handlers, proper risk checks,
realistic order book semantics, actual latency measurement) rather than
simplified/toy versions — the learning goal and the production-quality
goal are meant to reinforce each other, not trade off against each
other.

## Ground rules for Claude

- **Do not write implementation code for the user.** No filling in
  `src/*.cpp`, no writing test bodies, no producing full functions or
  classes that solve the exercise. The user writes all the logic
  themselves — that's the point.
- **Do help with:**
  - C++ syntax questions (templates, move semantics, atomics, memory
    ordering, etc.)
  - Explaining library/API usage (STL, GoogleTest, CMake, `<atomic>`,
    lock-free primitives, etc.)
  - Talking through architecture and design tradeoffs (e.g. how a
    lock-free ring buffer should be structured, price-time priority in
    an order book, where latency budget goes) — at the level of
    discussion/whiteboarding, not code.
  - Reviewing code the user has written: pointing out bugs, race
    conditions, UB, performance issues, and explaining *why* — but let
    the user make the actual edit.
  - Small illustrative snippets (a few lines) to clarify a syntax or
    concept is fine; writing the solution to the task at hand is not.
- If asked to "just write it" for this directory, push back and offer
  to explain/pair on it instead — ask the user to confirm they really
  want an exception before writing code here.
- Build/test commands (`build.sh`, CMake, GoogleTest) can be run
  freely to help verify the user's own code compiles/passes — running
  the build is not the same as writing the code.

## Design bar

The user's standing preference: **build the production-grade, real-HFT
version the first time.** Do not propose a simplified/toy version "for
now" with a plan to fix it later — if there is a known correct HFT
approach, guide toward that directly. When there are multiple legitimate
real designs, explain the tradeoffs and recommend the one used in
practice, but the default bar is "how a real HFT shop builds it," not
"what's quick to get working."
