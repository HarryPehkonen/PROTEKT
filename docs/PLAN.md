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

**Where it is now.** A deterministic world from a seed; a fixed-tick simulation with units, orders and
replay; a Vulkan client that draws it, headless or in a window; a command layer where the human and the
AI issue the same orders through the same interface; a utility AI that advances, engages and withdraws;
a trigger that chooses a target; and units that go around the ground rather than into it. 278 tests.

**What is next, in order.**

1. **Finish the feel slice: crosshair, hit feedback, sound.** The gun already fires and the trigger
   already chooses, but none of it is *visible*: nothing is drawn but the world, and nothing makes a
   sound. The crosshair, a marker on the unit being engaged and an edge flash when damage lands all fit
   inside the frozen renderer contract as world-space triangles, because the camera's view is the only
   thing that decides where they appear. Fired/killed events come from the existing observer, which
   infers them by diffing simulation state between ticks — no new sim machinery. Sound is SDL3's, which
   is already a dependency, synthesised rather than sampled so there is nothing to license.
2. **Economy and objectives: the unpaid half of M4.** Resources, a harvester, building, and a way for a
   match to end. This is what makes an A\* worth building and what gives turret placement a point.
3. **Deterministic A\* (navigation option 2)**, as its own milestone, once units have somewhere to go
   that matters.
4. **Rendering depth and sound**: textures, lighting, unit shapes. Today the world is flat colours.
5. **Strategic controllers** (below), the slow layer that decides what to defend.


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
Split in two, because the GPU plumbing and the fun are different risks.

**M3a (done) — the renderer, headless.** Vulkan behind `render::Renderer`
(`include/protekt/render/renderer.hpp` contains no Vulkan type at all): instance and device
selection, an offscreen colour+depth target, validation layers counted and asserted zero, shaders
compiled to SPIR-V at build time, readback to RGBA8, and `protekt-render` writing a PPM. 19 tests,
and they run on the gate box where the only device is llvmpipe. Vulkan is optional at configure
time: without a loader the renderer and its tests are absent and configure says so.

**M3b (done) — the window.** One game loop, two presenters: a swapchain window on a machine with a
display, an offscreen buffer on the gate box. The loop, the world view and the player commander are
all tested headless, so M3b added 24 tests (134 across 21 suites) and every one of them runs in the
gate. SDL3 for the window and the keyboard, nothing else.

*The player commands through the same interface the AI uses.* `game::PlayerCommander` reads an
`Observation` and emits `Order`s, exactly like `ScriptedCommander`; the throttle and the steering
are turned into a waypoint ahead of the tank, and the simulation moves it. Shooting stays the
simulation's automatic business until M4.

**How the window presents, and why.** The window renders through `render::create()` — the same
offscreen renderer the suite tests — and copies the result into the swapchain image. That costs a
readback and a re-upload per frame and buys three things: what is on screen is provably what the
tests check (a captured window frame and a captured headless frame are the same bytes by
construction), there is exactly one place in the project that draws geometry, and every failure path
is on the window's side of the seam. Rendering straight into the swapchain images with their own
pipeline is a later optimisation, not a correctness fix.

Verified on the play machine (GTX 1060 Max-Q, driver 580.178.04): the window opens, 300 frames
present at 1280x720, the capture is **byte-identical** to the headless capture of the same seed and
tick count, the sky is at the top, and SIGTERM closes it cleanly. The gate box has no display, so
the window itself is verified by hand there and the loop is verified by tests here.

**Two machine facts that cost real time**, both worth knowing before the next Vulkan machine:

1. Fedora and its rebuilds ship the loader as `libvulkan.so.1` with no `libvulkan.so` development
   symlink and no `vulkan.pc`, so `find_package(Vulkan)` fails on a machine that runs Vulkan
   perfectly well. CMakeLists now falls back to the versioned loader and says so at configure time.
2. The loader reports an ICD it cannot load and checks the extension names in a `VkDeviceCreateInfo`
   it is about to pass on — where an implicit layer (Mesa's `VK_LAYER_MESA_device_select`) may have
   added an extension this code never asked for. `validation_errors()` therefore counts validation
   findings (those with a `VUID-` id) and prints the rest: a broken driver stack on somebody's
   machine must not fail a test that is about this code. Measured on the play machine: 27 messages
   arrive at the messenger, none of them ours.

**Known gap, again:** ASan needs one suppression, for a leak inside libdrm reached through
`SDL_Init(SDL_INIT_VIDEO)`. `.ci/lsan.supp` scopes it to the function, not the library, so a leak
anywhere else still fails the gate.

**Determinism boundary**, stated because it is easy to assume wrongly: the SIMULATION is
bit-identical everywhere; rasterisation is not. An exact image hash is only meaningful on one
device and driver, so the suite asserts structure on any device and byte equality only between two
renders of the same request. There are no checked-in golden images.

**Known gap:** the Vulkan tests are skipped under ThreadSanitizer, because lavapipe races inside
itself and a driver cannot be instrumented. So the renderer's CPU-side code has ASan and UBSan
coverage but no TSan coverage. A suppression keyed to `libvulkan_lvp.so` was rejected on purpose:
it would hide the same reports while pretending to test.

### M4 — Commander layer
Split in two: the order interface first, the economy second.

**M4a (contract written, red) — the command layer.** `include/protekt/cmd/`:

* `Selection` — the player's command set. A SET, sorted ascending and unique, because everything
  downstream reads that order: a replay of human play is only exact if the order the player selected
  units in cannot reach the simulation. Bounded, so a drag-box cannot select the world, and it drops
  the dead so a player cannot command ghosts.
* `command.hpp` — the verbs: `move_to`, `guard_at`, `patrol`, `attack`, `stop`. Each a pure function
  from (roster, aim) to orders, one order per unit in ascending id order, and an impossible order is
  DROPPED rather than repaired: `attack` on `UnitId{}` is not a move, and a verb that silently
  becomes another verb is worse than one that refuses.
* `controller.hpp` — the standing controllers described below: `Policy` (`Hold`, `Deny`, `Focus`),
  `Controller` (a roster, a cadence, and orders through the same channel as the player), and
  `ControllerSet`, which enforces the invariant that matters — no unit is owned by two controllers,
  or two of them fight over one turret and the last order to arrive wins by accident.

26 new tests, red against stubs, with the 134 existing tests still green as a regression guard.

**M4b (later) — the build menu, the resource loop** (harvest → build → fight), turret placement
from the player's own hand, and click-to-attack.

**Known issue, measured, deliberately not fixed yet.** `scenario.cpp` places a unit on the first
passable tile, which guarantees ground to stand on but not a way out of it: at seed 42 the player's
tank spawns at tile (32,46) with **four crater tiles directly ahead**. The simulation is right to
refuse them and the tank turns normally, so the player is facing a wall rather than stuck -- but it
is a bad first impression and it should be a placement rule ("a clear run in at least one
direction"), not a lucky seed. It is deferred to M4b because changing spawn selection moves the
simulation's state and therefore every golden hash and recorded battle log in the suite; that
re-recording belongs in the milestone that reworks placement anyway.

**M4a, M4b and M4 done.** The command layer landed in three pieces, each with its own suite:

* `cmd::Selection` -- a bounded, sorted, unique set. Adding is idempotent.
* `cmd::` verbs -- `move_to` / `guard_at` / `patrol` / `attack` / `stop`. Pure: roster and aim in,
  orders out, one per unit in ascending id order. An impossible order is DROPPED, not repaired, and
  the caller gets nothing back so it can say why. A tile exists when it is inside `[0, kMapTiles)`.
* `game::pick` -- what a click means: the unit under the cursor (a box test, nearest first, ties to
  the lower id) and the ground tile behind it. Distances are SQUARED EUCLIDEAN, in whole tiles, and
  that is in the contract rather than in the implementation: a radius is a circle, and Manhattan
  distance is a diamond that moves where a unit stops on a diagonal.
* `game::Interaction` -- the player's hands: select, shift-toggle, cycle with Tab, clear with
  Escape, right click to move or attack. **The tank the player is in is never ordered by a click**;
  selecting it is allowed and ordering it is refused out loud, because that unit is driven from the
  keyboard every tick and an order would fight the player's own hands.
* `game::Session` -- the player's whole side of one frame, and the fix for a hole worth naming.

**The hole.** M3b's "playable" claim was too strong. The window opened, the simulation ran, and the
tank never moved, because nothing in `app/game.cpp` had ever called `PlayerCommander::set_input`:
throttle stayed zero, and the camera presented a still world. Measured before fixing -- 300 frames,
no input, hull tile (32,46) to (32,46). `Session` is now the one entry point from input to orders, so
the untestable part of the loop shrank to the pumping of the event queue, and everything between a
keystroke and the simulation is covered headless.

**And the second half of the same bug.** With keys wired, W still did nothing at seed 42: the tank
spawns facing four crater tiles and the simulation is right to refuse them. That was the spawn issue
recorded here as "deferred, not fixed"; it turned out to be a playability blocker rather than a bad
first impression. `game::navigate::clearest_heading` now chooses the spawn heading from the terrain
(64 directions, longest clear run, ties to the lowest heading). **This moves no golden values**,
because heading is presentation state, not simulation state -- which is exactly why it could be fixed
without re-recording a single hash.

**Deliberately not done:** mouse look while driving. Keys drive, the mouse selects and orders, and
what the mouse should do in motion is a question to answer after playing it, not before.

**Verified end to end:** driving moves the world (frame 30 differs from frame 300), an idle run
differs from a driving one, and on the laptop's GTX 1060 the windowed capture is byte-identical to
the headless one *while driving* -- the strongest available form of "what you see is what the suite
checks". 219 tests, 32 suites, on lavapipe and on the 1060.

### Strategic controllers (Harri's sketch, 2026-10-17)

The player places **controllers** on the map and gives each a standing intent -- "do not let them
through here" -- and the controller does the clicking: it posts turrets, and it calls targets
("concentrate fire on that one... now that one"). It thinks slowly: 0.5-2 Hz is ample, and is still
thirty times faster than a human.

This fits an interface that already exists. A controller is an `ICommander`: perception in,
`Order`s out, exactly like the scripted opponent and like `PlayerCommander`. Nothing in the
simulation changes.

What it needs that does not exist yet:

1. **A roster.** Today an order may name any unit (deliberately: a test of ours once forbade it and
   silently killed half the game). A controller must own a set of `UnitId`s, or two controllers
   will fight over the same turret. The player's command layer needs rosters anyway for group
   selection, so this is M4 work, and controllers inherit it.
2. **A policy vocabulary.** "Don't let them through" is a policy, not an order. The orders it
   compiles to already exist: `GuardAt` for posts, `Attack` for a called target -- explicit
   `Attack` outranks auto-engagement and makes the unit pursue, which is exactly what
   "concentrate fire" needs. So the vocabulary is small: `Hold{area}`, `Deny{path}`,
   `ConcentrateFire{priority}`, `Screen{unit}`.
3. **Terrain analysis for placement.** The interesting part, and the testable part: `world::sample`
   is a pure function, so "where are the passes through this ridge" is a deterministic, headless,
   unit-testable computation with no GPU and no tick loop involved.
4. **Determinism, non-negotiable.** A controller may use only simulation state and the tick number:
   no wall clock, no `rand()`, no floats in decisions. Otherwise a replay stops reproducing.
5. **Observability.** A controller that cannot say what it decided is a black box with a gun. Reuse
   the battle-log format (M2b) for a short controller log.

Deliberately NOT in scope: an LLM anywhere near the tick loop. Latency, cost and nondeterminism all
forbid it. An LLM may, at most, choose a policy's parameters offline or on demand -- the same
offline-tuning story as M5, and it must fall back to the rule-based path.

Testable claim to measure when this lands: with one `Deny` controller placed on a corridor, does the
scripted opponent get through? N seeds, headless, win/loss counted -- the same harness discipline as
everything else here.

### M4 — still owed, named so it is not silently dropped
**The economy was M4's second half in this plan and is NOT built.** There is no resource field, no
harvester, no `Build` order and no production; a factory is a 400 hp target and nothing more. It is
deliberately not being rushed in behind the commander layer: it changes the simulation's state (a
side's pool, a unit's cargo) and therefore every recorded hash, so it wants its own milestone with
its own re-recording rather than a pile of re-recorded goldens from an evening's work. The plan's
sequencing stands, and the item is open rather than done.

### M5 — AI opponent
A utility/behaviour-based `ICommander` (perception → decision → orders, on a slower tick than
the sim). Then, optionally, the *offline* AI enhancement: a tuning loop that searches unit
parameters and build orders against the headless sim harness. Never an LLM in the tick loop —
latency, cost and nondeterminism all forbid it.

**M5a — the utility commander.** `ai::UtilityCommander`, and the measurement that makes it worth
having. The scripted opponent attacks whatever is nearest from any distance at any health, so its
tanks cross the map one at a time and leave the base empty; the new one decides from what a unit can
see, how hurt it is, and what it is guarding.

Two rules shape it, and both are in the header as contracts rather than as comments:

* **Decisions are integers.** The AI needs a total order over a handful of options, not physical
  accuracy, and integer arithmetic makes every tie-break exact — two runs of one battle cannot
  disagree about which of two equidistant enemies dies first. No floats in a decision, at all.
* **Posture is a gate, not a tug of war.** A hurt tank withdraws; it does not weigh withdrawing
  against shooting and lose because an enemy is close. If the two competed on one scale, the retreat
  threshold in the tuning would not describe the behaviour — and a constant nobody can read off the
  outcome is a constant nobody can tune.

Utility is then used for what it is good at: choosing WHICH enemy (threat first, then distance, ties
to the lower id). The four postures — `Hold`, `Engage`, `Reposition`, `Retreat` — are exposed as
values, not as orders, because "why is that tank going home?" is answerable only if the reason is
something a person can read. `decisions()` exists for the same reason: a commander observable only
through its orders cannot be debugged.

**The honest claim to prove, and how.** "Better than scripted" is not a unit test. Before this counts
as done, the same scenario runs under both commanders across several seeds and the difference is
reported as a measured number — kills, losses, and how long the base survives — including the seeds
where the utility commander does NOT do better. A utility function nobody measured is a preference,
not an improvement.

**M5a — measured, and the number is not the one the milestone hoped for.** `./build/protekt-measure`
runs the arms below and prints them; 2400 ticks, ten seeds, `skirmish(seed)`.

| arm | player units lost | enemy units lost |
|---|---|---|
| placebo enemy (NO orders at all) | 18 | 21 |
| scripted enemy | 20 | 23 |
| utility enemy | 15 | 2 |
| utility as PLAYER vs scripted | 3 | 18 |
| utility against utility | 0 | 0 |

Three honest readings of that table, including the one that is bad news:

1. **The obvious metric is worthless.** "Loses fewer units" is won by a commander that does nothing at
   all: the placebo loses 21 and the scripted opponent loses 23. Only the ASYMMETRY survives that
   check, and by that measure the utility commander is genuinely better -- as the aggressor it takes
   18 units for 3 of its own, where the scripted opponent trades roughly even (23 for 20).
2. **The scripted opponent is barely distinguishable from silence.** 23 lost against the placebo's 21:
   its tanks walk at whatever is nearest from any distance and die arriving.
3. **Two utility commanders never fight.** Zero shots fired in 2400 ticks on all ten seeds, because
   each side holds at home and neither ever closes the distance out of its 14-tile engagement range.
   That is a degenerate equilibrium, not caution, and it means the commander cannot yet play the
   attacking role a solo game needs. **The missing posture is advance-to-contact**, and it is the next
   slice of M5 rather than a footnote.

Also visible in every arm: **no unit ever attacks a factory** -- 400 hp at the end of all fifty runs.
The base is currently unattackable in practice, so no match can be won or lost. That is the objective
and economy milestone's problem, and it is recorded here so it is not mistaken for balance.

**M5b — advance to contact, and the standoff fixed.** The defect M5a measured is now closed: one new
posture, `Intent::Advance`, plus one tuning number, `advance_percent{50}`. A tank with nothing in
range, whose side knows both bases, moves to a staging point halfway along the line from its own
factory to the enemy's -- halfway so that two commanders staging from opposite ends arrive at the same
tile and meet, and staging rather than attacking so it does not walk into the guns of the base it was
sent toward. The policy now reads as the sentence it is: **withdraw if hurt, engage what is in range,
otherwise go where the war is, and if nobody knows where that is, go home.**

Measured before and after, same tool, same ten seeds, 2400 ticks:

| arm | before M5b (P lost / E lost / seeds with contact) | after M5b |
|---|---|---|
| utility against utility | 0 / 0 / **0 of 10** | 3 / 5 / **8 of 10** |
| utility as PLAYER vs scripted | 3 / 18 / 7 of 10 | 7 / 22 / 8 of 10 |
| utility enemy vs scripted player | 15 / 2 / 7 of 10 | 16 / 12 / 8 of 10 |

Read honestly, that is not a clean improvement at every line. The enemy arm gets worse by the metric
of units NOT lost (2 -> 12) precisely because it now advances and fights instead of sitting at home;
what it buys is that it takes 16 of the player's units doing it, on eight seeds instead of seven. The
trade is deliberate and it is the whole point of the posture: **a commander that never leaves its own
half is not an opponent.** The aggressor asymmetry survives and grows (22 taken for 7 lost, against
the scripted opponent's 23 for 20).

The regression test for the standoff is an INTEGRATION test -- `UtilityBattle.TwoUtilityCommanders
ActuallyFightEachOther` runs a real 2400-tick battle and asserts that both sides fired and both sides
took losses -- because no unit test could see the defect: every unit was behaving exactly as its unit
tests said it should. Sabotaging the posture back out (`advance_percent = 0`) fails it, which is the
proof that the test is the reason it cannot come back.

### Navigation -- what a unit does when the ground is in the way (measured 2026-10-17)

A unit steers straight at its goal. When a step is refused it slides along each axis, and when the goal
direction AND both axes are blocked it takes a short **detour**: a remembered direction, held for up to
`kDetourTicks`, chosen from the eight neighbouring tiles with the ones nearest the wanted direction
first. `include/protekt/sim/steering.hpp` holds that decision, and it is the only place a step is
decided.

**The seam is built for a route planner.** `plan_step` answers one question -- "which way does this unit
go this tick?" -- from a ground handed to it as any type with `factor(tile_x, tile_y)`, so a test draws
a map and the sim hands it the world. A deterministic A* answers the same question from a waypoint list
and needs nothing else in the tick loop to change.

**What it does, and what it does not do.** A concave crater corner cannot hold a unit: at seed 42 a tank
ordered onto the enemy base crosses **94 tiles** to (137,158), which is **11.8 tiles** from the base and
inside its 12-tile weapon range, and stops there to fire. An unarmed base goes **400 -> 0 hp**, first
damage at tick **965**; with one turret covering the approach the tank still reaches firing range (first
damage tick 966) and takes the base to **336/400** before dying. What it does NOT do is plan a route: a
U-shaped crater field can still keep a unit inside the pocket, and there is no notion of a route at all.

Two constraints the code keeps, because the obvious alternatives are wrong:

- **Working around an obstacle is STATE, not a decision.** Steering re-aims at the goal every tick, so a
  one-tick dodge cancels itself out. The `Detour` lives on the unit and inside `state_hash()`, so a
  replay reproduces the same walk.
- **The step test is not the "is the way clear" test.** Units move about a fifth of a tile a tick, so
  "is the tile I would land on enterable" is almost always true -- it is the tile they are already
  standing in. It decides one step. The escape and the detour lifecycle use the whole-tile version;
  applying that to ordinary movement stops units crossing the map at all.

**Next: deterministic A* on the tile grid, followed as waypoints -- its own milestone, after the
economy.**

| option | what it fixes | cost and what it does NOT touch |
|---|---|---|
| **Deterministic A\* on the tile grid, followed as waypoints** (new module, ~300 lines + tests) | units actually reach destinations, including around U-shapes and long rims; makes an advance-to-contact order mean what it says | the world is a pure function of `(seed, x, y)` so the grid is cheap and the search is deterministic, but it puts a waypoint list into the order and replay format, so the format gains state and the replay corpus regenerates. It re-records the battle properties the detour already moved |

An economy whose harvesters cannot reliably reach a resource field is not worth building first, and an
A* nobody has yet needed is a module tuned against nothing. Clearing the spawn corridor was rejected:
it hides a gameplay defect behind a favourable map.

Recorded values, for provenance: the seed-42 1200-tick battle ends with **7 units** and state hash
`9354737314166346737`; a 300-frame `--autodrive` headless run hashes `8785776222935281865` (the detour
is part of unit state, so it is inside both numbers).

**A single unescorted tank destroys an undefended factory, and that is the design.** Factories are
unarmed on purpose; the base was never safe, it was only unreachable.

> **Factories are unarmed, and that is deliberate.** A factory has no defences of its own. Defence is
> what TURRETS are for: placing them, covering an approach, and deciding what is worth defending is
> the player's job and the strategic layer's job, not a property of the building. A factory that
> defends itself would make turrets decoration.

Two things this parks rather than forgets, at Harri's request (2026-10-17):

- **Shields** -- a future possibility, and an ENERGY concept (something that regenerates, something
  that has to be knocked down before damage lands). It is not a factory armour tweak, so it does not
  belong in the balance table next to hp.
- **Built-in factory defences** -- also a future possibility, and if they ever arrive they are
  *turrets on the structure* (the existing `UnitKind::Turret` with a mounting rule), not a new kind
  of building weapon. One mechanism for "shoots at things", always.

Measured for the record, so the design has a number under it: with one turret covering the approach,
the same single tank still reaches firing range (first damage tick 966) and takes the base to
**336/400** before dying. So a defended factory already costs an attacker its tank; an undefended one
is free to take.

### The world is drawn mirrored (found by playing it)

Harri's first session produced two reports that are the same bug seen twice: *"A and D seem to be
reversed -- I would expect A to turn me to the left"*, and *"after a few seconds I could not move
forwards or backwards, only turn around"*.

The first is a chirality bug in the camera. `look_at` builds its right vector as `cross(f, up)`, which
the code's own comment records as **-X** at yaw 0 (forward `+z`, up `+Y`). The camera basis is therefore
left-handed against the tile grid: **tile +x is drawn on the left of the screen**. A heading increase,
which the sim's arithmetic calls "right" (heading `(sin h, cos h)` swept toward `+x`), appears on screen
as a left turn. Pressing `A` consequently turned him right.

Fixed for now at the *input* level, with the sign and the reason written at the line: `A` adds to the
heading, so it turns the view to screen-left, which is what a player means by left. The honest fix is to
stop mirroring the world -- mapping tile `y` to camera `-z` so `cross(f, up)` becomes `+x` -- which
flips that line back. It is a presentation-layer change (no golden hash can move) but it touches every
vertex this renderer emits, so it wants its own slice and its own re-verification of the pixel tests.

The second report is not a rendering issue at all: **the tank had been destroyed**, and the game said
nothing. Turning still worked because the heading was updated *before* the "is my unit still in the
world" check, while movement needs a live hull -- a half-dead game state that reads as a frozen one.
Two rules now hold it: a commander whose unit is gone does not rotate its view, and the session says so
once, in words, and takes no more input. `tests/test_session.cpp` pins both, the first through the real
projection (the test asks where a point dead ahead is *drawn*, rather than trusting a sign).

### Cross-presenter determinism, and a stray event that broke it

The claim "the windowed and headless paths agree" was first observed at M3b from a single pair of runs.
Checked properly on the laptop at 120/180/240/300 frames: the two paths agree at every size, and the
windowed run is stable across repeats. **But one windowed run in eight disagreed** -- same seed, same
`--autodrive`, same tick, different `state_hash()`.

The mechanism was in the loop, not in the sim: the windowed path takes `hands = window->input()`, so a
real key or click arriving at a window that has just taken focus enters the world, and the headless path
has no such channel. The sim was never at fault, and it is not a race: it is the only one of the two
paths where a human's hands reach the simulation.

The fix is the semantics `--autodrive` should always have had: a scripted run drops the real input frame
(events are still pumped, so the window stays responsive and its internal state stays coherent). A
verification run has to be a function of the seed and the flags; anything else makes the comparison a
coin flip that usually lands on heads.

Cross-machine confirmation fell out of it: `--seed 42 --frames 300 --autodrive` gives state hash
**8785776222935281865** on the MacBook under lavapipe and on the laptop under the GTX 1060 -- two GPUs, two
driver stacks, one world.

**Explicitly not in M5a:** an LLM anywhere near the tick loop (latency, cost and nondeterminism all
forbid it), and any tuning search over the parameters. Those stay offline, against the headless
harness, and only if the measured gap above says they are worth it.

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
