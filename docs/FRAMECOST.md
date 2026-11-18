# Where a frame's time goes (N6) -- a recorded measurement

Tool: `tools/framecost.cpp`, target `protekt-framecost`; run `build-release/protekt-framecost`.
Timing hook: `FrameTimings` (scene.hpp) filled by `game::build_scene` when `SceneOptions::timings` is set.
Clock: `std::chrono::steady_clock`. Seed 42, radius 24 tiles, 1280x720, camera at the base's centroid,
5 warm-up + 60 measured frames per scenario, terrain cache warm (as the app has it).

Recorded 2026-11-17, Release `-O3 -DNDEBUG`, this machine, **renderer = llvmpipe (a SOFTWARE rasteriser)**.
Run-to-run drift between two full runs was about 5-8%, so compare a later change against these
numbers only when it moves a phase by more than that.

| scenario | buildings | build_scene | smooth_vertex_normals | units+buildings | hud | terrain | Renderer::render (llvmpipe) |
|---|---|---|---|---|---|---|---|
| 0 bare ground | 0 | 4.28 ms | 2.13 ms | 0.30 ms | 1.26 ms | 0.45 ms | 31.1 ms |
| A default skirmish | 1 | 8.38 ms | 4.98 ms | 1.57 ms | 1.01 ms | 0.45 ms | 32.1 ms |
| B his base | 10 | 10.37 ms | 5.86 ms | 1.96 ms | 1.65 ms | 0.49 ms | 42.0 ms |
| C his base x2 | 19 | 12.53 ms | 7.46 ms | 2.47 ms | 1.63 ms | 0.49 ms | 45.9 ms |

Renderer floor (one triangle: readback and fixed cost) was 14.5 ms in every scenario. The table is from one full run and the raw output below from a second, so they differ by that drift.

```
protekt-framecost: build type Release, flags:  -O3 -DNDEBUG

== 0: bare ground
   composition: 1 Tank and nothing else
   seed 42, 1 player units standing (0 of them buildings), radius 24 tiles, 1280x720
   -- game::build_scene, mean of 60 frames after 5 warm-up --
   terrain                   0.478 ms   10.8% of build_scene
   sky + shadows             0.025 ms    0.6% of build_scene
   units + buildings         0.314 ms    7.1% of build_scene
   world effects             0.026 ms    0.6% of build_scene
   smooth_vertex_normals     2.186 ms   49.3% of build_scene
   fog (both passes)         0.088 ms    2.0% of build_scene
   hud                       1.315 ms   29.7% of build_scene
   sum of the phases         4.431 ms  (inner total)
   build_scene               4.431 ms  (timed inside)
   build_scene               4.432 ms  (timed outside; median 4.389)
   vertices: 40023 total, 20502 world, 15000 terrain; terrain cache built once: 0.156 ms
   -- renderer (llvmpipe (LLVM 19.1.7, 256 bits)), mean of 60 frames after 5 warm-up --
   Renderer::render         31.061 ms  (median 30.551; 13341 triangles submitted)
   renderer floor           14.936 ms  (one triangle: upload/GPU/readback that does not scale with geometry)
   whole frame, CPU build + render: 35.493 ms  =  28.2 fps

== A: default skirmish
   composition: 3 Tank, HoverTank, Factory, 2 Turret, MortarBike, RocketTank, Scavenger, Recycler (undeployed)
   seed 42, 11 player units standing (1 of them buildings), radius 24 tiles, 1280x720
   -- game::build_scene, mean of 60 frames after 5 warm-up --
   terrain                   0.493 ms    5.5% of build_scene
   sky + shadows             0.158 ms    1.8% of build_scene
   units + buildings         1.701 ms   19.0% of build_scene
   world effects             0.027 ms    0.3% of build_scene
   smooth_vertex_normals     5.282 ms   59.1% of build_scene
   fog (both passes)         0.219 ms    2.5% of build_scene
   hud                       1.054 ms   11.8% of build_scene
   sum of the phases         8.934 ms  (inner total)
   build_scene               8.935 ms  (timed inside)
   build_scene               8.935 ms  (timed outside; median 8.765)
   vertices: 61749 total, 42186 world, 15000 terrain; terrain cache built once: 0.048 ms
   -- renderer (llvmpipe (LLVM 19.1.7, 256 bits)), mean of 60 frames after 5 warm-up --
   Renderer::render         33.298 ms  (median 32.721; 20583 triangles submitted)
   renderer floor           15.033 ms  (one triangle: upload/GPU/readback that does not scale with geometry)
   whole frame, CPU build + render: 42.234 ms  =  23.7 fps

== B: his base
   composition: Factory, DeployedRecycler, Power, GunTower, ServiceBay, RelayBunker, Armory, TrainingCentre, TechCentre, BomberBay (10 buildings) + 3 Tank, HoverTank, MortarBike, RocketTank, Scavenger
   seed 42, 17 player units standing (10 of them buildings), radius 24 tiles, 1280x720
   -- game::build_scene, mean of 60 frames after 5 warm-up --
   terrain                   0.538 ms    4.8% of build_scene
   sky + shadows             0.165 ms    1.5% of build_scene
   units + buildings         2.106 ms   18.8% of build_scene
   world effects             0.027 ms    0.2% of build_scene
   smooth_vertex_normals     6.460 ms   57.5% of build_scene
   fog (both passes)         0.245 ms    2.2% of build_scene
   hud                       1.691 ms   15.1% of build_scene
   sum of the phases        11.232 ms  (inner total)
   build_scene              11.233 ms  (timed inside)
   build_scene              11.233 ms  (timed outside; median 11.054)
   vertices: 67677 total, 48042 world, 15000 terrain; terrain cache built once: 0.051 ms
   -- renderer (llvmpipe (LLVM 19.1.7, 256 bits)), mean of 60 frames after 5 warm-up --
   Renderer::render         43.399 ms  (median 42.427; 22559 triangles submitted)
   renderer floor           15.130 ms  (one triangle: upload/GPU/readback that does not scale with geometry)
   whole frame, CPU build + render: 54.633 ms  =  18.3 fps

== C: his base x2
   composition: B, plus a second Power, GunTower, ServiceBay, RelayBunker, Armory, TrainingCentre, TechCentre, BomberBay, Factory, Turret x2 (19 buildings) + the same units
   seed 42, 28 player units standing (19 of them buildings), radius 24 tiles, 1280x720
   -- game::build_scene, mean of 60 frames after 5 warm-up --
   terrain                   0.510 ms    3.9% of build_scene
   sky + shadows             0.175 ms    1.3% of build_scene
   units + buildings         2.586 ms   19.8% of build_scene
   world effects             0.026 ms    0.2% of build_scene
   smooth_vertex_normals     7.791 ms   59.7% of build_scene
   fog (both passes)         0.298 ms    2.3% of build_scene
   hud                       1.656 ms   12.7% of build_scene
   sum of the phases        13.041 ms  (inner total)
   build_scene              13.042 ms  (timed inside)
   build_scene              13.042 ms  (timed outside; median 12.952)
   vertices: 78087 total, 58368 world, 15000 terrain; terrain cache built once: 0.057 ms
   -- renderer (llvmpipe (LLVM 19.1.7, 256 bits)), mean of 60 frames after 5 warm-up --
   Renderer::render         47.332 ms  (median 46.204; 26029 triangles submitted)
   renderer floor           14.917 ms  (one triangle: upload/GPU/readback that does not scale with geometry)
   whole frame, CPU build + render: 60.374 ms  =  16.6 fps
```

---

# The battle (N7) -- the half N6 could not see

His bar (verbatim): *"In BattleZone 2, there can be battles in the middle of a compound with dozens of
vehicles, tons of mortars and bolts flying at the same time.  Protekt needs to be able to keep up with that too!"*

Tool: the same `protekt-framecost`; `protekt-framecost battle` runs only these (`bases`, or no argument = both).
Hooks: `Sim::step(StepTimings*)` (`include/protekt/sim/timings.hpp`; null reads no clock), the hit-test span
inside `advance_projectiles`, and `FrameTimings` as before. Clock: `std::chrono::steady_clock`.

**The scenario, driven through the real sim.** Seed 42, 1280x720, radius 24. Both sides carry the same compound:
10 buildings (Factory, DeployedRecycler, Power, GunTower, ServiceBay, RelayBunker, Armory, TrainingCentre,
TechCentre, BomberBay) + 2 Turret, plus the vehicles below. The enemy is the app's own `ScriptedCommander` (every
combatant attacks the nearest enemy), so it marches on the player's compound; the player's side is held by
`UtilityCommander` (standing in for units the human is not driving). Each frame is what the game does:
`observe` + `think` (both sides) -> `Sim::step` -> `build_scene` -> `Renderer::render`. The sim runs un-timed
until the first impact (contact, ~415-440 ticks); then 20 warm-up frames and 200 counted frames (30 Hz: 6.7 s of
game time, every one in contact). The camera is the centre of the player's compound. Projectiles are the sim's own.

| size | vehicles per side | units alive (mean) | projectiles in flight (mean / max) | mortar shells (mean) | Sim::step | fire (guns) | AI | build_scene | Renderer::render (llvmpipe) |
|---|---|---|---|---|---|---|---|---|---|
| S skirmish | 5 Tank 2 Hover 3 Mortar 2 Rocket = 12 | 47.6 | 13.2 / 20 | 3.1 | 3.06 ms | 2.33 ms | 0.02 ms | 5.7 ms | 34.6 ms |
| M battle | 12 Tank 4 Hover 6 Mortar 4 Rocket = 26 | 65.7 | 22.7 / 30 | 7.4 | 6.60 ms | 4.67 ms | 0.03 ms | 13.2 ms | 51.4 ms |
| H heavy | 24 Tank 8 Hover 10 Mortar 6 Rocket = 48 | 94.0 | 19.5 / 25 | 8.8 | 21.69 ms | 20.10 ms | 0.04 ms | 16.4 ms | 63.3 ms |

Whole frame at H: CPU 38.1 ms + render 63.3 ms = 101 ms = 9.9 fps (S: 23.1 fps, M: 14.0 fps). Worst single `Sim::step`
at H: 84 ms. The frame's wall clock (worst frame) at H was 231 ms.

Growth S -> H: units x2.0, vehicles x4.0, but `Sim::step` x7.1 and `fire` x8.6; `build_scene` x2.9.

**Renderer: llvmpipe, a SOFTWARE rasteriser** -- its numbers say nothing about a real GPU, and they are the bulk
of the whole-frame figure here. The CPU rows (Sim::step, AI, build_scene) do not depend on it.
Release `-O3 -DNDEBUG`, this machine. Three full runs differed by under about 3% in the means.

**What the numbers support** (and what they do not) is in the slice report; the headline is in the raw output:
inside `Sim::step`, `fire` is 76% / 70% / 93% of the step at S / M / H and it is entirely the target search's two
tests (`has_line` and `shot_hurts_friend`); 99% of the friend probes REFUSE the shot (257 refusals per tick at H
against 0.79 shots fired). The AI is under 0.05 ms mean (spikes of 0.3 / 0.6 / 1.5-2.0 ms on its 15-tick beat).
The projectile advance is 0.24 / 0.47 / 0.40 ms, about 17-20 us per projectile in flight: it is NOT the cost at
20 projectiles, but this scenario never gets past 30 in the air, so "hundreds of projectiles" is NOT measured
here -- see the slice report.

```
== S: skirmish
   PER SIDE: 10 buildings (Factory, DeployedRecycler, Power, GunTower, ServiceBay, RelayBunker, Armory, TrainingCentre, TechCentre, BomberBay) + 2 Turret + 5 Tank, 2 HoverTank, 3 MortarBike, 2 RocketTank (12 vehicles); enemy = ScriptedCommander, player = UtilityCommander
   seed 42, 14 player vehicles + 10 player buildings at the start (enemy the same); contact after 438 un-timed ticks; 200 frames after 20 warm-up (all in contact), 1280x720, radius 24
   alive at the end: 14 player vehicles, 11 enemy vehicles
   -- what is on the field, per measured frame --
   living units (all, both sides) mean     47.6   min     45   max     48
   projectiles in flight      mean     13.2   min      4   max     20
     of which mortar shells   mean      3.1   min      0   max      5
   shots fired per tick       mean      0.6   min      0   max      3
   impacts per tick           mean      0.5   min      0   max      9
   scene vertices             mean  48149.8   min  45705   max  53439
   -- the SIM's side of the frame (mean and worst of the counted frames) --
   AI: observe (2 sides)      mean   0.008 ms   max   0.037 ms   sim::observe, player + enemy
   AI: think (2 sides)        mean   0.016 ms   max   0.299 ms   UtilityCommander + ScriptedCommander
   AI total                   mean   0.023 ms   max   0.306 ms   
   Sim::step                  mean   3.055 ms   max   5.539 ms   (timed inside)
     orders                   mean   0.000 ms   max   0.001 ms   
     movement                 mean   0.482 ms   max   1.712 ms   deploy, scavenge, steer, move
       of which steer         mean   0.466 ms   max   1.698 ms   inside movement
     bolts (advance)          mean   0.235 ms   max   0.364 ms   advance_projectiles
       of which hit tests     mean   0.221 ms   max   0.342 ms   inside bolts: units + buildings, per projectile
     fire (guns)              mean   2.330 ms   max   4.683 ms   target search, line of sight, launch
       of which has_line      mean   1.092 ms   max   2.182 ms   inside fire: line of sight (clock per call: reads high)
       of which friend probe  mean   1.215 ms   max   2.492 ms   inside fire: shot_hurts_friend, a whole flight each
     rest                     mean   0.006 ms   max   0.194 ms   lungs, repair, wrecks, store, queues
   per tick: 33.3 has_line calls (max 69), 33.17 friend probes (max 69); a probe costs 36.6 us on average; a projectile's hit tests 16.7 us
   per tick: 0.1 has_line calls found no line; 32.5 friend probes refused the shot; 0.59 shots were actually fired
   sum of step phases           3.055 ms   (worst frame's |sum of step phases - step total|: 0.0000 ms)
   -- the scene (game::build_scene) --
   build_scene                mean   5.700 ms   max  11.340 ms   (timed outside)
     units + buildings        mean   0.760 ms   max   1.227 ms   
     world effects            mean   0.044 ms   max   0.413 ms   projectiles, scrap, pools, beams, motes
     smooth_vertex_normals    mean   3.071 ms   max   7.336 ms   
     hud                      mean   1.234 ms   max   1.854 ms   
     terrain                  mean   0.443 ms   max   0.629 ms   
   (worst frame's |sum of scene phases - scene total|: 0.0004 ms)
   CPU per frame (AI + step + build_scene): 8.778 ms
   of that: AI 0.3%, Sim::step 34.8%, build_scene 64.9%
   -- renderer (llvmpipe (LLVM 19.1.7, 256 bits)) --
   Renderer::render           mean  34.598 ms   max  46.916 ms   
   whole frame: CPU 8.778 ms + render 34.598 ms = 43.376 ms = 23.1 fps  (the loop's own wall clock per frame: mean 43.378 ms, worst 59.792 ms)

== M: battle
   PER SIDE: 10 buildings (Factory, DeployedRecycler, Power, GunTower, ServiceBay, RelayBunker, Armory, TrainingCentre, TechCentre, BomberBay) + 2 Turret + 12 Tank, 4 HoverTank, 6 MortarBike, 4 RocketTank (26 vehicles); enemy = ScriptedCommander, player = UtilityCommander
   seed 42, 28 player vehicles + 10 player buildings at the start (enemy the same); contact after 435 un-timed ticks; 200 frames after 20 warm-up (all in contact), 1280x720, radius 24
   alive at the end: 22 player vehicles, 16 enemy vehicles
   -- what is on the field, per measured frame --
   living units (all, both sides) mean     65.7   min     58   max     76
   projectiles in flight      mean     22.7   min     10   max     30
     of which mortar shells   mean      7.4   min      4   max     12
   shots fired per tick       mean      0.9   min      0   max      6
   impacts per tick           mean      1.1   min      0   max     22
   scene vertices             mean  80173.7   min  71631   max 106923
   -- the SIM's side of the frame (mean and worst of the counted frames) --
   AI: observe (2 sides)      mean   0.009 ms   max   0.012 ms   sim::observe, player + enemy
   AI: think (2 sides)        mean   0.017 ms   max   0.610 ms   UtilityCommander + ScriptedCommander
   AI total                   mean   0.026 ms   max   0.619 ms   
   Sim::step                  mean   6.595 ms   max  26.133 ms   (timed inside)
     orders                   mean   0.001 ms   max   0.002 ms   
     movement                 mean   1.453 ms   max   6.620 ms   deploy, scavenge, steer, move
       of which steer         mean   1.413 ms   max   6.577 ms   inside movement
     bolts (advance)          mean   0.449 ms   max   0.891 ms   advance_projectiles
       of which hit tests     mean   0.422 ms   max   0.826 ms   inside bolts: units + buildings, per projectile
     fire (guns)              mean   4.665 ms   max  24.329 ms   target search, line of sight, launch
       of which has_line      mean   2.205 ms   max  11.613 ms   inside fire: line of sight (clock per call: reads high)
       of which friend probe  mean   2.420 ms   max  12.702 ms   inside fire: shot_hurts_friend, a whole flight each
     rest                     mean   0.028 ms   max   0.419 ms   lungs, repair, wrecks, store, queues
   per tick: 55.8 has_line calls (max 253), 54.55 friend probes (max 253); a probe costs 44.4 us on average; a projectile's hit tests 18.6 us
   per tick: 1.3 has_line calls found no line; 53.6 friend probes refused the shot; 0.89 shots were actually fired
   sum of step phases           6.595 ms   (worst frame's |sum of step phases - step total|: 0.0001 ms)
   -- the scene (game::build_scene) --
   build_scene                mean  13.158 ms   max  21.114 ms   (timed outside)
     units + buildings        mean   2.874 ms   max   6.999 ms   
     world effects            mean   0.058 ms   max   0.683 ms   projectiles, scrap, pools, beams, motes
     smooth_vertex_normals    mean   7.779 ms   max  12.366 ms   
     hud                      mean   1.525 ms   max   1.982 ms   
     terrain                  mean   0.470 ms   max   0.638 ms   
   (worst frame's |sum of scene phases - scene total|: 0.0002 ms)
   CPU per frame (AI + step + build_scene): 19.780 ms
   of that: AI 0.1%, Sim::step 33.3%, build_scene 66.5%
   -- renderer (llvmpipe (LLVM 19.1.7, 256 bits)) --
   Renderer::render           mean  51.396 ms   max  79.654 ms   
   whole frame: CPU 19.780 ms + render 51.396 ms = 71.176 ms = 14.0 fps  (the loop's own wall clock per frame: mean 71.178 ms, worst 124.046 ms)

== H: heavy battle
   PER SIDE: 10 buildings (Factory, DeployedRecycler, Power, GunTower, ServiceBay, RelayBunker, Armory, TrainingCentre, TechCentre, BomberBay) + 2 Turret + 24 Tank, 8 HoverTank, 10 MortarBike, 6 RocketTank (48 vehicles); enemy = ScriptedCommander, player = UtilityCommander
   seed 42, 50 player vehicles + 10 player buildings at the start (enemy the same); contact after 414 un-timed ticks; 200 frames after 20 warm-up (all in contact), 1280x720, radius 24
   alive at the end: 18 player vehicles, 46 enemy vehicles
   -- what is on the field, per measured frame --
   living units (all, both sides) mean     94.0   min     84   max    120
   projectiles in flight      mean     19.5   min      9   max     25
     of which mortar shells   mean      8.8   min      5   max     12
   shots fired per tick       mean      0.8   min      0   max      3
   impacts per tick           mean      1.7   min      0   max     31
   scene vertices             mean  95390.2   min  64065   max 165093
   -- the SIM's side of the frame (mean and worst of the counted frames) --
   AI: observe (2 sides)      mean   0.011 ms   max   0.023 ms   sim::observe, player + enemy
   AI: think (2 sides)        mean   0.029 ms   max   1.484 ms   UtilityCommander + ScriptedCommander
   AI total                   mean   0.040 ms   max   1.495 ms   
   Sim::step                  mean  21.686 ms   max  84.269 ms   (timed inside)
     orders                   mean   0.001 ms   max   0.003 ms   
     movement                 mean   1.133 ms   max   6.591 ms   deploy, scavenge, steer, move
       of which steer         mean   1.074 ms   max   6.504 ms   inside movement
     bolts (advance)          mean   0.400 ms   max   0.842 ms   advance_projectiles
       of which hit tests     mean   0.376 ms   max   0.790 ms   inside bolts: units + buildings, per projectile
     fire (guns)              mean  20.095 ms   max  81.852 ms   target search, line of sight, launch
       of which has_line      mean   9.325 ms   max  36.656 ms   inside fire: line of sight (clock per call: reads high)
       of which friend probe  mean  10.717 ms   max  45.555 ms   inside fire: shot_hurts_friend, a whole flight each
     rest                     mean   0.058 ms   max   2.858 ms   lungs, repair, wrecks, store, queues
   per tick: 258.7 has_line calls (max 1178), 258.06 friend probes (max 1176); a probe costs 41.5 us on average; a projectile's hit tests 19.3 us
   per tick: 0.6 has_line calls found no line; 257.1 friend probes refused the shot; 0.79 shots were actually fired
   sum of step phases          21.686 ms   (worst frame's |sum of step phases - step total|: 0.0000 ms)
   -- the scene (game::build_scene) --
   build_scene                mean  16.418 ms   max  33.496 ms   (timed outside)
     units + buildings        mean   3.823 ms   max   9.214 ms   
     world effects            mean   0.092 ms   max   0.731 ms   projectiles, scrap, pools, beams, motes
     smooth_vertex_normals    mean  10.053 ms   max  21.726 ms   
     hud                      mean   1.455 ms   max   2.327 ms   
     terrain                  mean   0.461 ms   max   0.736 ms   
   (worst frame's |sum of scene phases - scene total|: 0.0004 ms)
   CPU per frame (AI + step + build_scene): 38.144 ms
   of that: AI 0.1%, Sim::step 56.9%, build_scene 43.0%
   -- renderer (llvmpipe (LLVM 19.1.7, 256 bits)) --
   Renderer::render           mean  63.302 ms   max 135.636 ms   
   whole frame: CPU 38.144 ms + render 63.302 ms = 101.445 ms = 9.9 fps  (the loop's own wall clock per frame: mean 101.448 ms, worst 231.281 ms)
```


---

# The standing buildings' meshes, cached (N8) -- the first optimisation

`BuildingMeshCache` (`include/protekt/game/building_cache.hpp`, `SceneOptions::buildings`): a building that stands
is built once (the same `build_building_model` + `scale_model` calls the live path makes, then smoothed alone),
copied out on every later frame, and its range is stepped over by `smooth_vertex_normals`. Keyed by everything the
builders read: kind, side, `turned`, the place (`x.raw`, `y.raw`), the hull variant and the world seed. A turret
(aims), a deployed recycler (a hull), a marked building (its colour), a site under construction (cut each frame)
and a lifted one are drawn live. An entry a frame did not use is dropped at its end.

**The frame is byte-identical**: `protekt-game --headless --no-sound --seed 42 --frames 3 --scenario <12 buildings
+ tank> --capture` is `sha256 338f2e63...97fd` before and after; `BuildingCache.*` compares every vertex of
every cacheable kind, both sides, with `memcmp`.

Same binary, `protekt-framecost bases` (Release `-O3 -DNDEBUG`, llvmpipe), cache off -> on, the x2 base (C) is
18 buildings from the cache, 17,208 of 78,087 vertices; the pass walks 41,160 vertices instead of 58,368:

| scenario | smooth_vertex_normals | build_scene | whole frame (llvmpipe) |
|---|---|---|---|
| 0: bare ground | 2.15 -> 2.14 ms | 4.28 -> 4.29 ms | 35.0 -> 35.5 ms |
| A: default skirmish | 5.00 -> 4.84 ms | 8.38 -> 8.19 ms | 40.8 -> 40.0 ms |
| B: his base | 5.90 -> 4.73 ms | 10.35 -> 8.86 ms | 53.3 -> 50.8 ms |
| C: his base x2 | 7.49 -> 4.89 ms | 12.52 -> 9.11 ms | 58.4 -> 55.1 ms |

**What this does and does not buy.** The buildings were 17.2k of the 58.4k vertices the pass walked at C, so the
pass fell by about a third, not to zero: the rest is the terrain (15,000 vertices, about 2.1 ms by itself -- the
whole of the bare-ground scenario's pass) and the units, shadows and effects. The cache takes about 2.6 ms off the
pass and about 0.8 ms off "units + buildings" at C, 3.4 ms off `build_scene`. It does not reach the goal's 20 ms
alone; the next biggest single cost in the pass is the terrain, which changes with the camera and was not touched.
