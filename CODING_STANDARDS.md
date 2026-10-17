# Coding standards — PROTEKT

Binding for every human and every agent that touches this repository. In the spirit of the
**C++ Core Guidelines** (Type, Bounds and Lifetime profiles). Exceptions are *allowed* for
error handling — the "no exceptions" jet standards (JSF/MISRA) are rejected deliberately.

## Rules

1. **C++20.** No compiler extensions; `-Wall -Wextra -Wpedantic -Werror` on every target.
   A warning is a build failure, and the gate counts `warning:` lines even in targets
   `-Werror` does not cover.
2. **No raw owning pointers, no `new`/`delete`, no `reinterpret_cast`, no C-style casts.**
   `gsl::not_null`, `std::span`, `std::string_view`, `std::unique_ptr` where ownership is real.
3. **TDD.** A behaviour change starts as a failing test that is watched go red. Bug fixes too.
4. **Gates before "done":** `./tools/ci.sh fast` while working, the full tier before a push
   (format, build, tests, release, version, asan, tsan, tidy, pristine). Never weaken a
   check to make it pass; fix the finding.
5. **Fuzz targets are clients.** Any new input-handling entry point lands with a libFuzzer
   target and a seed corpus, and the target must keep building afterwards.
6. **Measure before claiming.** Performance and behaviour claims come with a measurement, not
   an intuition.

## Determinism rules (this repo's extra, non-negotiable section)

A world is `f(seed, chunk_coord)` — a pure function. These rules protect that:

7. **No floating point in world generation.** Integer or fixed-point only. FMA contraction,
   `-ffast-math` and libm differences make floating-point results non-portable, and a seed
   that gives a different world on a different machine is a bug.
8. **No `std::random_device`, no `std::uniform_*_distribution`, no `std::hash`.**
   `std::uniform_int_distribution`'s mapping is implementation-defined. Use
   `protekt::rng` (SplitMix64 / PCG), which is specified here and pinned by tests.
9. **No unseeded randomness, no wall clock, no locale, no iteration-order dependence.**
   One seeded stream per subsystem (`world`, `sim`, `ai`) so that consumption in one cannot
   shift the values another sees.
10. **Fixed timestep.** The simulation advances in whole ticks through an accumulator. The
    frame rate must never change the outcome of a tick.
11. **Determinism is asserted, not assumed.** `world_hash(seed)` and
    `sim_state_hash(replay)` have golden values in the test suite; changing one is a
    deliberate act that updates the value in the same commit.

## Architecture rule

12. **Game logic may not include a rendering header.** The renderer sits behind an interface;
    the sim, world and AI are headless and must stay testable with no GPU and no window.
    The human commander and the AI produce the same `Order` values through the same
    `OrderQueue` — that is what makes the AI an enhancement rather than a dependency.
