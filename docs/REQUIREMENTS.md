# PROTEKT — requirements

What the game IS and MUST BE, with the status of every part. Written against tree base `b3b153d`.

## 0. How to read this

`docs/PLAN.md` owns WHEN: the order of the work and the decisions behind it. This document owns WHAT:
the rules the game has, or is meant to have. `README.md` is the player's manual and
`CODING_STANDARDS.md` is how the code is written. None of the three is restated here beyond what a
rule needs.

Every rule, feature or behaviour carries exactly one status marker.

| marker | meaning |
|---|---|
| `[SHIPPED <commit>]` | In the code today, and read there. The commit is the first one that introduced the named symbol (`git log -S`); the `file:line` in the provenance line is where it lives now. Where no commit could be pinned, a `file:line` stands in and says so. |
| `[DECIDED — not built]` | A ruling of Harri's that no slice has built. It names who decided and when. A few rulings are about process and have no code at all; they carry this marker and say "process". |
| `[INTENDED — unconfirmed]` | Reads like a plan or an assumption, and nobody has ruled on it. |
| `[INTENDED — status unverified]` | Could not be classified from the tree or the documents. Each one is listed in section 14. |

Rules for the provenance lines:

- Every section ends with a `PROVENANCE:` line. A reader can check any claim against it.
- `Harri <date>` is a ruling that appears in a repo document or a code comment, and the citation
  says where. `Harri <date> (brief)` is a ruling that appears in no repo document: it comes from the
  brief that commissioned this file. Section 14 asks Harri to confirm each one so it can be cited
  from the repo next time.
- `BZ2 capture 2026-10-24` means the screenshots Harri took of BattleZone II, as quoted by
  `include/protekt/game/menu.hpp` and `tests/test_menu.cpp`.
- Line numbers are against `b3b153d`.

Terms. The sim's unit kind is `Scavenger`. Code comments call the same unit a "hauler", and BZ2 calls
it a harvester. This document says "scavenger" for the kind and quotes the other words only when it
quotes code. Section 9 records the team-naming ruling. The sim's ledger of delivered loads is called
`ore` in code (`Sim::ore`), "SCRAP" on the HUD and "scrap" for the pieces on the
ground; they are one currency. A battle OPENS with a field of pieces near each base (C0) -- instant
action said "some scrap" -- and the amount is the composition's own dial (`ScenarioData::scrap_per_side`,
default 12 a side, one piece one load). A scenario assembled by hand opens on clean ground.

PROVENANCE: `docs/PLAN.md:1-11`, `README.md:1-9`, `CODING_STANDARDS.md:1-5`,
`include/protekt/game/menu.hpp:46-50`, `include/protekt/sim/sim.hpp:108`.

## 1. The game

PROTEKT is a solo, first-person hover-tank game in the manner of Battlezone and Battlezone II:
Combat Commander, with a real-time-strategy command layer on top. One tank is driven. Everything else
is commanded.

- [SHIPPED 029a7b1] The player drives one unit with the keyboard and mouse: `W`/`S` throttle,
  `A`/`D` turn (or strafe, for a hover tank), the mouse looks. At the start the driven unit is the
  first player `Tank` in spawn order.
- [SHIPPED b865ac1] The player commands the rest through a selection and the order verbs `move_to`,
  `attack`, `follow`, plus the stances the sim understands (section 5).
- [SHIPPED 1b64326] The simulation is a deterministic, fixed-step core in Q16.16 integers with no
  floating point, no clock and no unseeded randomness. It knows nothing about windows, GPUs, audio or
  keyboards.
- [SHIPPED f4998a4] The world is a pure function of `(seed, chunk)`. The same seed gives the same
  world bit for bit on any machine.
- [SHIPPED e5469a2] The renderer, the text log, the sound, the AI and the player's hands are clients
  of the sim. They read its state and issue orders through the same `OrderSink`. A build test reads
  the sources and fails if a lower layer includes a higher one.
- [SHIPPED 94f0acc] The playable game runs one enemy commander, `ai::ScriptedCommander`, on the
  enemy side. See section 14 for what that means for the AI described in the README.

Why the sim being a client-independent core is a REQUIREMENT and not a preference (rationale; each
reason is cited, and the requirement itself is the status-marked bullets above):

1. A save, a bug report and a replay are all "a seed plus the order log". They only work if the same
   inputs always give the same battle. (`docs/PLAN.md:58-59`, decision D2.)
2. The gate machine has no GPU. Everything between a keystroke and the simulation has to be testable
   headless, or it is not tested. (`docs/PLAN.md:49-51`, `docs/PLAN.md:189-191`.)
3. The human and the AI must issue the same orders through the same interface, or "the AI is an
   enhancement, not a dependency" (D3) is not true. (`docs/PLAN.md:47-52`.)
4. A windowed frame and a headless frame of the same seed and tick count must be the same bytes. That
   is the only way "what you see is what the suite checks" holds. (`docs/PLAN.md:207-210`,
   `docs/PLAN.md:543-562`.)
5. [SHIPPED 029a7b1] One windowed run in eight once disagreed because a stray real key reached the sim.
   That bug is why a scripted run (`--autodrive`) drops real input. (`docs/PLAN.md:543-558`.)

- [SHIPPED e5469a2] The floating-point rule (`CODING_STANDARDS.md` rule 13) and the layering rule
  (rule 14) are enforced by `tests/test_layering.cpp`, which reads the sources.

PROVENANCE: `README.md:3-13`, `docs/PLAN.md:30-75`, `CODING_STANDARDS.md:7-21`,
`src/app/game.cpp:374,503,574-580`, `src/game/session.cpp:38-47`, `tests/test_layering.cpp`,
`include/protekt/sim/units.hpp:16-34`.

## 2. Scope

### 2.1 The decisions of 2026-10-16

| # | Requirement | Status |
|---|---|---|
| D1 | Solo only. Scripted scenarios and AI opponents. No networking. | [SHIPPED 1b64326] — a search of `src/`, `include/`, `fuzz/`, `tools/` and `CMakeLists.txt` for socket, asio, enet, gamepad and joystick finds one match, an incidental substring in a comment at `src/sim/projectile.cpp:129` |
| D2 | Same seed, same world, bit for bit. "The primary engineering requirement, not a feature." | [SHIPPED f4998a4] — `region_hash` is the oracle; the golden values are in the suite |
| D3a | The sim and its tests need no AI module. | [SHIPPED e5469a2] — `sim/` never includes `ai/`, and `--commander none` is a legal `protekt-sim` mode |
| D3b | The *game* is complete with zero AI modules loaded. | [INTENDED — status unverified] — `src/app/game.cpp:374` calls the enemy commander unconditionally, and no flag removes it; see section 14 |
| D4 | Keyboard and mouse. No gamepad, no touch. | [SHIPPED 029a7b1] — `include/protekt/input/input.hpp` has keys, clicks and mouse deltas only |
| D5 | Unlicense, on GitHub (`HarryPehkonen/PROTEKT`). | [SHIPPED d79f923] — `LICENSE` is in the first commit; `origin` is `git@github.com:HarryPehkonen/PROTEKT.git` |
| D6 | MVP first, one reviewable step at a time. Harri reviews and redirects after each milestone. | [DECIDED — not built] — Harri 2026-10-16, `docs/PLAN.md:20` (process; no code) |
| D7 | C++ to the Core Guidelines, a self-hosted gate (`tools/ci.sh`), no external CI. | [SHIPPED d79f923] — the gate and hooks are in the scaffold commit; there is no `.github/` |
| D8 | Claude Code writes most of it; Haiku implements, a stronger model plans. | [DECIDED — not built] — Harri 2026-10-16, `docs/PLAN.md:21`, `docs/PLAN.md:609-632` (process; no code) |

### 2.2 Standing scope rules

- [SHIPPED CMakeLists.txt:131] Vulkan and SDL3 are optional at build time (`find_package(... QUIET)`, lines 131 and 162). Without them the game builds and runs
  headless, and that is how the gate tests it.
- [SHIPPED 67ea954] A machine with no sound device, or `--no-sound`, runs silent.
- [DECIDED — not built] No LLM anywhere near the tick loop. Harri 2026-10-17, `docs/PLAN.md:339-342`
  and `docs/PLAN.md:564-566`. Nothing to build: it is a prohibition.
- [SHIPPED 1b64326] A factory is unarmed on purpose; defence is what turrets are for. Harri
  2026-10-17, `docs/PLAN.md:497-500`; `kUnitSpecs` gives the factory range zero.
- [DECIDED — not built] Shields are a future ENERGY concept and not a factory armour tweak. Harri
  2026-10-17, parked, `docs/PLAN.md:502-506`.
- [DECIDED — not built] If a factory ever gets defences of its own they are turrets on the structure
  and not a new kind of building weapon. Harri 2026-10-17, parked, `docs/PLAN.md:507-510`.
- [DECIDED — not built] Saving a game mid-battle is a snapshot of the sim, not a replay. It carries a
  format version and the state hash it was written with, and it is refused mid-tick or in a mission
  that declares itself no-save. Harri 2026-10-17, `docs/PLAN.md:570-607`.
- [INTENDED — status unverified] "Strategic controllers": the player places a controller with a
  standing intent and it posts turrets and calls targets. Harri's sketch, 2026-10-17,
  `docs/PLAN.md:309-345`. The module was deleted unwired (`8e247e2`); whether the sketch is a
  requirement is not stated.
- [SHIPPED 67ea954] Audio exists. `docs/PLAN.md:11` and `docs/PLAN.md:661` list audio as a non-goal;
  that is stale (section 14).

### 2.3 Platform and toolchain

- [SHIPPED d79f923] CMake 3.20 or newer, C++20, clang or gcc, GoogleTest. `clang-format` and
  `clang-tidy` for the full gate.

PROVENANCE: `docs/PLAN.md:15-26`, `docs/PLAN.md:11`, `docs/PLAN.md:659-665`, `README.md:206-211`,
`CMakeLists.txt:7,22,131,162`, `src/app/game.cpp:167`, `include/protekt/sim/sim.hpp`, Harri 2026-10-17
as cited.

## 3. The world

- [SHIPPED f4998a4] The world is `f(seed, chunk_coord)`: generated on demand, never stored. A chunk is
  64 by 64 tiles. The playable world is 256 by 256 tiles (`kMapTiles`), which is 4 by 4 chunks.
- [SHIPPED 1b64326] A tile is whole. It has an integer height in `[-512, 512]` and a terrain, one of
  `Flat`, `Rough`, `Ridge`, `Crater`. There is no floating point in generation.
- [SHIPPED 1b64326] Passability is the terrain's speed factor: `Flat` 1, `Rough` 3/4, `Ridge` 1/2,
  `Crater` 0. A factor of zero means a unit cannot enter. The map edge also blocks. `tile_passable`
  answers the question for a caller that is not the mover.
- [SHIPPED 0eb1986] Units plan routes over the tile grid with a deterministic integer A\*: eight
  connected, no corner cutting, ties broken by lowest f, h, x, y. A goal that cannot be reached gets
  no route and the unit steers straight at it.
- [SHIPPED fc0d91d] Ground height matters to guns. A gun does not shoot a hill: a target with no line
  to it is not a target, costs no bolt and is not reported as fired, except the one the player's
  trigger named.
- [SHIPPED include/protekt/game/scene.hpp:31] A tile is 2 metres in the renderer (`kMetresPerTile`). The sim works in tiles.
- [SHIPPED e2a5be0] Both sides spawn on passable ground. The player starts in the low-coordinate
  quadrant and the enemy in the high one, and the enemy base is the candidate with the fewest blocked
  tiles on the straight lines from the player's tanks. The scenario is a pure function of the seed.
- [SHIPPED 029a7b1] The player's tank is turned at spawn toward the longest clear run
  (`clearest_heading`, 64 directions), so it does not start facing a wall.

The two views of the world, and the rule about what each shows:

| view | key | shows | does not show | status |
|---|---|---|---|---|
| Radar | always on | everything alive within 40 tiles of the driven unit, yours and theirs; your factory as an arrow at the rim when it is out of range; your nav marks | any hostile beyond 40 tiles — not dimmed, not on the rim, not hinted at | [SHIPPED f8e65de] |
| World map | `R` toggles | the whole 256 by 256 world, north up, in the ground's own colours; your living units and buildings, the driven one in white with a heading needle; your nav marks with their numbers | **any enemy asset, ever** | [SHIPPED 5458ec4] |

- [SHIPPED 5458ec4] The rule is one loop: `world_map` copies only the viewer's side's living units,
  and there is no other path from a unit to the map. A test scans the result by id.
- [SHIPPED 5458ec4] The map is a picture. It issues no order and cannot be clicked.
- [SHIPPED 20f224e] The map takes the radar's corner. While the map is up the radar is not drawn.
- [SHIPPED f8e65de] The radar is heading-up: the panel turns with the tank. N, E, S, W sit on the ring
  for the true directions (`83d29ac`).
- [SHIPPED include/protekt/game/radar.hpp:125-137] The radar is range-based, not line-of-sight. A hostile behind a hill inside 40
  tiles is on it. README calls this the version to play before adding occlusion.

PROVENANCE: `include/protekt/world/world.hpp:9-24,37-52`, `include/protekt/sim/units.hpp:16-18`,
`src/sim/sim.cpp:22-35,982-985`, `src/sim/scenario.cpp:62-94,120-162`, `include/protekt/sim/sim.hpp:73-76`,
`include/protekt/game/scene.hpp:31`, `include/protekt/game/radar.hpp:12-31,125-137`,
`include/protekt/game/world_map.hpp:1-20`, `src/game/world_map.cpp:135-146`,
`src/game/session.cpp:17-26`, `docs/PLAN.md:478-487`, `README.md:90-108,201`, `README.md:33`.

## 4. Units and the roster

### 4.1 The seven kinds and their measured numbers

The spec table is `kUnitSpecs` in `include/protekt/sim/spec.hpp:77-102`. The sim runs at 30 ticks a
second. Speeds are tiles per tick in the table and are shown per second here. Ranges are in tiles.

| kind | index | what it is for | hp | speed (tiles/s) | range | damage | cooldown (ticks) | status |
|---|---|---|---|---|---|---|---|---|
| `Tank` | 0 | the main combat unit; fires on the move | 100 | 6.0 | 12 | 8 | 15 | [SHIPPED 1b64326] |
| `Turret` | 1 | a deployable defence: crawls unmounted, fires only when mounted | 150 | 1.5 | 16 | 12 | 20 | [SHIPPED e15935e] |
| `Factory` | 2 | the base: repairs units near it and builds tanks; unarmed and immobile | 400 | 0 | none | 0 | 0 | [SHIPPED 1b64326] |
| `HoverTank` | 3 | a tank's numbers, driven differently (A/D strafe, the mouse turns the hull) | 100 | 6.0 | 12 | 8 | 15 | [SHIPPED 6812c57] |
| `Mortar` | 4 | lobs a splashing shell over hills; outranged by a turret | 75 | 3.0 | 14 | 16 | 45 | [SHIPPED 564c1b2] |
| `Rocket` | 5 | slow, heavy, single target | 100 | 6.0 | 15 | 24 | 75 | [SHIPPED 564c1b2] |
| `Scavenger` | 6 | unarmed hauler: carries wreckage home to its own factory | 60 | 4.5 | none | 0 | 0 | [SHIPPED e653f16] |

Derived and exact values behind the table:

- [SHIPPED 1b64326] Tank speed is 13107 raw Q16.16 per tick (0.2 tiles/tick). A turret's is a quarter
  of that, a mortar's a half, a scavenger's three quarters. A tank shot costs a factory 8 hit points.
- [SHIPPED be02c32] Weapons are data. Every armed kind's bolt flies at 16 tiles a second with a
  climb ceiling of 2.0. The mortar's bolt speed is half, the rocket's three quarters. Bolts fizzle after
  16 tiles.
- [SHIPPED 564c1b2] The mortar's shell has gravity 2.25 height units per tick squared, an arc cap of
  4.0 and a 1.5 tile splash that hurts every living unit in radius, either side.
- [SHIPPED fd58977] The bouncer is a *round*, not a unit: it leaves the barrel with the mortar's
  ballistics, bounces up to 3 times, rests 90 ticks spinning, and pulses shrapnel every 15 ticks
  (12 damage at the centre, falling linearly to zero at 3 tiles).
- [SHIPPED a79e7bf] A turret is mounted after 90 ticks (3 s) of unbroken stillness. A mounted turret
  told to move spends 90 ticks unmounting first (`720b7fd`). Harri 2026-10-17 set the mount time at
  3 seconds; the unmount time is the same figure (section 14).
- [SHIPPED 1b64326] Every tile's speed factor multiplies a unit's speed (section 3).
- [SHIPPED 720b7fd] Every kind but the factory can be driven (`is_drivable`), which includes the
  unarmed scavenger. An order from the player outranks the scavenger's fetch.
- [SHIPPED e653f16] `UnitSpec::buildable` is false for every kind, and `tests/test_roster.cpp:106`
  (`Roster.NoKindIsBuildableYet`) pins that. A factory nevertheless builds a `Tank` (section 8 and
  section 14).

### 4.2 Who starts with what

- [SHIPPED 0effaea] `side_roster(side)` returns the kinds a side spawns with, in spawn order. Both sides
  carry the same list: 3 `Tank`, 1 `HoverTank`, 1 `Factory`, 2 `Turret`, 1 `Mortar`, 1 `Rocket`.
  Nine a side, eighteen in all. Ids ascend in spawn order. Pinned by
  `Scenario.BothSidesGetTheFrozenComposition`.
- [SHIPPED a67233b] The enemy mirrors the player slot for slot, including the hover tank.
- [SHIPPED e653f16] No `Scavenger` is in `side_roster`. Neither side starts with one (section 8).

### 4.3 The command row

- [BUILT, ROW-SLOTS] The row is `~`, `1` (recycler), `2` (factory), `3` (armory), then `F1`..`F10`: fourteen
  keys, each in the same spot on the screen for the whole game (`row.hpp`: slot 4 is `F1`, slot 13 `F10`).
  Harri 2026-10-24, from a Battlezone: Combat Commander capture.
- [BUILT, ROW-SLOTS] A key that holds nothing is not drawn, and the row does not close up: a wiped-out
  group leaves a blank space for the next vehicles built. No dim ink, no placeholder, no zero.
- [BUILT, ROW-SLOTS] Buildings are only on the numbered keys; `F1`..`F10` hold groups of VEHICLES, not
  kinds. One kind may hold several keys; one key may hold a single vehicle. Today only `2` (the
  factory) has a kind; `1` and `3` are empty and so not drawn. `~` is drawn when a nav mark exists.
- [BUILT, ROW-SLOTS] A vehicle not seen before takes the first empty `F` slot, lowest first, filling a
  gap in the middle before anything after it. A unit keeps its slot until its whole group is gone.
  Every vehicle is its own group for now; a shift-built batch will share one key while a lone build takes
  its own (Harri 2026-10-24), and needs a batch id the sim does not yet record.
- [BUILT, ROW-SLOTS] An undeployed recycler may sit under any `F` key. Until deploying exists (RB3) it
  stays a vehicle in the F-row; on deploy it will move to `1`.
- [BUILT, ROW-SLOTS] A drawn key shows the kind's name, how many vehicles the group holds, and a health
  bar: the group's summed hit points over its summed ceilings.
- [BUILT, ROW-SLOTS] The memory is in the game layer (`RowState`); the sim has no new field and no hash
  depends on it.
- [BUILT, ROW-SLOTS] The digit row selects nothing. Pressing an `F` key whose slot is empty is refused
  with `You have no group F<n>.` and leaves the selection alone.
- [SUPERSEDED, ROW-SLOTS] K1's key pool (a key per kind, dealt in spawn order, survivors moving down into
  a freed key, `F1..Fn` with no gap) is gone: it is the opposite of the ruling above.

### 4.4 Death

- [DECIDED — not built] Harri 2026-10-24 (brief): a dead unit explodes and vanishes. There are no
  greyed-out rows and no ghost slots in any roster or HUD.
- [SHIPPED src/game/scene.cpp:3059] The roster strip, the radar, the world map, the unit bars and the draw
  loop each skip a unit with `alive == false` (`src/game/row.cpp:19`, `src/game/radar.cpp:253`,
  `src/game/world_map.cpp:142`, `src/game/scene.cpp:3059-3061`). A dead unit has no row, no mark and no
  mesh.
- [SHIPPED 67ea954] The "explode" half is a sound: a dead unit of yours is heard at full volume, and
  anyone else's within 90 tiles. There is no explosion *visual* in `scene.cpp`.
- [SHIPPED e653f16] A dead unit leaves four pieces of wreckage (section 8).
- [SHIPPED 61298d2] The corpse stays in the sim's unit table with `alive == false` (the commit's
  `ADeadUnitsIdIsNotWhatTheMenuIsAbout` test anchors on it), so ids are never reused and every consumer
  has to filter on `alive`.

### 4.5 Retreat and repair

- [SHIPPED 18eef6f] A living friendly factory mends every living non-factory unit of its side within 6
  tiles, 1 hp every 6 ticks, up to the kind's maximum. It applies to both sides and costs nothing.
- [SHIPPED 7f681fb] The player's other tanks and hover tanks (never the one being driven) retreat to a
  living own factory at 20 hp or less and return to their remembered last order at 80 hp or more. With no
  factory they keep fighting. The thresholds are `UtilityTuning::retreat_hp` and `return_hp`; the
  player-side rule reads the same numbers.
- [SHIPPED bf4ec55] A wounded tank at its own factory stays until it reaches the return threshold.
- [SHIPPED 2e0c9b8] An idle own tank with no standing order defends in place: every 15 ticks it fires on
  the nearest hostile in range, never over a player order, never when retreating or driven.
- [SHIPPED fe6eb9a] `ai::UtilityCommander` withdraws a hurt tank on the same 20/80 numbers. The
  playable game's enemy does not use it: `ai::ScriptedCommander` reads no hit points
  (`src/ai/scripted_commander.cpp`, 44 lines).
- [DECIDED — not built] 0 RECYCLE is parked. Harri 2026-10-22 (brief) asked to hold off but keep it in
  the plan. The harvester menu draws a live `0 Recycle` row (section 6) and nothing is behind it.
- [SHIPPED 87cf06f] The follow radius is 4 tiles (`kFollowRadiusTiles`). Harri 2026-10-17 (brief).

PROVENANCE: `include/protekt/sim/spec.hpp:13-102`, `include/protekt/sim/units.hpp:36-49,120-172`,
`include/protekt/sim/scenario.hpp:10-24`, `src/sim/scenario.cpp:34-60`, `tests/test_scenario.cpp:28-44`,
`tests/test_roster.cpp:25,106`, `include/protekt/game/row.hpp:9-34,53-70`,
`src/game/row.cpp:14-38`, `tests/test_row_slots.cpp:238-296`, `src/sim/sim.cpp:849-867`,
`src/game/interaction.cpp:291-326`, `include/protekt/ai/utility.hpp:44-51`,
`src/ai/utility.cpp:78-113` (`role_of`, `threat_of`), `src/game/sounds.cpp:121-136`, `README.md:37-38`,
Harri 2026-10-17 (brief), Harri 2026-10-22 (brief), Harri 2026-10-24 (brief).

## 5. The player's verbs

### 5.1 The control map

`include/protekt/input/bindings.hpp` owns the movement and command predicates and
`include/protekt/input/input.hpp` the scancodes; `src/game/interaction.cpp` owns what the clicks and
command keys mean. The README table and the bindings agree on every key. Where they disagree is on
meaning, and those are in section 14.

| input | what it does | status |
|---|---|---|
| `W` / `S` | throttle forward / back; the driven tank always drives the way its body points | [SHIPPED 029a7b1] |
| `A` / `D` | turn the body, on the spot or while driving (a driven *hover tank* strafes instead) | [SHIPPED 029a7b1] |
| mouse | turn the turret relative to the body; the crosshair is always screen centre; 0.0025 rad per pixel; pitch limited to 1.3 rad | [SHIPPED 491a3ec] |
| `E` (held) | boost: lifts the hull; presentation only, not counted as "driving" | [SHIPPED 3c7070c] |
| left click | the trigger: one bolt along the crosshair; rate-limited to one every 8 ticks | [SHIPPED 826b1e6] |
| right click | switch weapon, gun → rocket → mortar → bouncer → gun; the ONLY weapon switch | [SHIPPED a2400a9] |
| `Space`, nothing selected | select the one own unit under the mark, if the world view draws it (inside the drawn radius); ground, hostiles and the driven tank are ignored. The mark, Space and the `T` list only ever reach what the world view draws: inside the drawn radius, and for a hostile also inside the viewer's radar reach (40 tiles, a scout's 80) | [SHIPPED 3404ba9] |
| `Space`, something selected, a tank under the mark | the selection follows it | [SHIPPED 3404ba9] |
| `Space`, something selected, a hostile under the mark | the selection attacks it | [SHIPPED 3404ba9] |
| `F1`–`F10` | select every unit of the kind that holds that key | [SHIPPED 160dec4] |
| `Tab` or `Escape` | clear the selection (and cancel a pending `G` or `T`) | [SHIPPED 029a7b1] |
| `X`, mark on an own drivable unit | swap: drive that unit; the one you leave gets a `Stop` and becomes an ordinary unit | [SHIPPED c76d8cf] |
| `F`, mark on a living unit | the selection follows that unit, holding within 4 tiles | [SHIPPED 6e09100] |
| `M` | the selection moves to the ground under the mark, even when a unit is in front of it | [SHIPPED 86a0261] |
| `T`, hostile under the mark | the selection attacks it directly | [SHIPPED 029a7b1] |
| `T`, no hostile under the mark | arm the attack list: the nearest enemies within radar range, numbered `1`–`9` then `0`; the next digit attacks that row | [SHIPPED bbe4650] |
| `N` | drop a numbered nav mark `0`–`9` under the crosshair, or clear the one aimed at; ten is the limit | [SHIPPED 4594cec] |
| `G`, then `0`–`9` | send the selection to that nav mark as a move order; `Escape` cancels; any other key cancels and then acts | [SHIPPED 30b618c] |
| `R` | toggle the world map in the radar's corner | [SHIPPED 20f224e] |
| `O` | BZ2's objectives key; unbound until objectives exist | [SHIPPED 20f224e] |
| `C` | toggle the colour calibration view (red, green, blue bars) | [SHIPPED 3c7070c] |
| `F11` or `Alt`+`Enter` | toggle full screen | [SHIPPED 47ba745] |
| `Ctrl`+`Q` | quit from anywhere; deliberately a modified key | [SHIPPED 47ba745] |
| digits `1`–`9`, `0`, no prompt open | act on the selected subject's command menu, for the two rows that can act: `1 Follow Me` (the selection follows the craft you are driving) and `4 Hold` (each unit guards its own position with `GuardAt`). A digit the menu has no row for, a row the model dims, or a subject with no menu does nothing at all — and the recycler's menu has no `4` | [SHIPPED d80ce95] |
| digits `1`–`9`, `0`, a prompt open | answer `G`, `T` and the attack list exactly as before: the prompt swallows the digit first, and the menu never sees it | [SHIPPED 160dec4, precedence pinned d80ce95] |
| digits in a command menu, rows that cannot act yet | inert, with no message: `2 Go to Nav` (which mark it means is unruled), `3 Scavenge` (the fetch is automatic), `3 Deploy` (RB3), `5 Pick Me Up`, `9 Bailout`, `0 Recycle`, `~`, and the factory's seven build rows (RB5) | [SHIPPED d80ce95] |

Selection and orders, the rules behind the rows:

- [SHIPPED 029a7b1] A selection is a set, sorted ascending and unique, bounded, and drops the dead.
  A replay of human play cannot see the order the player selected in. (`b865ac1` for `cmd::Selection`.)
- [SHIPPED 029a7b1] The tank you are sitting in is never moved by an order. It is selectable, and the
  one thing an order may do to it is fire: `FocusFire`. Orders given to a selection that includes it go
  to the other units; "You are driving that one" is the refusal when the driven tank is all there is.
- [SHIPPED 3404ba9] `Space` never ADDS to a selection and there is no `Shift`+`Space` gesture. A
  selection comes from a function key or from `Space` over one unit starting from empty. Harri's
  directive, 2026-10-23 (U3 commit).
- [SHIPPED d80ce95] A digit with no prompt open is a row of the selection's menu, and neither routed row
  ever orders the craft you are driving — `1` and `4` order the rest of the selection and refuse when that
  is all there is, exactly as the verbs above do. `Hold` is `GuardAt` built one unit at a time at that
  unit's OWN position, in ascending id order, and deliberately not `Stop`: `Stop` idles a unit, and a
  harvester's fetch re-picks scrap on the same tick, so it would hold nothing, while `GuardAt` is an order
  stance the fetch leaves alone. No new command builder was needed.
- [SHIPPED 87cf06f] `Follow` is a standing order: closes when outside 4 tiles, stands still inside,
  fires at what is in range, never leaves the radius to chase, lapses if the target dies. Any living
  unit of either side may be followed except the follower itself.
- [SHIPPED c76d8cf] A swap refuses an enemy unit, the factory, a dead unit, the unit already driven and
  an empty mark.
- [SHIPPED 1b64326] The sim's order vocabulary is `MoveTo`, `GuardAt`, `Patrol`, `Attack`, `Stop`,
  `FocusFire`, `FireTowards`, `Strafe`, `Follow`, `FireLob` (`OrderKind`, ten values). `GuardAt`,
  `Patrol` and a bare `Stop` have no player key today; the AI uses them.
- [SHIPPED 029a7b1] Shooting never costs the selection. Only `Tab` and `Escape` clear it.

### 5.2 The mark and the shot

- [SHIPPED c9caf78] The mark is the crosshair, fixed at screen centre. White means nothing is under it,
  green means one of yours, red means a hostile. The unit under it is tinted too, amber for yours and
  salmon for a hostile.
- [SHIPPED be6da0a] The shot goes where the crosshair points and nothing aims for it. A unit under the
  mark is intent only: it decides whom the shot may hurt, not where it goes.
- [SHIPPED 7a225e7] The shot leaves the muzzle and crosses the reticle where the reticle is drawn.
- [SHIPPED 60b21a5] A click fires one shot. The bolt is a real projectile that takes time to arrive,
  inherits the shooter's velocity, keeps flying if its target dies, and fizzles after 16 tiles.
- [SHIPPED 3c7070c] Friendly fire is on purpose. Auto-engagement only ever picks a hostile; an explicit
  order or a named trigger shot can hurt your own. A unit may never name itself
  (`FriendlyFire.AUnitCannotTargetItself`).
- [SHIPPED 27f24f6] With the mortar or bouncer up, the trigger lobs a shell along the view's own
  heading and pitch, with no aim assist and no lead.
- [SHIPPED fd58977] The bouncer is lobbed the same way and fires a bouncing round.
- [SHIPPED 9ee8354] Each weapon has its own reticle and HUD label; the mortar's reticle is a range
  ladder of eight rungs (`kMortarRungPitches`), the lit one being where the view's pitch lands.
- [SHIPPED ad39a68] The reticle spreads for 6 ticks when your shot damages an enemy, and is shaded
  while the gun reloads.

PROVENANCE: `include/protekt/input/bindings.hpp:16-156`, `include/protekt/input/input.hpp:22-39`,
`src/game/interaction.cpp:85-287,291-326,378-586`, `src/game/session.cpp:38-47`,
`src/game/player.cpp:69-136`, `include/protekt/game/player.hpp:30-35`, `include/protekt/game/weapon.hpp:13-58`,
`include/protekt/sim/orders.hpp:26-134`, `tests/test_row_slots.cpp:119-134`,
`tests/test_friendly_fire.cpp:69-140`, `README.md:15-88`, commit message of `3404ba9`
("The owner's directive") and `a2400a9`.

## 6. The command menu

The numbered menu down the left of the frame. Its model is `include/protekt/game/menu.hpp` and its
rows are in `src/game/menu.cpp`.

**What the rows are.** [DECIDED — not built] The rows, the verbs and the per-item costs are a
QUOTATION from Harri's BattleZone II captures of 2026-10-24. The slice that replaces them with real
per-item identity and per-item costs is RB5. Harri 2026-10-24 (brief); `include/protekt/game/menu.hpp:46-50`,
`src/game/menu.cpp:59-62`, `tests/test_menu.cpp:1-10`.

What is built, what is not:

- [SHIPPED 3e799a0] The row model: a pure function `menu_rows(MenuInput)` from what is selected, whose
  it is, what is armed, what is under the cursor and how much scrap is banked, to the rows to draw. It
  routes no input and touches no sim.
- [SHIPPED be3afb4] The rows are drawn down the left margin, key column then verb column, in the order
  handed over, never sorted or renumbered. A `~` glyph was added for the key column.
- [SHIPPED 61298d2] The menu reaches the frame. It appears whenever the selection is made of living
  units of one kind that has a captured menu: `Factory` → `Deployment`, `Scavenger` → `Harvester`,
  `Turret` → `Turret`. A mixed selection, an empty one and every other kind get no menu.
- [SHIPPED b3b153d] A scripted key press (`--key FRAME NAME`, `space`, `tab`, `f1`–`f10`) puts the menu on
  screen headless, and `GameCliTest.TheMenuIsOnScreenWhenTheRosterSelectsAFactory` proves it by pixels.
- [INTENDED — unconfirmed] Arming a command from the menu is RB2 (`3` arms the deploy, the player
  points, `SPACE` places it). `player_view.hpp:51` says "nothing is armed yet (arming is RB2)", and
  `menu.hpp:60-63` describes the flow. No ruling of Harri's names it.
- [INTENDED — unconfirmed] The unit kinds and per-side item names that give the build rows their
  meaning arrive with "the R3 roster" and RB5 (`menu.hpp:147-151`). No ruling of Harri's names the
  roster slice.

### 6.1 What opens the menu, and the rows per selection

| selection | subject | rows, top to bottom | status |
|---|---|---|---|
| a `Factory` | Deployment | `SPACE` state row; `TAB Close Menu`; `~ Drop Off`; then 7 build rows `1 Harvester 20`, `2 Scout 45`, `3 Guardian 40`, `4 Hauler 30`, `5 Builder 40`, `6 Healer 50`, `7 Service Pod 2` | [SHIPPED 3e799a0] |
| a `Scavenger` | Harvester | `SPACE`; `TAB Close Menu`; `~ Select Unit` (dim); `1 Follow Me`; `2 Go to Nav`; `3 Scavenge`; `4 Hold`; `5 Pick Me Up` (dim); *gap where 6, 7, 8 would be*; `9 Bailout`; `0 Recycle` | [SHIPPED 3e799a0] |
| a `Turret` | Turret | the `SPACE` state row alone — not even `TAB Close Menu` | [SHIPPED 3e799a0] |
| a recycler / matriarch, not deployed | Recycler | `SPACE`; `TAB Close Menu`; `~ Rotate Buildings` (dim); `1 Follow Me`; `2 Go to Nav`; `3 Deploy` | [SHIPPED 3e799a0] — modelled and tested, but no `UnitKind` maps to it, so no selection can reach it (`app/player_view.cpp:12-27`) |
| a `Tank`, `HoverTank`, `Mortar`, `Rocket`, or nothing | None | no menu, no rows | [SHIPPED 61298d2] |

- [SHIPPED 3e799a0] The enemy side's build list is EMPTY. A per-side item name and a per-kind cost is
  RB5's and the R3 roster's, and inventing a list "would put names in the mouth of a capture that never
  showed them". A deployment on the enemy side draws no build rows
  (`Menu.TheEnemyBuildListIsNotInvented`).
- [SHIPPED 3e799a0] `~ Drop Off` is drawn as the reference draws it and means nothing yet; no behaviour
  is invented behind it (`Menu.TheDeploymentKeepsTheDropOffStringItCannotExplain`).

### 6.2 The structural rules

- [SHIPPED 3e799a0] A row is a `{key, verb}` pair. The keys are NOT contiguous: the harvester's menu
  reads 1 2 3 4 5, a visible gap, then 9 and 0. Numbering by position is wrong, and the drawn order is not
  key order (`Menu.TheKeysAreNotContiguousAndTheGapIsDrawn`, `Menu.TheRowOrderIsNotKeyOrder`).
- [SHIPPED 3e799a0] A key's verb depends on the selection: `3` is `Deploy`, `Scavenge` or
  `Guardian 40` (`Menu.TheThreeKeyMeansWhateverTheSelectionMakesIt`).
- [SHIPPED 3e799a0] **The top row is a live readout, not a binding.** It is `SPACE Select` when
  something of yours is under the cursor, `SPACE Deploy` once `3` has armed the deploy, `SPACE <item>`
  once a build row has armed that item, and a bare `SPACE` otherwise. It is never dimmed, and pressing
  `SPACE` does whatever the row currently names. Arming does not rewrite the row that armed it
  (`Menu.TheTopRowIsLiveStateNotABinding`, `Menu.ArmingDoesNotRewriteTheBindingThatArmedIt`).
- [SHIPPED 3e799a0] **A row is dim for one of two reasons, under one look.** A build row is dim because
  it is `Unaffordable` (the cost exceeds the banked scrap). An action row such as `Select Unit` or
  `Pick Me Up` is dim because it `DoesNotApply` to this selection. `MenuRow::reason` carries which, and
  `enabled` is exactly `reason == None`. The look is the same ink (`kMenuDimInk`, 0.40 grey)
  whatever the reason (`Menu.ADimmedRowCarriesTheReasonItIsDimmed`, `Menu.NoRowIsEverDimmedForTheWrongReason`).
- [SHIPPED 61298d2] Affordability reads the sim's real ledger (`Sim::ore`). The test steps the sim until
  a scavenger banks a wreck and asserts live and dim rows.
- [SHIPPED 3e799a0] `TAB Close Menu` is the second row of every menu except the turret's.
- [SHIPPED d80ce95] **The digits are the menu's verbs, for the rows that can act.** With no prompt open,
  `1` is `Follow Me` and `4` is `Hold` on a selection whose subject owns a menu. The digit is looked up in
  the MODEL's own rows (`menu_rows`) and only an ENABLED row acts, so a dimmed row and a digit with no row
  do nothing — exactly as they did before anything was routed. Section 7's key table lists which rows act
  and which are inert, and why each inert one cannot act yet.
- [SHIPPED d80ce95] The PROMPTS still win, and that is deliberate: `G`, `T` and the attack list settle a
  digit above the menu in `Interaction::handle` and `continue`, so the menu branch is only reached with no
  prompt open. This is the conservative reading of the next bullet, not the next bullet.
- [INTENDED — unconfirmed] **A menu is a MODE that SUSPENDS the prompts.** What is still unruled is whether
  opening the menu takes the digits away from `G`/`T`/the attack list: the layout says the menu is opened and
  closed (`TAB Close Menu`), but `Tab` clears the selection and nothing suspends anything. In normal mode the
  digits would address buildings (`1` recycler, `2` factory, `3` armory) once those exist. `menu.hpp:40-43`
  writes the mode as a property of the reference menu; whether PROTEKT adopts it is not ruled anywhere I can
  find (section 14, the first of the rules 14.1 could not classify).
- [SHIPPED 61298d2] The menu and the attack list share one slot under the notice, one panel at a time:
  the attack list wins.
- [SHIPPED be3afb4] With no menu rows, a frame is byte-identical to one that never heard of the menu layer (`Menu.DrawnWithNoRowsItEmitsNothingAndEveryFrameIsByteIdentical`).

PROVENANCE: `include/protekt/game/menu.hpp:12-159`, `src/game/menu.cpp:10-149`, `tests/test_menu.cpp:74-640`,
`include/protekt/app/player_view.hpp:38-56`, `src/app/player_view.cpp:9-114`,
`src/game/scene.cpp:2185-2191,2557-2579`, `tests/test_game_cli.cpp:163-318`,
`tests/test_player_view.cpp`, commit messages of `61298d2` and `b3b153d`, BZ2 capture 2026-10-24,
Harri 2026-10-24 (brief).

## 7. The HUD

All HUD text is the built-in 5 by 5 block font (`mount_glyph`). A "cell" is one square of that grid, in
pixels at a 720-high frame, and a glyph is 5 cells tall.

**The type scale.**

- [DECIDED — not built] HUD text follows BattleZone II's MEASURED fixed glyph height of about 10 px:
  cell 2.0 on the 5 by 5 grid at a 720-high frame. BZ2's text is a fixed pixel font that ignores the
  resolution, counted at 10 px in the 800 by 600 originals and still about 10 px in a 1280 by 730
  screenshot of the remaster. "Make it bigger for my eyesight" is an argument Harri has retired, 2026-10-24 (brief). "Make it obvious" buys a brighter ink and an opaque plate, never a bigger cell
  (`scene.cpp:2203-2205`).
- [SHIPPED bd32f0b] These elements are at cell 2.0: the roster strip, the notice line, the attack list,
  the command menu, the scrap ledger and the verdict banner.
- [SHIPPED src/game/scene.cpp:1778] These are not: the weapon label (cell 4.8, `scene.cpp:1778`), the mortar rung digits
  (cell 4.0, `scene.cpp:1797`), the ship instrument's readings (cell 3.0, `scene.cpp:1924`) and the
  `DEPLOYING` plate (cell 3.0 at 450 px, `scene.cpp:1615`). Whether they are exempt is not stated
  (section 14).

### 7.1 The elements

| element | where | what it reads | status |
|---|---|---|---|
| Roster strip | top left, 6 px margin | one plate per kind with living units, in key order: the key (`F1`…), the count as `x<n>`, the kind's short name, and a 3 px bar of the group's summed hp over summed maximum (green above 60%, yellow above 30%, red below). The frame is yellow when any unit of the group is selected, white otherwise. A plate is 88 by 34 px | [SHIPPED 609fd67] |
| Notice line | under the strip, yellow | the last refusal or confirmation, held for `kNoticeTicks` of sim time; the `go where? 0-9 for a mark.` prompt while `G` waits | [SHIPPED d81b8cf] |
| Attack list | under the notice | after `T` with no hostile under the mark: one row per nearest enemy within the radar, `key  kind  metres`, keys `1`–`9` then `0`; the row's meaning is fixed when `T` was pressed | [SHIPPED eff1299] |
| Command menu | the left margin, in the attack list's slot | section 6 | [SHIPPED be3afb4] |
| Radar | bottom left | a wide shallow oval, 40 tile radius, heading-up; the driven unit a triangle at the middle, yours squares, hostiles red squares, your base an outline, nav marks, a rim arrow to your base; terrain relief in a wireframe mesh | [SHIPPED f8e65de] |
| World map | bottom left, replaces the radar | section 3 | [SHIPPED 5458ec4] |
| Scrap ledger | above whichever view holds the bottom-left corner | the word `SCRAP` and the viewer's own `Sim::ore` count, amber on an opaque black plate | [SHIPPED d0ffee1] |
| Ship instrument | bottom right | a C-shaped ring of 12 + 12 segments: the upper arc is hull in green (the driven unit's hp over its spec's maximum), the lower arc is ammo in blue. The two readings sit in the opening. Beside it, the four carried weapons, the active one lit green | [SHIPPED 341fbb3] |
| Reticle | screen centre | section 5.2: four ticks round an open centre and a dot, colour by what is under it, spread on a hit, shaded while reloading; the weapon's name beside it | [SHIPPED ad39a68] |
| Floating unit bars | over each living field unit in view | a 20 by 4 px bar, green yours and red hostile; not on the factory or a turret, and none on the viewer's own tank | [SHIPPED 7dfc67d] |
| `DEPLOYING` plate | over an own turret that is not yet mounted | a yellow plate; a mounted or hostile turret shows nothing | [SHIPPED 3398f08] |
| Hit flash | whole frame | red, fading over 0.4 s when the driven unit is hit | [SHIPPED 3c7070c] |
| Verdict banner | above the crosshair | `YOU HAVE LOST` in red or `ENEMY DEFEATED` in green; nothing while the war is undecided (section 10) | [SHIPPED e23dec8, retargeted onto the recycler by c5eb877] |
| Colour calibration view | whole frame, `C` | three bars, red, green, blue | [SHIPPED 3c7070c] |
| Wreckage | on the ground | each scrap piece a small dark plate lying on the terrain | [SHIPPED 62a948f] |
| Starting field | around each base | the pieces the battle opens with, laid on passable tiles in rings 4 to 12 out from the base centre, one per tile, each side's own | [SHIPPED C0, this commit] |

- [SHIPPED e653f16] The radar's hull bar is retired. Harri 2026-10-23, quoted at `scene.cpp:1433`:
  "Retire radar's hull bar. It's replaced by the health bar at the bottom right." The README's "health
  bar" section is stale (section 14).
- [SHIPPED 341fbb3] **The ammo reading is a named placeholder.** The ship instrument's blue arc is always
  full and its number is always the string `100`. It is the constant `kAmmoPlaceholderFraction` and
  `ammo_placeholder_percent()`, never a number the sim computed, because the sim has no magazine.
  Harri 2026-10-23: "For now, we can keep the ammo capacity always at 100%. When we add capacity, we can
  change that." The slice that replaces it is AMMO-2.
- [DECIDED — not built] Real ammo capacity. Harri 2026-10-23, `scene.hpp:425-431` (AMMO-2).
- [SHIPPED bd32f0b] HUD panels are rounded (5 px corners at 720) with a 65% translucent face and an
  opaque frame and ink, like BZ2's.
- [SHIPPED 341fbb3] The ship instrument's hull reading is the driven unit's hp, from the sim. With no
  driven unit it draws no number at all, because "a number the sim never produced is the lie the ammo
  placeholder exists to avoid".

PROVENANCE: `src/game/scene.cpp:1527-1541,1609-1615,1775-1800,1894-1962,1996-2025,2145-2351,2353-2580,2586-2643,2645-2655`,
`include/protekt/game/scene.hpp:420-433`, `include/protekt/game/radar.hpp:32-48`,
`include/protekt/game/world_map.hpp`, `src/app/game.cpp:383-384,467-469`,
`include/protekt/game/interaction.hpp:29`, `README.md:110-148`, commit messages of `bd32f0b`,
`d0ffee1`, `e653f16`, Harri 2026-10-23 and 2026-10-24 (brief).

## 8. The economy

- [DECIDED — not built] Scrap is THE resource. Harri 2026-10-16 (brief); the D-list in `docs/PLAN.md`
  does not contain it (section 14). The closest text is `docs/PLAN.md:8`, "a resource you physically
  harvest".

### 8.1 Wreckage, the hauler and the ledger

- [SHIPPED e653f16] A destroyed unit scatters four pieces of wreckage (`kScrapPerWreck`) within two
  tiles (`kScrapScatterRadius`), each at the centre of a tile. The position is a pure function of
  `(seed, dead unit id, piece index)`, so a replay scatters the same field. The ground holds at most 512
  pieces.
- [SHIPPED e653f16] A piece that lands on ground no vehicle can enter (a crater) settles onto the
  nearest tile one can, and never onto a tile another piece of the same wreck already holds.
  `Scrap.WreckageNeverRestsOnGroundAVehicleCannotStandOn`.
- [SHIPPED 62a948f] Wreckage is drawn. Harri's complaint was that the litter was invisible.
- [SHIPPED e653f16] The scavenger (hp 60, unarmed) with no orders of its own walks to the nearest piece,
  takes one load (within 1 tile), and carries it to its own side's lowest-id living factory, handing it
  over within 2 tiles. Any order replaces the fetch. Pick-up and hand-over happen wherever the scavenger
  *is*, even under a player's own driving.
- [SHIPPED e653f16] The ledger is one counter per side of delivered loads, `Sim::ore(side)`, hashed
  only while non-zero so a battle with no scavenger hashes as it always did.
- [SHIPPED d0ffee1] The ledger is on the HUD as `SCRAP`.

### 8.2 What is free

- [DECIDED — not built] Harri 2026-10-23 (brief): scrap is spent only when a factory builds a NEW item.
  Repair, energy and ammo are ALWAYS free. Never gate repair, replenishment or firing on scrap.
- [SHIPPED 18eef6f] Repair is free. `Ledger.RepairHappensAndTheLedgerPaysForNothing`
  (`tests/test_scavenger.cpp:359`) pins it, and the repair loop reads no ledger.
- [SHIPPED 1b64326] Firing is free. The firing loop (`sim.cpp` step 4) reads no ledger and no ammo.
- [DECIDED — not built] Energy and ammo as quantities. The sim has neither: "nothing counts rounds and
  nothing ever runs dry" (`scene.hpp:425-427`). The rule that they stay free binds the slice that adds
  them.

### 8.3 The build rule

Ruling: a factory REPLACES A LOST GUN. It does not spend when rich.

- [REMOVED 2026-10-28] The condition was one function, `short_and_affordable(armed_living, armed_started,
  ore)`: the side's living armed units fewer than it started with, and at least `kTankOreCost` (4) loads.
  The production beat was its only caller left (the attrition verdict it was shared with had already been
  retired for the recycler verdict), so the function, `kTankOreCost` and the armed counts went with the beat.
- [SHIPPED e653f16] "Armed" is any kind with a nonzero range: tank, hover tank, turret, mortar, rocket.
  A side starts with 8 of them. A full yard builds nothing and spends nothing
  (`Build.NoLossIsNoBuild`).
- [SHIPPED e653f16] The beat is global, every 150 ticks (5 s). A building factory makes a `Tank`
  whatever kind was lost.
- [SHIPPED e653f16] The new tank appears on the factory's own tile when it is free, else on the nearest
  free passable tile within 3 tiles (`kBuildSpawnRadiusTiles`). A yard with no room builds nothing and
  spends nothing.
- [REMOVED 2026-10-28] The cost was `kTankOreCost = 4` loads (the constant is gone), "one wreck's worth, so one kill pays for one
  tank".
- [REMOVED 2026-10-28, with the beat] The code made ONE build per beat in total, not one per side: the loop ends after
  the first factory builds (`sim.cpp:932-934`). The ruling says one per side per beat. Both sides'
  factories are in the same loop, the player's has the lower id, so the enemy builds only on a beat the
  player's factory did not (section 14). **The wording of that ruling comes from a brief rather than from
  any repo document, so which is wanted — one build per side, or one build in a beat — is Harri's to
  confirm.**

### 8.4 The hauler in the skirmish

- [SHIPPED e653f16] No scavenger spawns in `skirmish(seed)` (`side_roster` has none), and neither
  commander orders one to be built. In the playable game, therefore, no load is ever banked, the ledger
  stays at zero, and no factory ever builds. The economy is complete in the sim and exercised by tests that
  spawn a scavenger by hand; it is inert in the shipped scenario.
- [DECIDED — not built] Per-item identity and per-item costs (HARVESTER 20, SCOUT 45, and so on) replace
  the old flat refund (`kRecycleOre`) and the menu's quotation in RB5. Harri 2026-10-24 (brief).

PROVENANCE: `include/protekt/sim/scrap.hpp:12-41`, `include/protekt/sim/units.hpp:140-172`,
`src/sim/sim.cpp:361-531,849-936`, `include/protekt/sim/sim.hpp:100-109`, `src/sim/scenario.cpp:34-44`,
`tests/test_scavenger.cpp:137-436`, `tests/test_scrap.cpp:81-207`, `include/protekt/game/scene.hpp:425-433`,
`src/game/menu.cpp:59-67`, commit messages of `e653f16`, `62a948f`, `d0ffee1`, `e23dec8`,
Harri 2026-10-16, 2026-10-23, 2026-10-24 (brief).

## 9. The teams

- [DECIDED — not built] Harri 2026-10-24 (brief): there is a human team and an alien team, and the
  player chooses which to play.
- [DECIDED — not built] A scavenger and a harvester are the SAME vehicle, identical in function. They
  differ only in name and appearance, and the difference follows the TEAM, not who is the player.
- [SHIPPED 1b64326] The sim has no faction concept. The only partition is `sim::Side::Player` and
  `sim::Side::Enemy` (`units.hpp:51`), and both sides spawn the same kinds.
- [SHIPPED 3e799a0] Today's names are not keyed by team. The unit-kind label is `SCAVENGER`
  (`roster.cpp:63`) and the player's build list says `Harvester` (`menu.cpp:64`), both on the one side.
  `menu.hpp:93-95` already says "the same kind is a HARVESTER on one side and a Scavenger on the other",
  so the model expects per-side naming and the table does not provide it.
- [DECIDED — not built] Per-team naming is re-keyed when factions exist. Until then no code should
  treat `Side` as the team, and no new string should be added that assumes the player is human.

PROVENANCE: `include/protekt/sim/units.hpp:51`, `include/protekt/game/menu.hpp:90-96`,
`src/game/row.cpp:49-67`, `src/game/menu.cpp:63-67`, Harri 2026-10-24 (brief).

## 10. Winning and losing

- [DECIDED — not built] Winning and losing are a LIST of conditions, never one hard-coded rule. Harri
  2026-10-24 (brief).
- [SHIPPED c5eb877] Destroying the enemy's production building wins instantly and totally. Losing your own
  loses the game. The condition is written on the RECYCLER, which the scenario now spawns one of per side
  (`a584683`): the enemy's recycler -- the alien team's matriarch -- dead means the player has won
  whatever else the enemy still has, and the player's own dead means the player has lost. Both dead reads
  as a LOSS (the reading the old banner used for mutual annihilation). Harri 2026-10-24 (brief).
- [DECIDED — not built] A mission scripts its own conditions, for example "escort an APC; fail if it
  dies". Harri 2026-10-24 (brief). `docs/PLAN.md:600-605` says "There are no missions yet", and no mission
  data type is in the tree. The predicate it would be written in exists (`kind_wiped_out`, below); the
  DATA does not.
- [SHIPPED c5eb877] The earlier attrition verdict is RETIRED by Harri, 2026-10-24. Nobody should keep two
  meanings of "the war is over", and the code no longer does: `Sim::side_has_lost` and its rule are
  DELETED, not merely unused.

What is in the tree:

- [SHIPPED c5eb877] `Sim::kind_wiped_out(side, kind)` is the one named predicate the conditions are
  written in: true when the side was given at least one `kind` and none of them is alive now. A dead unit
  stays in the table, so "had one" needs no second record, and a side that NEVER HAD the kind has not lost
  one -- which is what keeps a rig whose scenario spawns no recycler from reading as a defeat on tick zero.
  Derived, never stored, never hashed.
- [SHIPPED c5eb877, generalised in OBJ] `Sim::war_outcome()` asks that predicate once per entry of the
  scenario's condition list, in order, and returns the verdict of the first entry that fires -- `Undecided`
  when none has. The built-in list is the recycler pair, the player's own loss first, so a pair that dies
  together reads as a loss; a list with no entries is a sandbox that is never decided. A battle nobody asks
  about produces exactly the state it did before this existed: the list is derived, never stored, never
  hashed, and the measure baseline is byte-identical across this change.
- [SHIPPED c5eb877] The HUD banner reads `war_outcome()`, emits nothing while undecided (so the vertex
  tail is unchanged for a battle still being fought), and is stated from the player's chair, so a viewer on
  the enemy side reads it reversed.
- [SHIPPED c5eb877] Destroying the FACTORY does not end the game, and now that is by design rather than
  by omission: the factory is not the unit the ruling's condition names. A factory can be razed and the war
  carries on until a recycler dies.
- [INTENDED — status unverified] Whether the "production building" of the ruling IS the factory. The
  condition is on the recycler today, which is what the scenario places and what BZ2's deployed recycler
  becomes; the brief does not say the factory and the deployed recycler are the same thing (section 14,
  question 2 of 14.1).

PROVENANCE: `src/sim/sim.cpp:944-967` (the two predicates),
`include/protekt/sim/outcome.hpp:1-11`, `include/protekt/sim/sim.hpp:143-162`,
`src/game/scene.cpp:2318-2360`, `tests/test_scavenger.cpp:406-520`, `tests/test_scene.cpp:5253-5560`,
`docs/PLAN.md:494-495,600-605`, commit messages of `e23dec8` and `c5eb877`, Harri 2026-10-24 (brief).

## 11. Sound

- [SHIPPED 67ea954] Every sound is synthesised from arithmetic: a seeded noise, a decay and a sine. There
  are no assets and nothing to license. Three sounds exist: `YourShot`, `HitTaken`, `Explosion`.
- [SHIPPED 67ea954] **What is heard is driven by the sim's report of what happened.** The pipeline
  diffs the sim's unit table before and after each tick (`events_between`) and a pure rule,
  `sounds_for`, turns the events into voices. The sim never learns sound exists.
- [SHIPPED 67ea954] The trigger is no exception: pulling it raises a flag and the pipeline judges it
  against the sim's own `Shot` record. A trigger the sim declines (still reloading, turret not
  mounted) is silent and the notice says why (`audio_pipeline.cpp:41-54`).
- [SHIPPED 67ea954] Your own gun is full volume, dead centre. A friend's is the same while within
  60 tiles.
- [SHIPPED 3c7070c] Being hit is a full-volume low thud with a sharp attack, dead centre, whichever way the
  shot came, because the thing hurting you is not something to locate.
- [SHIPPED 67ea954] A hostile shot that hurt nobody of yours is heard as a gun, quieter with distance
  and panned left or right. Dead ahead and dead behind sound alike.
- [SHIPPED 67ea954] A death is an explosion: yours at full volume, anyone else's quieter with distance
  and carrying 90 tiles.
- [SHIPPED 67ea954] **Beyond earshot, nothing.** 60 tiles for a shot and 90 for a death: not quiet,
  absent. It is the radar's promise made to the ears: a battle you cannot perceive must not be audible.
  Gain falls with the square of distance to exactly zero at the earshot.
- [SHIPPED 67ea954] No sound device, or `--no-sound`, runs silent; a missing device is reported on one line
  (`src/app/game.cpp:638-645`).

PROVENANCE: `include/protekt/game/sounds.hpp:11-38`, `src/game/sounds.cpp:42-142`,
`src/app/audio_pipeline.cpp:25-82`, `include/protekt/audio/synth.hpp:20-25`, `src/app/game.cpp:167`,
`README.md:150-172`.

## 12. Matching BattleZone II

- [DECIDED — not built] PROTEKT follows BattleZone II in controls and in look and feel. Harri, standing
  (brief). Anything about BZ2 is MEASURED from Harri's captures, never remembered.

What has been measured, and what is ours:

| subject | the number | measured against | status |
|---|---|---|---|
| HUD glyph height | about 10 px; cell 2.0 on a 5 by 5 grid at 720 high | BZ2 originals at 800 by 600 and a 1280 by 730 remaster screenshot, counted (`scene.cpp:2152-2154`) | [SHIPPED bd32f0b] |
| Radar geometry | a wide shallow oval; a ground disc seen from 21° above, so height is sin 21° = 0.358 of width; width 0.258 of the frame | "pixel-counted off BZCC screenshots" (`radar.hpp:37-39`) | [SHIPPED f8e65de] |
| Radar size clamp | 200 to 480 px | no measurement cited; ours | [SHIPPED f8e65de] |
| Radar radius | 40 tiles | ours; no BZ2 figure cited | [SHIPPED include/protekt/game/radar.hpp:34] |
| World map footprint | the radar's major diameter | Harri 2026-10-23, quoted at `world_map.hpp:35-40`; it replaced an earlier 12.7% of frame width taken from BZ2's minimap | [SHIPPED 43c1807] |
| World map key | `R`; `O` is the objectives key | BZ2's own keys (`bindings.hpp:143-146`) | [SHIPPED 20f224e] |
| Command row | fixed key spots; an empty key is not drawn and nothing closes up; `F` keys hold vehicle groups | BZ2 capture, Harri 2026-10-24 (`row.hpp`); it replaced the 2026-10-22 key pool | [BUILT, ROW-SLOTS] |
| Command menu | rows, verbs, costs, the gap, the top readout, the dimming | BZ2 capture 2026-10-24; a quotation (section 6) | [SHIPPED 3e799a0] |
| Ship instrument | a segmented C ring with hull and ammo, weapons beside it | BZ2's gauge; one fraction the brief gave "measured wrong" and was corrected (`scene.cpp:1928-1935`) | [SHIPPED 341fbb3] |
| Roster strip plate | 88 by 34 px, 6 px apart, 5 px corners | no measurement cited beyond the glyph height; ours | [SHIPPED bd32f0b] |
| Panel translucency | 0.65 alpha face | "which is how BZ2's panels read"; no figure measured | [SHIPPED bd32f0b] |
| Control scheme as a whole | WASD plus mouse look, right click weapon switch, number and function keys | not recorded as measured anywhere in the tree | [INTENDED — status unverified] |

PROVENANCE: `include/protekt/game/radar.hpp:34-48`, `include/protekt/game/world_map.hpp:34-48`,
`include/protekt/input/bindings.hpp:143-146`, `include/protekt/game/row.hpp:9-34`,
`src/game/scene.cpp:1928-1935,2145-2183`, `tests/test_menu.cpp:1-10`, commit message of `bd32f0b`,
`README.md:3-4`, Harri standing (brief).

## 13. The build

- [SHIPPED d79f923] `tools/ci.sh` is the single definition of green. It has two tiers, and the hooks
  name a tier instead of repeating its stages.
  - **fast** (`pre-commit`): `format build tests`.
  - **full** (`pre-push`): `--require-clean tree format kitprobes build tests release version asan tsan
    tidy pristine`.
- [SHIPPED d79f923] There is no external CI. The hooks are armed once per clone with
  `git config core.hooksPath .githooks`. Per-machine settings go in `.ci.env`, which is gitignored.
- [SHIPPED d79f923] `pristine` builds a clean clone of what git actually carries, the only way to catch a
  file that passes only because it is on disk.
- [SHIPPED d79f923] Edits to `tools/ci.sh` must be explained in `INCIDENTS.md` (its header says so).
- [SHIPPED d79f923] The toolchain floor is CMake 3.20, C++20, clang or gcc, GoogleTest. `clang-format`
  and `clang-tidy` for the full tier. Vulkan and SDL3 optional.
- [SHIPPED d79f923] Code is written to `CODING_STANDARDS.md`: C++20, `-Wall -Wextra -Wpedantic -Werror`, no
  raw owning pointers, TDD, fuzz targets are clients, measure before claiming, and the determinism and
  layering rules. This document points there rather than restating it.
- [SHIPPED 2ea1cef] The honest gap: GUI tests are fragile. The window itself cannot be exercised on the
  gate box, which has no display. The loop, the world view and the player commander are tested headless
  and byte-compared, but the window, the swapchain hand-off and the real device are verified by hand on the
  play machine. One display-path fault (red and blue swapped) passed the whole suite
  (`README.md:127-136`), which is why the `C` calibration view exists.
- [SHIPPED 3c7070c] Vulkan tests are skipped under ThreadSanitizer because lavapipe races inside itself
  (`docs/PLAN.md:233-236`).
- [DECIDED — not built] The laptop sync is retired: the laptop is pulled and built by Harri, not synced. Harri
  2026-10-24 (brief). Process; no code.

PROVENANCE: `tools/ci.sh:1-40`, `.githooks/pre-commit`, `.githooks/pre-push`, `README.md:293-307`,
`CODING_STANDARDS.md:23-66`, `INCIDENTS.md:1-20`, `docs/PLAN.md:228-236`, `CMakeLists.txt:7,22`,
Harri 2026-10-24 (brief).

## 14. Status questions

This section is a deliverable. Three kinds of entry: rules whose status I could not verify, drift between
a document and the code, and places where the documents contradict each other. Nothing here is fixed.

### 14.1 Rules I could not classify

1. **D3b — is the game complete with no AI loaded?** The sim is (no `ai/` include below `app/`). The
   playable game builds `ai::ScriptedCommander enemy_` and calls it every frame
   (`src/app/game.cpp:374,503`); no flag removes it. Is D3 a rule about the sim only, or about the game?
2. **"A menu is a MODE."** `menu.hpp:40-43` states it as a property of the reference menu. Nothing routes
   the digits, and `Tab` clears the selection rather than closing a mode. Do you want the mode, and if so
   is it RB2?
3. **Which building is the "production building"** whose destruction wins (section 10)? The factory is the
   only unit that builds, and the menu treats it as the deployed recycler. Is that the same thing, and is
   a second kind of building coming?
4. **Strategic controllers** (`docs/PLAN.md:309-345`) are called a "design sketch" and the module was
   deleted. Requirement, or parked idea?
5. **Which HUD elements are exempt from the 10 px type scale?** The weapon label (4.8), the mortar ladder
   digits (4.0), the ship readings (3.0) and the `DEPLOYING` plate (3.0 at 450) are larger than cell 2.0.
6. **(Moot since ROW-SLOTS: the pool is gone.) The roster-key ruling had two wordings.** `roster.hpp` (now `row.hpp`) records "when the last unit dies, the
   function key space gets returned to the pool" as Harri's of 2026-10-22; the brief gives 2026-10-24 for
   "a key returns to the pool". Is the later one a restatement or a new ruling? The code satisfies both.
7. **Mount and unmount time.** Harri 2026-10-17 (brief) gave 3 seconds for the mount. The unmount is also
   90 ticks in code (`720b7fd`), but the ruling as given covers only the mount.
8. **BZ2 control scheme as a whole.** Only the map key, the function-key pool and the objectives key are
   recorded as taken from BZ2. Are the remaining bindings (WASD, right-click weapon switch) also matched
   to a capture?
9. **Rulings that appear in no repo document.** Teams, free repair/energy/ammo, the build rule's wording,
   the key pool's 2026-10-24 date, dead units vanishing, the win/lose conditions, the 10 px rule, the menu
   quotation, the parked RECYCLE, follow radius and mount time, the laptop sync, and "scrap is the
   resource" dated 2026-10-16. The D-list of `docs/PLAN.md` has no scrap entry. Each is cited here as
   `(brief)`; please confirm or correct the date.

### 14.2 Drift: a document against the code

| # | document says | the code does |
|---|---|---|
| 1 **CLOSED** | `README.md:24`: `Space` adds a unit to the selection or removes it; `Shift`+`Space` does the same | `Space` never adds; with nothing selected it selects one unit, with something selected it follows a tank or attacks a hostile (`interaction.cpp:223-264`, `3404ba9`) |
| 2 **CLOSED** | `README.md:31,143`: `T` with the mark on one of your own units orders an attack on it | `T` with no hostile under the mark arms the attack list, own unit or not (`interaction.cpp:274-278`); only a hostile is attacked directly. The trigger can still hurt your own (`README.md:62`) |
| 3 **CLOSED** | `README.md:19`: `A`/`D` turn the body | for a driven `HoverTank` they strafe and the mouse turns the hull (`player.cpp:69-136`, `6023fd2`) |
| 4 **CLOSED** | `README.md:45-48`: "`Escape` and Stop are how you hold fire" | `Escape` clears the selection and cancels `G` and `T`; there is no Stop key (`interaction.cpp:203-205`). A unit under a `Stop` order still fires |
| 5 **CLOSED** | `README.md:92-93`: the radar is round, a disc with a ring border | it is a wide shallow oval (`radar.hpp:37-44`, `f8e65de`) |
| 6 **CLOSED; this row was wrong** | `README.md:103`: a nav mark is a "numbered plate" on the radar | the radar's nav indicator IS a 9 px plus, two crossed bars with no number; the number appears only on the world map (`scene.cpp`, verified in the DOCDRIFT pass) |
| 7 **CLOSED** | `README.md:116-125`: a health bar sits above the radar | retired by Harri 2026-10-23 (`scene.cpp:1433`); the hull is in the ship instrument |
| 8 **CLOSED** | `README.md:112-114`: the top strip shows "key, kind and hit points" | it shows key, count `x<n>`, kind and a group bar (`scene.cpp:2148-2158`) |
| 9 **CLOSED** | `README.md:193-194`: "No economy", "No way to win or lose"; `docs/PLAN.md:347-353`: the economy is not built | scrap, the scavenger, the ledger, the build and the C8 verdict are built (`e653f16`, `e23dec8`); the README gap table and the PLAN's "still owed" are stale |
| 10 **CLOSED** | `README.md:181`: "the enemy commander advances, engages and withdraws" | the playable game's enemy is `ScriptedCommander`, which reads no hit points and does not retreat; the utility commander is wired only into the CLI tools |
| 11 **CLOSED; this row was wrong** | `docs/PLAN.md:114-115`: six unit kinds | **eight**; `Scavenger` is the seventh and `Recycler` the eighth (`units.hpp:23-34`) |
| 12 **CLOSED** | `docs/PLAN.md:117-118`: roster strip `1`–`9` and world map `O` | `F1`–`F10` and `R`; `O` is unbound (`bindings.hpp:111-154`) |
| 13 **CLOSED** | `docs/PLAN.md:11,661-663`: audio is a non-goal and out of scope, and there is no pathfinding beyond steering | audio shipped (`67ea954`) and A\* shipped (`0eb1986`); PLAN §5 already says so, §8 and the header do not |
| 14 **CLOSED** | `docs/PLAN.md:661`: no save-game serialisation beyond seed plus order log | `docs/PLAN.md:570-607` records Harri's call to plan a save. The two are in the same file |
| 15 **CLOSED** | `docs/PLAN.md:634-655`: "Open decision (blocks M3 only)": which machine plays | M3 is done and the play machine is named at `docs/PLAN.md:207` |
| 16 **CLOSED** | `docs/PLAN.md:258` calls the build menu and the resource loop "later"; `:270` says "M4a, M4b and M4 done" | the economy is built; the build menu is only a drawn quotation |
| 17 **CLOSED** | `include/protekt/input/bindings.hpp:7` says "Movement only" | the file holds the command predicates too (`quit_requested` and others) |
| 18 **PARTLY CLOSED** | `README.md:26` and the refusal in `interaction.cpp:390` say `X` swaps only into your own *armed* units | `is_drivable` is every kind but the factory, so an unarmed scavenger can be swapped into (`units.hpp:47-49`) |

**Status: CLOSED by the DOCDRIFT commit (`git log --grep DOCDRIFT`).** Every row was re-checked
against the code before the document was touched. Rows 1-5, 7-10 and 12-17 were right and are closed.
**Two rows were themselves wrong.** Row 6: the radar's nav mark *is* a plus (`scene.cpp` draws two
crossed bars, about 9 px, no number), but the README said "numbered plate" and the number appears only on
the world map; the README now says plus. Row 11: there are **eight** unit kinds, not seven (`Recycler`
is the eighth, `units.hpp`); PLAN now says eight. Row 9's "C8 verdict" is stale as well: C8's attrition
verdict was retired (`c5eb877`) and the war ends on a recycler's death (`Sim::war_outcome`); the
documents say that. Row 9's "spawns no scavenger" line is superseded by `1f125f5` (HAULER): each side
gets one. Row 4 is stronger than the table: there is no hold-fire at all (auto-fire never reads
stance). **Row 18 is CLOSED** by the Q12 commit: `is_drivable` asks whether the hull MOVES, so the
`swap_to_crosshair` refusal now reads "...only swap into your own vehicles." and the scavenger, the
undeployed recycler and every armed kind are drivable while the factory and a deployed recycler are
not -- pinned by `Swap.XOnAnOwnScavengerDrivesItEvenThoughItIsUnarmed` and
`Swap.TheRefusalForAnImmobileBuildingNamesTheDrivingRuleAndNotArmament`.

### 14.3 Drift: a ruling against the code

19. ~~**The retired verdict is the shipped one.**~~ **CLOSED by `c5eb877` (OBJ-a)**: `side_has_lost`
    and its attrition rule are deleted, and the banner reads `war_outcome()`. Kept here as the record of
    what the register found. Section 10.
20. **One build per beat in total, not per side.** `sim.cpp:932-934`. Section 8.3.
21. **`buildable` is false for every kind, yet the factory builds a `Tank`.** `spec.hpp:36` and
    `test_roster.cpp:106` against `sim.cpp:929`, whose comment calls the tank "the only `buildable` kind".
22. **No scavenger in the skirmish.** The economy is inert in the playable game. Section 8.4.
23. ~~**The menu's `Recycler` subject is unreachable**, and every live-looking digit row (`3 Scavenge`,
    `0 Recycle`, …) does nothing.~~ **CLOSED in two steps**: the subject became reachable when the recycler
    kind and its spawn landed (`ddc6423`, `a584683`), and the rows that CAN act now act (`d80ce95` — `1`
    and `4`). The rows still inert are inert because the SIM has nothing for them to do (`3 Deploy` awaits
    RB3, the build rows await RB5, `2 Go to Nav` awaits a ruling), not because no code routes the digits.
24. **Names are not keyed by team**, and `Harvester` and `SCAVENGER` both appear on the player's side.
    Section 9.
25. **Menu costs are on another scale from the sim's.** The menu quotes HARVESTER 20 to SERVICE POD 2;
    the sim spends 4 loads on a tank. RB5 replaces them (`menu.cpp:59-62`).
26. **"Explode and vanish."** A dead unit vanishes, and is heard exploding, but nothing is *seen*
    exploding. Say if the visual is part of the ruling.

27. **The menu's top row does not know about a live selection.** §6.2 takes the row from the capture
    (`SPACE Select` when something of yours is under the cursor), but the code's `Space` means SELECT
    only from an EMPTY selection: with something already selected it FOLLOWS a vehicle or ATTACKS a
    hostile, and says so in the notice (`interaction.cpp:223-264`). The row therefore reads
    `SPACE Select` in a state where pressing `Space` would not select. Closing it needs a `MenuInput`
    field that says whether the selection is empty, and the two extra verbs — that is RB2's territory.

### 14.4 What I could not determine

- Whether `radar.cpp` draws the nav mark as a plus (drift 6) — I read the commit message only.
- The BZ2-capture basis for each menu row beyond `tests/test_menu.cpp`'s own statement that they are
  quotations; I have no access to the captures.
- Whether `scripted_commander.cpp` is meant to be the long-term enemy. It includes `utility.hpp` for
  helpers, and I did not trace it past its 44 lines.
- The wording of any ruling marked `(brief)`: I have the brief's paraphrase, not Harri's own sentence,
  except where a code comment quotes it (the ammo placeholder, the hull bar, the radar-diameter call).

PROVENANCE: every item above names its own `file:line`; the documents are `README.md`,
`docs/PLAN.md`, `include/protekt/input/bindings.hpp`, `include/protekt/game/menu.hpp` and
`include/protekt/game/row.hpp`.

## Part B — the reference, per entity and per menu

Section 0 to section 14 above are **Part A**: the numbered requirements and the drift findings, written
against `b3b153d`. Everything below is **Part B**: the REFERENCE behind them — what each entity is and what
each key and each menu row does, from the owner's own rulings and from the code. Part A is not restated,
renumbered or amended here; where the two touch, the Part A section is named rather than repeated. The
status markers, the provenance rule and the terms of section 0 apply to Part B unchanged, and every section
below ends with its own `PROVENANCE:` line. Line numbers in Part B are against `dc90293`.

Every entry says who a fact came from. A fact in the owner's words is quoted and dated. A fact read out of
the code names the commit that first carried it. A passage that the sources themselves mark as the
assistant's reading, or as waiting on the owner to confirm, is carried across as
`[INTENDED — unconfirmed]` and is never promoted to a ruling; section B.8 lists the ones deliberately kept
out of the sections above it for that reason.

PROVENANCE: `~/.hermes-workspace/.hermes/plans/protekt-hud-and-power.md` (the 2026-10-24 capture, the
command row and the POWER rulings), `~/.hermes-workspace/.hermes/plans/protekt-open-questions.md` (the
entries marked ANSWERED on 2026-10-24), `/home/harri/hermes-workspace/protekt-plan/slices.json` (the RECY,
OBJ, RB0-RB6, ROW-SLOTS, ROW-MOVE and BAR-LOOK notes), and the code read at `dc90293`.

### B.1 The command row

The top strip is fourteen keys — `~`, `1`, `2`, `3`, then `F1`..`F10` — and every one of them sits in the
same spot on the screen for the whole game: slot index 4 is `F1` and slot 13 is `F10`, fixed forever. The
row is the HUD's copy of a Battlezone: Combat Commander capture. HIS RULING, 2026-10-24:
*"Then ~ is for Nav locations that the player has set. 1 recycler, 2 factory, 3 armory, and then f1 to f10.
All of those keys are always in the same spot on the screen!"* [SHIPPED a15a059]

- [SHIPPED a15a059] `~` is the nav locations the player has set. It is drawn once at least one mark exists,
  labelled `NAVS` with the number of marks; it carries no hit points and no bar. `RowState::occupied` returns
  true for slot 0 only when `nav_marks > 0`, and the marks themselves are the game layer's `NavMarks`.
- [SHIPPED a15a059] `1` recycler, `2` factory, `3` armory are the reserved BUILDING keys, one per building
  KIND. In the tree today only `2` can ever fill — the `Factory` kind exists — and `1` (the deployed
  recycler) and `3` (the armory) have no kind yet, so those two keys are empty and therefore not drawn. That
  is the ruled behaviour, not a stub.
- [SHIPPED a15a059] `F1`..`F10` hold groups of VEHICLES, not kinds. One kind may hold several keys at once
  (his capture: turrets on `F1`, `F3` and `F4`; tanks on `F5` and `F10`), and one key may hold a single
  vehicle (a lone constructor on `F6`, `F7`, `F8`). Today every vehicle is its own one-unit group, because
  nothing builds a batch yet; `row.hpp:31-33` leaves the batch seam as a comment and not as scaffolding.
- [SHIPPED a15a059] A key that holds nothing is NOT drawn — no dim ink, no placeholder, no zero — and the row
  does NOT close up. His own example: turrets under `F2` wiped out leave `F1` tanks, a blank space at `F2`
  ("now available for the next vehicles that you want to build") and `F3` scavengers. The slot keeps its
  index; the space stays blank.
- [SHIPPED a15a059] A freed key is reused by the next units built, filling a gap in the middle before
  anything after it: a vehicle not seen before takes the first empty group slot, lowest index first. His
  words: *"If all tanks under F3 get destroyed, and I then build turrets, and F3 is the first available slot,
  that's where they go."* A unit already assigned keeps its slot, and loses it only when its whole group is
  gone.
- [DECIDED — not built] A shift-built batch shares ONE key, while each lone build takes its own. HIS RULING,
  confirmed by him in as many words on 2026-10-24: *"Your inference is CORRECT: shift-built batch shares one
  [F key] while a lone build takes its own [F key]."* It needs a batch id that the sim does not record yet;
  `row.hpp:31-33` is where it will arrive.
- [SHIPPED a15a059] No buildings ever under an `F` key. HIS RULING, 2026-10-24: *"There are never any
  buildings under F keys. They are always vehicles."* Buildings live on the numbered keys alone, and only
  `1`, `2` and `3` carry a number — *"the OTHER buildings don't carry a number, such as barracks and air
  hanger."* Barracks and a hangar do not exist in the tree; they are [DECIDED — not built].
- [SHIPPED a15a059] An UNDEPLOYED recycler may sit under any `F` key, because it is still a vehicle: HIS
  RULING, 2026-10-24 — *"An un-deployed recycler can sit under any F key. It doesn't matter. In the official
  games, they seem to have always put them under F1."* So there is no display rule for it. The shipped spawn
  appends it last on each side (`a584683`), so it lands on `F7` today.
- [DECIDED — not built] On deploy the recycler leaves the `F` row and moves to key `1`. HIS RULING, read as
  the recycler and confirmed by him the same day: *"when a [recycler] is deployed, it must move from F1 to
  1"*. Until the deploy sequence exists (RB3) the recycler stays a vehicle in the `F` row, which is what
  `row.hpp:18-19` records.
- [SHIPPED a15a059] A drawn key shows the kind's name, how many vehicles the group holds (as `x<n>`), and one
  health bar: the group's summed hit points over its summed ceilings, filled in proportion and tinted green
  above 60 per cent, yellow above 30, red below. The ink is the group's, not a unit's.
- [DECIDED — not built] HIS RULING, 2026-10-24, CHANGES that bar: *"Under each function key, there is one
  health bar for each individual vehicle. They vary in COLOUR, not length. Bright green is completely
  healthy, then yellow, red, and black, the lower the health. When a vehicle is destroyed, its health bar is
  removed."* So a key holding three tanks shows three bars, each bar a band colour rather than a shrinking
  length, and a dead vehicle's bar is gone rather than drawn black or empty. The shipped bar (above) is a
  single varying-length group bar, so this is a model change as well as a drawing change; `SlotReading`
  exposes only the two sums today. The band cut-points are UNRULED — the default on record is green above
  two-thirds, yellow above one-third, red above zero, black at the lowest living band — and the bar order
  under a key is the group's own spawn order, left to right. That is BAR-LOOK.
- [DECIDED — not built] SHIFT-MOVE BETWEEN KEYS (ROW-MOVE). HIS RULING, 2026-10-24: with a selection in hand,
  shift and a function key MOVES the selected vehicles into that key — the target keeps its units and gains
  them, the source key goes blank if it is emptied, and nothing else in the row moves. A target already
  holding a DIFFERENT kind is REFUSED: *"Yes, refuse if not the same type of vehicle."* The refusal belongs on
  the notice line under the strip, which already prints the last refusal. Within a key, order stays spawn
  order. Two consequences on record, neither a blocker: the row stops being a pure function of the sim, so an
  observer needs the input stream to reconstruct the player's grouping (replays carry input, so replay
  fidelity survives); and the slice needs the unit-to-slot assignment to be mutable state, which it is.
- [SHIPPED a15a059] The memory is in the GAME layer (`RowState`): the sim has no new field, no tick reads it
  and no hash depends on it, so the measure output is byte-identical to what it was before the row changed.
- [INTENDED — unconfirmed] The row's PLACE on the screen is not settled. His capture carries the whole row as
  one TOP bar, while PROTEKT's HUD draws the command menu down the LEFT; only one of those can be the target,
  and the source records it "so the HUD layout pass asks rather than assumes". No ruling of his picks one.

PROVENANCE: `protekt-hud-and-power.md` sections 4, 5, 6 and 7 (Harri 2026-10-24, quoted; section 7 is his
ruling that changes ROW-SLOTS's drawing); `protekt-open-questions.md` Q6, Q6.1, Q6.3 (answered 2026-10-24);
`slices.json` ROW-SLOTS, ROW-MOVE, BAR-LOOK; code plus commit — `a15a059` (`include/protekt/game/row.hpp:12-92`,
`src/game/row.cpp:22-113`, `src/game/scene.cpp:2537-2606`), `1832d55`, `dc90293`, and `a584683` for the
recycler's spawn order.

### B.2 The recycler

The mobile producer that deploys into your base — the alien one is the MATRIARCH, a name now and a look
later. His ruling, 2026-10-24: ONE recycler per side, EVER [DECIDED — not built].

- [SHIPPED ddc6423] `UnitKind::Recycler` is the sim's eighth kind, APPENDED (value 7) so that every kind's
  index, and every hash, replay and observation that carries one, is unchanged by its arrival. It is
  unarmed (range zero, and the whole ballistics block with it) and it is drivable: it is the same case as the
  hauler, unarmed but it moves, so an order from the player outranks anything it would otherwise do. It is
  labelled `RECYCLER` in the row, and a selected recycler asks for `MenuSubject::Recycler` — the subject that
  was modelled in RB0 and that no kind could reach until this landed.
- [SHIPPED ddc6423] Its hp and speed are BORROWED and not designed: the tank's hull, so a base does not die
  too easily, and the hauler's speed, so it can reach a deploy site. Its own numbers are Harri's to set in a
  balance pass; `spec.hpp:102-107` says so in the table.
- [SHIPPED a584683] Each side spawns exactly one, the LAST id on its side: appended last to the roster and
  the spawn offsets at `{-3,-3}` from that side's base centre and mirrored for the enemy. At seed 42 that is
  the player's `(22,42)` and the enemy's `(150,169)`, neither on the factory's tile.
- [SHIPPED c5eb877] Its death is the LOSE CONDITION: an enemy recycler dead means the player has won whatever
  else the enemy still has, and the player's own dead means the player has lost. The condition is the derived
  predicate `Sim::kind_wiped_out(side, kind)`, read through `Sim::war_outcome()`; destroying the factory does
  NOT end the game. Since OBJ the condition is one ENTRY in the scenario's list rather than the whole rule,
  and the predicate counts BOTH FORMS of the unit -- the vehicle and the building it deploys into -- because
  deploying changes the kind of the same unit and a war that watched the vehicle alone would stop watching a
  deployed base. See Part A section 10. A dead recycler's tie with a dead one of the enemy's — both dead —
  reads as a LOSS, which is the executor's choice and NOT covered by any sentence of his; it stays open.
- [SHIPPED a584683, c5eb877] It STARTS AS A VEHICLE and must deploy: a vehicle in the `F` row, drivable,
  unarmed, one per side, whose destruction decides the war. The enemy's starts the same way and deploys by
  script — a scripted opening behaviour, not something emergent.
- [DECIDED — not built] The DEPLOYED form is a NEW KIND, not a flag on the recycler. HIS RULING, 2026-10-24:
  *"If I was the one doing the programming, I think I would make everything easier on myself by making a
  deployed recycler a new kind of entity, different from a non-deployed recycler. A non-deployed recycler is
  a vehicle. A deployed recycler is a building. Plus, those two look different. So no flag. A new kind."*
  The kind is named `DeployedRecycler`, APPENDED (8). The reason is a CATEGORY change, vehicle to building,
  and the models differ too; it also settles the key question by construction, because the reserved keys are
  per kind and a building is not a vehicle. The sim has eight kinds today and `Recycler = 7` is the latest.
- [DECIDED — not built] The deploy flow is the F1 -> 3 -> SPACE gesture: select the recycler (its function
  key, or point at it and press SPACE), press `3 Deploy` from its menu to ARM the deploy, point at a good
  spot, then press SPACE to CONFIRM. `3` is not a second binding for SPACE: it arms, SPACE confirms, and the
  menu's top row becomes a live readout of the armed step. Of that menu only `1 Follow Me` and `4 Hold` route
  today (RB-DIGITS); `3 Deploy` is inert and says why.
- [SHIPPED 0099c53] The deploy's WAIT is visible: while a unit's `deploy_ticks` is non-zero it wears
  a second bar right above its health bar, in its side's colours, filled `deploy_ticks / kDeployTicks` from
  the left (paint only; the same reach and radius cull as the unit). No count, no bar. The rising building
  is not part of this.
- [SHIPPED 3e799a0, ddc6423] The undeployed recycler's menu is six rows, in drawn order: `SPACE` (the state
  row), `TAB Close Menu`, `~ Rotate Buildings` (dim — it does not apply until there are buildings to rotate),
  `1 Follow Me`, `2 Go to Nav`, `3 Deploy`.
- [DECIDED — not built] Its deploy site: deployable ground is a TERRAIN TYPE and not a slope — his words,
  *"turn one of the ground types to 'flat enough for deploying on'"* — delegated to the executor and decided
  as `terrain == Flat` behind ONE predicate, so the ghost's green and red, the sim's deploy gate, the
  builder's site check and the turret gate all read the same function, and a real `Deployable` terrain value
  is a one-line change plus art later. That is RB2; the ghost that draws it is RB4.
- [DECIDED — not built] 10 SECONDS to deploy, and the deploy time is a per-entity CONSTANT with no global
  number. HIS RULING, 2026-10-24: *"I'm thinking 10 seconds to deploy. It should probably be different for
  different entities. Recycler, turrets, scavengers over a biometal pool, factories, armories... I think they
  should all have their own specific constants for how long it takes to deploy one."* So the recycler's is
  10 s (300 ticks at the sim's 30 ticks a second), and the turret, the scavenger over a biometal pool, the
  factory and the armory each carry their own — the shape `spec.hpp` already has for hp, speed and range, so
  it costs a table and not a mechanism.
- [DECIDED — not built] Ruling-adjacent, from the same answer: factories and armories are raised by the
  BUILDER on the alien side and the CONSTRUCTOR on the human side; more buildings are coming (barracks, and
  maybe an air hangar); and buildings carry no control numbers of their own — they only unlock things.

PROVENANCE: `protekt-open-questions.md` Q5, Q5b, Q6, Q6.1, Q6.3 and the Q5 caveat (all Harri 2026-10-24,
quoted); `protekt-hud-and-power.md` sections 4, 5 and 7; `slices.json` RECY, RB0, RB2, RB3, RB4, RB5, OBJ;
code plus commit — `ddc6423` (`include/protekt/sim/units.hpp:23-38`, `spec.hpp:102-107`,
`src/game/menu.cpp:118-135`), `a584683` (`src/sim/scenario.cpp:36-64`), `c5eb877` (Part A section 10),
`3e799a0`, `d80ce95`.

### B.3 The scavenger / harvester

One sim KIND with per-faction names and looks: a scavenger and a harvester are the SAME vehicle, identical
in function, differing only in name and appearance — and the difference follows the FACTION, never the
player or the enemy side. HIS RULING, 2026-10-24 (Part A section 9) [DECIDED — not built].

- [SHIPPED e653f16] `UnitKind::Scavenger` is the sim's seventh kind (value 6, appended, so no recorded value
  moved). It is unarmed — range zero is what makes it unarmed everywhere in the sim, in firing, targeting
  and the AI's threat list — and light on purpose: three fifths of a tank's hit points and three quarters of
  its speed, because a hauler is meant to be worth escorting, not worth arming.
- [SHIPPED e653f16] With no orders of its own it runs the fetch: walk to the nearest piece of wreckage, take
  one load within 1 tile, carry it to its own side's lowest-id living factory, hand it over within 2 tiles.
  Any order from the player or an AI replaces the fetch. Pick-up and hand-over happen wherever it is, even
  under the player's own driving.
- [DECIDED — not built] Its name and its appearance are PER FACTION: `Harvester` on the alien side,
  `SCAVENGER` on the human side. The name follows the faction and not the side, so the sim's one kind wears
  two names. Today the code is not keyed by faction at all: the kind's label is `SCAVENGER`
  (`src/game/row.cpp:94-95`) and the one shipped build list says `Harvester` (`src/game/menu.cpp:63-67`),
  both on the player's side. Part A section 9 records the drift.
- [DECIDED — not built] A DEPLOYED scavenger is selectable by NO key at all. HIS CORRECTION, 2026-10-24:
  *"When a scavenger is deployed, as opposed to a recycler, there is no key that can select the deployed
  scavenger."* Its first sentence had said "scavenger" where he meant the recycler, and he confirmed the
  recycler reading — so the deployed RECYCLER owns key `1` while the deployed SCAVENGER owns no key.
- [DECIDED — not built] The scavenger's deploy site is a BIOMETAL POOL, not level ground: it is one of the
  entities Q6.1 names as carrying its own deploy constant, and the same answer puts the recycler on level
  ground and the scavenger over a pool. So the deploy SITE predicate is per kind, and only the gesture is
  shared. The pool itself is BZ2's resource node; nothing in the tree resembles it.
- [DECIDED — not built] Sources: *"Scavengers may collect the scrap for re-use"* — and each power building
  provides 3 units of power and taking one away costs 3 (B.5).
- [SHIPPED 3e799a0, d80ce95] Its menu (subject `Harvester`) is drawn in this order, keys not contiguous:
  `SPACE` (the state row), `TAB Close Menu`, `~ Select Unit` (dim — it does not apply to this selection),
  `1 Follow Me`, `2 Go to Nav`, `3 Scavenge`, `4 Hold`, `5 Pick Me Up` (dim), a visible GAP where 6, 7 and 8
  would be, `9 Bailout`, `0 Recycle`. Of these only `1` and `4` act today; every other row is inert and says
  why (`3 Scavenge` because the fetch is automatic, `0 Recycle` because it is parked).
- [SHIPPED d80ce95] `4 Hold` is `GuardAt` at each unit's OWN position, and deliberately not `Stop`: `Stop`
  idles a unit and a harvester's fetch re-picks scrap on the same tick, so it would hold nothing.

PROVENANCE: `protekt-open-questions.md` Q6.1 and Q6.2 (Harri 2026-10-24, quoted);
`protekt-hud-and-power.md` section 3 item 0 (his correction) and section 5; `slices.json` RECY, RB5;
`src/game/menu.cpp:41-51,63-67,118-135`, `src/game/row.cpp:80-100`, and commits `e653f16` (the kind and its
fetch), `3e799a0` (the row model and the menu), `d80ce95` (`Follow Me` and `Hold` route).

### B.4 The factions and the production lists

Two factions, HUMAN and ALIEN, and the player chooses which to play. HIS RULING, 2026-10-24: with the map,
*"They may also want to choose the side, either the human side or the alien side."* Side choice and map
choice are menu and setup concerns; the C-series HUD has no map or side picker. [DECIDED — not built]

- [DECIDED — not built] The production lists, per FACTION, keys 1 to 7, exactly as he gave them on
  2026-10-24: *"On the alien side, harvester: 20, scout 45, guardian 40, hauler 30, builder 40, healer 50,
  service pod 2. On the human side, scavenger 20, scout 50, turret 40, tug 45, constructor 40, service truck
  50, service pod 2. Those are the correct names for each side! I just got them from screenshots that I made.
  I haven't shared them with you, but I will if you want. Of course the keys go from 1 to 7."*
- [DECIDED — not built] His list ORDER IS THE ROW ORDER — read back to him for confirmation, because a
  mis-ordered menu is a real bug.

  | key | alien | cost | human | cost |
  |---|---|---|---|---|
  | 1 | harvester | 20 | scavenger | 20 |
  | 2 | scout | 45 | scout | 50 |
  | 3 | guardian | 40 | turret | 40 |
  | 4 | hauler | 30 | tug | 45 |
  | 5 | builder | 40 | constructor | 40 |
  | 6 | healer | 50 | service truck | 50 |
  | 7 | service pod | 2 | service pod | 2 |

- [DECIDED — not built] The SAME ROLE differs across the sides in name and sometimes in cost: the scout is
  45 alien and 50 human; the hauler is 30 alien and the tug 45 human; the harvester is the alien scavenger
  and the human turret is the alien guardian. `service pod` at 2 is the only item both sides name
  identically.
- [DECIDED — not built] The NAME FOLLOWS THE FACTION, never the player or the enemy side. This also settles
  which faction an earlier capture was playing: it showed `Harvester 20`, and harvester is the ALIEN name,
  so that capture was the alien side. So the shipping code's list is not keyed by faction (below).
- [SHIPPED 3e799a0] What the code has today: ONE build list, and it is handed out by `Side::Player`
  (`build_items`), i.e. the alien list of seven rows — `1 Harvester 20`, `2 Scout 45`, `3 Guardian 40`,
  `4 Hauler 30`, `5 Builder 40`, `6 Healer 50`, `7 Service Pod 2` — drawn as a QUOTATION of the reference
  capture, with the cost composed onto the row and weighed against the sim's real ledger. Because both the
  numbers and the faction keying differ from the ruling above, this table is exactly what RB5 and the
  per-side roster replace.
- [SHIPPED 3e799a0] The ENEMY side's build list is EMPTY, and deliberately so: inventing one "would put names
  in the mouth of a capture that never showed them". A deployment on the enemy side therefore draws no build
  rows.
- [SHIPPED 3e799a0 for the table; DECIDED — not built for the data] There is no per-kind cost in the tree.
  `UnitSpec::buildable` is false for every kind and has no consumer; the only flat number in the sim is
  `kRecycleOre` (2 loads), on another scale entirely from HARVESTER 20 and SERVICE POD 2. A build menu needs
  `cost` as per-kind DATA and the item NAMES as per-faction DATA — an added field plus the R3 roster, and
  neither exists yet.
- [DECIDED — not built] From the same answer as the deploy durations: factories and armories are raised by
  the BUILDER (alien) and the CONSTRUCTOR (human), and buildings carry no control numbers of their own; they
  only unlock things, such as new vehicles that can be built.
- [DECIDED — not built] The BUILDING NAMES follow the faction too, as the vehicle names do. HIS INFO,
  2026-10-25, relayed from a Gemini lookup of Battlezone II sources: the alien team's mobile producer is the
  MATRIARCH and keeps that name when deployed, exactly as the human RECYCLER does; its production building is
  the KILN; its armory is the DOWER; its power provider is the POWER LUNG; its comm tower is the ANTENNA
  MOUND; its gun tower is the GUN SPIRE. The human team's equivalents are Recycler, Factory, Armory, Power
  Generator, Comm Tower and Gun Tower. So "foundry" is not a Battlezone II name and should not be used.
- [INTENDED — unconfirmed] Two alien buildings upgrade in place: the Kiln into the FORGE and the Antenna Mound
  into the OVERSEER ARRAY. PROTEKT has no building-upgrade mechanism — no cost, time, key or changed
  behaviour — so this is reference carried across, not a ruling. If upgrades are wanted, they are their own
  slice.
- [DECIDED — not built] The name table is keyed by (kind, faction), matching the vehicle-name rule in
  section 9. The matriarch is the recycler's ALIEN NAME, not a separate kind.

PROVENANCE: `protekt-open-questions.md` Q6.2 and Q6.1 (Harri 2026-10-24, quoted, with the read-back table);
Harri 2026-10-25 (relayed Battlezone II reference: Matriarch, Kiln/Forge, Dower, Power Lung, Antenna
Mound/Overseer Array, Gun Spire);
`slices.json` RB5, RECY; code plus commit — `3e799a0` (`include/protekt/game/menu.hpp:90-96,140-152`,
`src/game/menu.cpp:59-67,113-116`), `include/protekt/sim/spec.hpp:35-36,110-112`, and Part A sections 6 and 9
for the drift.

### B.5 Power

A new subsystem: a cost of the BUILT world, counted in units of 3 per provider, spent by buildings that
need it, and clawed back when the provider dies. Nothing in the sim has anything like it: a case-sensitive
search for `power` over `src/` and `include/` finds nothing at `dc90293`, the one case-insensitive hit being
an unrelated comment about equal-power panning in the audio mixer. It must not be bolted onto the scrap
ledger. [DECIDED — not built]

- [DECIDED — not built] HIS RULING, 2026-10-24: *"The first one that looks like a lightning is the number of
  'Power' units that the player has available. To get power units, the player must build power buildings.
  Each power building provides 3 units of power. If a power building is destroyed, you lose the 3 power
  units. Power units are required to power some buildings like gun towers or gun spires. I'll verify the
  names later..."* So: EACH POWER BUILDING PROVIDES 3 UNITS OF POWER, and LOSING THE BUILDING COSTS THE 3
  back.
- [DECIDED — not built] The number on the screen is the SURPLUS, and consumers draw DIFFERENT amounts. HIS
  RULING, the same day, which retires the earlier `+1` puzzle: *"At that point in the game, I had built
  several power buildings and several buildings that consume the power. The buildings that consume power may
  consume different amounts of power. For example, if I have +3 of power available, and I build a new
  guntower, I will then have +2 of power left. The only relationship between scrap and power is that it takes
  scrap to BUILD a power building. When any building is destroyed, scrap is scattered in the vicinity.
  Scavengers may collect the scrap for re-use."* Read as: surplus = capacity from providers minus draw from
  consumers; `+1` on the capture was consistent all along, and the note that it failed to reconcile with
  "3 per building" was the assistant's error, not the capture's.
- [DECIDED — not built] Scrap is needed only to BUILD a provider. Power and scrap touch at construction time
  and nowhere else, and a destroyed BUILDING, like a destroyed vehicle, scatters scrap on the ground for a
  scavenger to collect (Part A section 8.1 has the wreckage rules).
- [INTENDED — unconfirmed] That power is a CAPACITY (`providers grant`, `consumers draw`) rather than a
  currency spent once, and therefore needs its own slice rather than an extension of the scrap ledger, is the
  assistant's consequence from his ruling and not a sentence of his. The provider and consumer names are his
  to verify, as he said: the power buildings, the gun towers and the gun spires. Which icon is which
  building is unread (only `R` and `A` are legible in the capture), and whether the powered buildings carry
  per-faction names too is the assistant's guess from the production-list pattern — not a ruling.

PROVENANCE: `protekt-hud-and-power.md` section 1 (his ruling, quoted and dated 2026-10-24) and section 5
(his clarification, quoted, which retires section 3 item 2's puzzle); section 3 items 1, 3 and 4 are the
unconfirmed readings carried into B.8. No code: `search_files` for "power" over `src/` and `include/` is
empty at `dc90293`.

### B.6 The economy and the two modes

Scrap is THE resource (Part A section 8). Two game modes are ruled [DECIDED — not built], and Instant
Action is the specification of what is being built.

- [DECIDED — not built] 40 SCRAP PER FACTION, ALWAYS, IN INSTANT ACTION. HIS RULING, 2026-10-24: *"Yes to
  the 40 of scrap for each faction. Let's keep that as always for Instant Action mode. Story mode depends on
  the mission, which may sometimes have none."* The consequence on record: 40 is an INSTANT-ACTION scenario
  constant and not a sim constant, because Story mode must be able to pass a different number through the
  same seam the objective uses — including zero. The sim's ledger (`Sim::ore`) starts at 0 today, so this is
  exactly the 40 the HUD does not yet read.
- [SHIPPED a584683, in part; DECIDED — not built, in part] INSTANT ACTION, his words: *"In instant action,
  it's always the same: each side gets a non-deployed recycler, some scrap, some vehicles. Instant action
  games take place in one of several maps. The player chooses the map. They may also want to choose the side,
  either the human side or the alien side. The game is played until one side loses their recycler."* What is
  built is the fixed shape of the sides: both spawn one non-deployed recycler and the same nine other
  vehicles, the scenario a pure function of the seed. What is NOT built is "some scrap" (the ledger starts at
  0), the map choice and the side choice.
- [DECIDED — not built] STORY MODE, his words: *"In story-mode, there is a series of games played in
  sequence. Each game has a mission. A mission can be defeating the enemy, escorting a vehicle without it
  being destroyed by the enemy, etc. Let's have story-mode closer to the very end, but keep it in mind so
  that we don't lock ourselves out of the possibility of implementing it easily. Right now we are
  concentrating on the instant action mode."* So Story mode is deliberately LAST, and nothing built in the
  meantime may assume mission scripting exists or lock the possibility out. Part A section 10 records that
  the DATA type and its consumer do not exist yet.
- [SHIPPED c5eb877, the seam] The objective is mission-level and not sim-level: the sim should end up
  answering "is this objective met", with Instant Action passing in the recycler objective. HIS RULING, from
  Q5's caveat: *"If we are in story-mode, there may or may not be a recycler in any form -- it depends on the
  mission."* So the win condition cannot be a hardcoded recycler rule forever; today it does not need to be,
  because `war_outcome` is DERIVED from the units and never stored, and a mission can supply its own
  objective through that same seam. That door is open and must stay open -- OBJ walked through it: the
  conditions are the scenario's own data now, and Instant Action is the scenario that passes in the
  built-in recycler pair.
- [SHIPPED b587d90] Win and lose are a LIST of conditions, not one hard-coded rule, and "the enemy
  recycler is dead" is one entry in that data list. HIS RULING, 2026-10-24 (Part A section 10, and OBJ).
  The entry is `sim::Objective{verdict, side, kind}` -- when every unit of `kind` that `side` was given is
  gone, the war is `verdict` -- the list rides on the scenario (`Scenario::objectives`, defaulted to
  `sim::built_in_objectives()`), and `war_outcome` asks the entries IN ORDER and returns the first that
  fires. A `ScenarioData` that mentions no conditions plays the built-in pair, so a game started from a
  scenario file that lists only units is still a war that can be won. What is NOT built is the ARRIVAL
  half of an escort ("win when the APC arrives"): that needs a unit to carry its own goal, which the sim
  has no notion of. Until then a mission is a list of deaths.
- [DECIDED — not built] The ENEMY DEPLOYS ITS OWN UNITS AUTOMATICALLY in an Instant Action game, via a
  scripted opening behaviour rather than something emergent. Its recycler starts as a vehicle like the
  player's and deploys by script.
- [DECIDED — not built] A destroyed BUILDING scatters scrap, exactly as a destroyed vehicle does, and
  scavengers may collect it (B.5). On record beside it: scrap is spent only when a factory builds a NEW item
  — repair, energy and ammo are always free and must never be gated on scrap (Part A section 8.2).
- [INTENDED — unconfirmed] "Do NOT hardcode another game-ending rule into the sim before the mission seam
  exists" is the assistant's design consequence of the ruling above, not a sentence of his. The seam it
  speaks of (`war_outcome`, `kind_wiped_out`) does exist.

PROVENANCE: `protekt-open-questions.md` Q5, Q5b and Q9 (Harri 2026-10-24, quoted); `protekt-hud-and-power.md`
section 5 (the scrap-and-buildings sentence); `slices.json` OBJ and RECY; code plus commit — `a584683`
(`src/sim/scenario.cpp:36-64`), `c5eb877` (Part A section 10), `include/protekt/sim/sim.hpp` (`war_outcome`,
`kind_wiped_out`).

### B.7 The build queue

One press is one unit; holding SHIFT while pressing a build key adds one more of the SAME kind per press, up
to ten in that batch; a producer holds one batch at a time and is busy until it is done; the batch drains one
at a time, gated by build time and scrap. [DECIDED — not built]

- [DECIDED — not built] HIS RULING, 2026-10-24: *"if I select the recycler and hit 1 for scavengers, I only
  build one scavenger. If, instead, I select the recycler, press the shift key and keep it down while I press
  1 one to ten times, the recycler builds the one to ten scavengers, one at a time. Each one takes a finite
  time to build, and also depends on how much scrap I have."* So one press is ONE unit; shift held queues one
  MORE per press, to a ceiling of TEN PER BATCH; the queue drains ONE AT A TIME; build time AND the scrap
  balance gate it. Nothing in the sim queues builds today.
- [DECIDED — built, BATCH-ONE-KIND] HIS RULING, 2026-10-28: *"Batch build can only build multiples of the same
  thing. If I start building a tank with the shift key down, I can only add more tanks of the same kind into
  that same batch. If I want to build anything different, I have to let go of shift, which initiates the
  build. Then I have to wait until the batch is done. After that, I can create another single build or batch
  build. If the recycler is doing a batch build, I can still get the factory to do its own batch build, and
  vice versa."* So a producer's queue IS one batch: an empty queue takes anything, and its first entry fixes
  the batch's kind and token; only that kind under that token joins, to a cap of ten that belongs to the
  batch (not a pool shared with other batches); a lone build is a batch of one and closes the queue until
  it is done. The sim refuses any other build at the door, so it never reaches the command log or a replay
  (`Sim::enqueue`, `build_fits`); the player's hands refuse aloud at the press (`Interaction::queue_build`).
  Each producer has its own queue, so a recycler's batch never blocks a factory's. Presses enter the line as
  they are made and letting go of shift ends the run; there is no "frozen until released" batch.
- [DECIDED — not built] Build TIME is per kind — 10 s for a scavenger, 30 s for a tank — so it belongs in
  the spec table beside the cost, which is the same shape as the per-entity DEPLOY time (B.2). The tick
  figures that accompany those seconds elsewhere in the source are written as 600 and 1800, which is 60 ticks
  a second; the sim runs at 30 (`kTicksPerSecond`), so take the SECONDS as the ruling and the tick figures as
  a stale conversion.
- [RULED 2026-10-28] A queue PAUSES when the scrap runs out and resumes when it returns. The bank is
  touched only when a unit STARTS to build, and then for that unit's own cost: "I must have the scrap to
  create the first, but I can add to the batch at will after that. Each build started at the exact same
  time as the scrap cost is deducted from my reserve." So the door never asks the bank — a batch may be
  queued on an empty reserve, and its later entries hold until the scrap arrives — and nothing is paid
  when a batch is ordered. (This corrects the earlier wording, which said the deduction happened as each
  unit completed.)
- [DECIDED — not built] A shift-built batch that finishes shares ONE function key, and each finished vehicle
  joins its batch's group; a lone build takes its own key (B.1). Each vehicle in a key draws its own health
  bar, in colour and not length, and a destroyed one's bar is removed (B.1, BAR-LOOK).
- [REMOVED 2026-10-28] The sim's automatic tank production (the C4 PRODUCTION BEAT, shipped in `e653f16`) is
  gone. It replaced a lost gun with a free `Tank` at a living factory on a global 150-tick beat; once the
  BUILD-QUEUE producer landed it only put a second, unordered build path in the player's own yard. Harri's
  ruling: "I think take it out completely. Smarter AI is needed for this. There should be different goals
  depending on state. I'll explain later." So nothing is built until a `Build` order is queued, for either
  side; the only build path in the tree is now the producer's queue. What replaces the beat is
  state-dependent goals in a real AI -- PARKED, not designed: Harri has deferred the explanation. Until it
  exists the ENEMY side queues nothing, so it never replaces a loss (see `docs/baselines/measure-C1.txt`).
  Pinned by `Build.NothingIsBuiltUntilItIsOrdered`.

PROVENANCE: `protekt-hud-and-power.md` section 5 (the shift-build ruling, quoted) and section 6;
`slices.json` RB5 (build times, the pause-and-resume rule, the batch ceiling) and ROW-SLOTS; code plus commit
— `e653f16` (`include/protekt/sim/units.hpp:148-154`, `src/sim/sim.cpp:849-936`), `include/protekt/sim/spec.hpp:35-36`.

### B.8 Carried across as unconfirmed, not as rulings

The sources mark these as the assistant's readings, or as waiting on the owner. They are kept here so a
reader can see what they are, and none of them is a rule the code may be built from.

- [INTENDED — unconfirmed] The capture's F-ROW reading that every slot carries a building-or-vehicle icon and
  a bar, and that the F-row "mixes buildings and units". It is an eye-witness reading of pixels, and it is
  contradicted by his later ruling: *"There are never any buildings under F keys. They are always vehicles."*
- [INTENDED — unconfirmed] The capture reading that a crystal or tower-like icon on `F2` "would fit" a power
  plant. A guess at an icon; the power plant does not exist.
- [INTENDED — unconfirmed] The names of the power buildings, the gun towers and the gun spires. He said
  himself that he would verify the names later, so no name may be taken from the capture reading.
- [INTENDED — unconfirmed] Whether the powered buildings he named are per-FACTION names too. The guess is
  "probably, by the production-list pattern"; the pattern is ruled, the inference is not.
- [INTENDED — unconfirmed] Which icon on the command bar is which building. Only `R` under `1` and `A` under
  `3` are legible in the capture.
- [INTENDED — unconfirmed] The `+1` reading on the power icon. It was retired by his own clarification of the
  surplus (B.5), so no number from that note is to be implemented.
- [INTENDED — unconfirmed] That power "must NOT be bolted onto the scrap ledger" because its shape is a
  capacity rather than a currency. A design consequence drawn from his ruling, not his sentence.
- [INTENDED — unconfirmed] The layout note that his capture carries the whole command row as one TOP bar
  while PROTEKT draws the command menu on the LEFT. Recorded so the layout pass asks rather than assumes.
- [INTENDED — unconfirmed] The half-remembered "each scavenger deployed over a pool raises the available
  scrap by 20". His answer to Q9 confirmed the 40 per faction and nothing else; the 20 was never said in this
  thread and was not confirmed, so it may not be built on. It is kept here only so it is not mistaken for a
  ruling later.
- [INTENDED — unconfirmed] "A menu is a MODE that SUSPENDS the prompts" (`menu.hpp:40-43`). It is written as
  a property of the REFERENCE menu; whether PROTEKT adopts the mode is ruled nowhere (Part A section 14.1,
  question 2). The shipped behaviour is the conservative reading: the `G`, `T` and attack-list prompts settle
  a digit before the menu sees it.
- [CLOSED by his ruling, 2026-10-26] The recycler's key. HIS WORDS: *"A deployed recycler is not under any
  function key. It's under the 1 key. Only an un-deployed recycler is under a function key. The deployment
  process turns it into a building, and it becomes 1. Always 1."* So there is no `F1` display rule and no
  spawn-order change: an undeployed recycler may sit under any `F` key (the shipped spawn puts it on `F7`),
  and on deploy the building takes key `1`, always. Q6.3 is answered; RB3 and RB5 read this.

PROVENANCE: `protekt-hud-and-power.md` sections 2, 3 (items 1-4) and the layout note in section 4;
`protekt-open-questions.md` Q9 (the unconfirmed 20; Q6.3 is answered — the CLOSED entry above);
`include/protekt/game/menu.hpp:40-44`.
