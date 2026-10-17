# PROTEKT — implementation plan

> Working document. It is the authority Claude Code / Haiku implement against.
> Anything not decided here is not decided — ask before inventing.

**Goal:** a single-player, first-person hover-tank game in the spirit of Battlezone /
Battlezone II: *you* drive one vehicle, command base units and turrets, build from a
resource you physically harvest, against a scripted or AI opponent — on a world that is
reproducible from a seed.

**Non-goal (v1):** multiplayer, networking, mod support, asset pipeline, audio.

---

## 1. Decisions already made (Harri, 2026-10-16)

| # | Decision |
|---|----------|
| D1 | Solo only. Scripted scenarios and AI opponents; no networking. |
| D2 | **Same seed → same world, bit-for-bit.** This is the primary engineering requirement, not a feature. |
| D3 | AI opponents are an *enhancement*, never a dependency: the game must be complete and testable with zero AI modules loaded. |
| D4 | Keyboard + mouse. No gamepad, no touch. |
| D5 | Unlicense, on GitHub (`HarryPehkonen/PROTEKT`). |
| D6 | MVP first, one reviewable step at a time — Harri reviews and redirects after each milestone. |
| D7 | C++ (Core Guidelines, `CODING_STANDARDS.md`), self-hosted gate (`tools/ci.sh`), no external CI. |
| D8 | Claude Code writes most of it. Haiku for implementation, a stronger model for planning/architecture. Cautious rollout, measured (§6). |

---

## 2. The one architecture decision that pays for itself

**The human commander and the AI emit the same orders through the same interface.**

```
             input (keyboard/mouse)          AI commander (optional module)
                      │                              │
                      └──────────┬───────────────────┘
                                 ▼
                        OrderQueue (data, no behaviour)
                                 │
                                 ▼
      worldgen ──▶ Sim (fixed-step) ──▶ state ──▶ Renderer (behind a thin interface)
       (pure)      (pure-ish)            │
                                         └──▶ text/ASCII observer (tests, replay, AI training)
```

Consequences, which are the reason for this shape:

- the sim runs **headless** — tests, replays and AI tuning need no GPU and no window;
- "AI as enhancement" (D3) is literal: an `ICommander` implementation is swapped in or out;
- the AI is testable the same way the game is, because it produces plain data;
- the renderer cannot leak into game logic, which is what makes the backend choice in §5 deferrable.

---

## 3. Determinism rules (binding)

`world = f(seed, chunk_coord)` — a **pure function**. The world is never stored; it is
regenerated on demand from the seed. Saving a game is saving the seed plus the order log.

1. **No floating point in worldgen.** Integer / fixed-point only. `-ffast-math`, FMA
   contraction, `sin`/`cos`/`pow` from different libms, x87 vs SSE — all of them break
   bit-exactness across compilers, optimisation levels and machines. Integers do not.
2. **No `std::` RNG, no `std::hash`, no `std::random_device`.** `std::uniform_int_distribution`
   is not portable across implementations. Use the project's own SplitMix64/PCG (§4).
3. **No unseeded randomness, no clock, no locale, no iteration-order dependence.** One
   seeded stream per subsystem (`world`, `sim`, `ai`) so a change in one cannot shift another.
4. **Fixed timestep.** The sim advances in whole ticks via an accumulator; frame rate never
   changes the simulation.
5. **The oracle is a hash.** `world_hash(seed)` and `sim_state_hash(replay)` are asserted
   values in the test suite. A change that alters them is either a bug or a deliberate
   format change that updates the golden value in the same commit.

Determinism here is testable, fuzzable and cheap to state — which is exactly what makes it
the right first milestone rather than a nice-to-have.

---

## 4. Layout

```
include/protekt/    public headers, self-contained
src/                implementation
src/world/          worldgen (pure, integer)
src/sim/            tick, entities, orders, replay
src/ai/             ICommander implementations (optional, replaceable)
src/render/         renderer interface + backends (nothing above may include these)
src/app/            main(), input, glue
tests/              unit tests (ctest)
fuzz/               libFuzzer targets + seed corpus
tools/ci.sh         the gate (from AI-DEV-STARTER), .githooks/ run it
docs/               this plan, design notes
```

---

## 5. Milestones — each one is a review gate for Harri

Ranked by *risk reduction per unit of work*, not by visibility. Note that the flashy part
(rendering) is deliberately third: it is the part with the most known-knowns.

### M0 — Skeleton and gate ✅ (this commit)
Repo, CMake, `tools/ci.sh` green from the first commit, `CODING_STANDARDS.md`, this plan.
**You can review:** run `./tools/ci.sh`.

### M1 — Deterministic world, headless
`protekt::world::generate(seed, chunk)` → terrain height, ground type, spawn candidates.
Integer hash noise (value noise on a fixed-point lattice). Delivered with:
- known-answer tests (golden hashes for a handful of seeds);
- a fuzz target taking an arbitrary `u64` seed — invariant: generation always yields a
  valid world, spawn points are always reachable, no assertions fire;
- a CLI: `protekt-map --seed 42 --size 64` that dumps the world as ASCII.

**You can review:** look at the ASCII map for a seed, and see the same seed give the same map
twice, on a different machine or optimisation level. This is the milestone where the seed
requirement is either true or exposed.

### M2 — Headless sim: units, orders, replay
Fixed-step tick; a tank, a turret, a factory; an order queue carrying `MoveTo`, `GuardAt`,
`Patrol`, `Attack`, `Stop`; steering (no pathfinding yet — a coarse nav grid only if the
terrain demands it). Split in two because the first is arithmetic and the second is I/O:

- **M2a — the sim core (done).** Q16.16 fixed point, units, orders, the tick loop,
  terrain-aware movement, combat. No floating point and no randomness anywhere in the sim.
- **M2b — the commander and the outside world (next).** The replay text format, `ICommander`
  with one scripted implementation, the `protekt-sim` CLI, and the first byte-level libFuzzer
  target on the replay parser.

**Deliberate scope cut:** the harvester and the `Build` order moved to M4, where the resource
loop lives — a build order with no resources to spend cannot be tested in any interesting way.
M2 is Tank/Turret/Factory with move, guard, patrol, attack and stop: enough for a real
skirmish, and nothing that needs an economy.
**You can review:** watch a scripted skirmish play out as text, and see a replay reproduce a
state hash exactly.

### M3 — First playable frame 🎮
The renderer. One map, your hover tank, cockpit camera, flat-shaded primitives, a few
buildings, the commander cursor.
**Blocked on one decision (see §7): which machine plays it.** Everything else about this
milestone is decided.

### M4 — Commander layer
Group selection, order issuing with the keyboard/mouse, build menu, the resource loop
(harvest → build → fight), turret placement and guard postures.

### M5 — AI opponent
A utility/behaviour-based `ICommander` (perception → decision → orders, on a slower tick than
the sim). Then, optionally, the *offline* AI enhancement: a tuning loop that searches unit
parameters and build orders against the headless sim harness. Never an LLM in the tick loop —
latency, cost and nondeterminism all forbid it.

---

## 6. How the work is split (Claude Code, Haiku, Opus, Hermes)

**Haiku 4.5 is genuinely capable** — roughly 90% of Sonnet 4.5 on agentic coding evaluations
at a fraction of the cost and twice the speed. What it has less of is headroom on
long-horizon, multi-file work, which is precisely what a game engine is.

**The ladder (Harri, 2026-10-16): Haiku first; Sonnet when Haiku is too small; Opus or Fable
only when neither will do — he considers them overkill for this work.** So:
- **The planner (Opus, or Harri) writes:** this plan, the interfaces/headers, the CMake wiring, the tests
  that pin behaviour, and anything touching the AI architecture.
- **Haiku implements:** one header + its tests, to green. Mechanical additions (a new unit
  type from a table, another terrain layer, a CLI flag).
- **Haiku never edits:** interfaces, `CMakeLists.txt`, this document, or the gate. Its known
  failure mode is making a build pass by weakening a test or silently changing an interface.
- **TDD is mandatory** in every brief: failing test first, watch it red, implement, watch it
  green. The gate is the arbiter, never the agent's self-report.
- **Measure the split rather than trusting it:** M1 is the bake-off. Same spec + same tests to
  Haiku and to a stronger model; count round-trips to green and cost. If Haiku is not cheaper
  per merged milestone, use it for mechanical edits only.
- Mechanics: `HOME=/home/harri claude ...` (a sandboxed HOME breaks auth);
  `--output-format json` for `total_cost_usd`; put `Bash(clang-format *)`, `Bash(cmake *)`,
  `Bash(ctest *)` in `--allowedTools` or the run stalls at the formatter.

---

## 7. Open decision (blocks M3 only)

**Which machine plays the game?** Measured on the Debian box (`hermes`, MacBookPro9,2,
Intel HD 4000 / Ivy Bridge):

```
physical Vulkan devices found: 1
  [0] 'llvmpipe (LLVM 19.1.7, 256 bits)'   type=4 (CPU)
```

The only Vulkan device is the **software** rasteriser. Mesa's ANV supports Gen8+ and `hasvk`
Gen7.5; Ivy Bridge is Gen7, so the hardware driver reports no device. OpenGL is hardware
accelerated there (`crocus_dri.so`, GL 4.2-class). So on this machine Vulkan is possible but
not playable, and the options are:

- **A — OpenGL 4.2 first, Vulkan behind the same renderer interface later.** Playable now,
  and the interface is thin (indexed meshes, a few shaders), so little is thrown away.
- **B — Vulkan only**, and play it on a machine that has a Vulkan-capable GPU.
- **C — Renderer interface with both backends from the start.** Most work; do it only if
  both machines matter.

Nothing above M3 changes either way, which is why M1 is not blocked by this.

---

## 8. What this plan deliberately does NOT do

No ECS framework, no scripting language, no networking, no save-game serialisation beyond
seed + order log, no asset importer, no audio, no shadow/lighting system beyond flat shading,
no pathfinding beyond steering until terrain proves it necessary, no Vulkan abstraction layer
beyond the renderer interface. Each of these is a real answer to a question nobody has asked
yet, and each is cheap to add when it is asked.

---

## 9. Name

**PROTEKT** — spelled the way a machine spells it. A Battlezone-era shout of a title with the
kind of off-by-one letter that only a model makes, which is the joke: the game's opponents are
AI, the seed *is* the world, and the name is what an AI calls "protect". Repo `PROTEKT`,
public, Unlicense. (The working title was "Planet Defense"; it collided with a pile of
JavaScript canvas tutorials.)
