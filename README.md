# PROTEKT

PROTEKT — a single-player, first-person hover-tank game in the spirit of *Battlezone* and
*Battlezone II: Combat Commander*: you drive one vehicle, command base units and turrets,
build from a resource you physically harvest, and fight an opponent on a world that is
**reproducible from a seed**.

The seed is the point. The same seed must produce the same world, bit-for-bit, on any machine
and at any optimisation level — which is why world generation is a pure integer function and
never uses floating point.

## Status

Pre-alpha. **Scaffold only**: build wiring and the local gate. No game code yet. The plan is
in [`docs/PLAN.md`](docs/PLAN.md); milestone M1 is the first real code.

## Requirements

- CMake ≥ 3.20, a C++20 compiler (clang or gcc)
- GoogleTest (Debian: `libgtest-dev`)
- `clang-format`, `clang-tidy` for the full gate

## Build and test

```bash
cmake -S . -B build
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

## The gate

No external CI. One script is the single definition of "green", and the git hooks call it:

```bash
./tools/ci.sh              # every stage
./tools/ci.sh fast         # what a commit runs: format build tests
./tools/ci.sh --list       # the stages
git config core.hooksPath .githooks   # once per clone: enables pre-commit/pre-push
```

Per-machine settings go in `.ci.env` (gitignored; see `.ci.env.example`).

## Layout

```
include/protekt/  public headers
src/              implementation (world/ sim/ ai/ render/ app/)
tests/            unit tests
fuzz/             libFuzzer targets
tools/ci.sh       the gate
docs/PLAN.md      the plan
```

## License

[The Unlicense](LICENSE) — public domain.
