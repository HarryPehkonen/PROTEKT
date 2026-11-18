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

## N9: the guns' target search and line of sight go on a beat

N7 measured the battle's cost as the guns choosing targets (`fire`: 2.33 ms of a 3.06 ms step at S, ~16-20 ms of a
21.7 ms step at H), and 99% of the friend probes refusing the shot. A gun whose every candidate a friend shields never
fires, so it is ready again the next tick and searches everything again.

Now a unit chooses its target, and proves a line to it, once every `kThinkBeatTicks` = 15 ticks (the AI's own beat,
`game::kDefendCadence` is the same constant), phased by unit id so the guns do not all search on one tick, and KEEPS
the choice in `Unit::fire_target` between beats. Between beats a kept target is fired at if it is alive and in reach
and the friend probe (one flight per SHOT, not per tick) still allows the shot; otherwise it is dropped and the next
beat chooses. The line of sight is not asked between beats: here it is terrain-only, and the ground does not move.
A `FocusFire` is still honoured every tick. An `Attack` order's named target is taken up on the next beat.

Release `-O3 -DNDEBUG`, llvmpipe renderer (software), same machine and scenarios as N7; OLD = `d47c427`:

| size | Sim::step OLD -> NEW | fire OLD -> NEW | has_line calls per tick | CPU per frame (AI+step+scene) OLD -> NEW |
|---|---|---|---|---|
| S | 3.11 -> 1.47 ms | 2.37 -> 0.27 ms | 33.3 -> 3.0 | 8.4 -> 6.7 ms |
| M | 6.56 -> 2.16 ms | 4.62 -> 0.66 ms | 55.8 -> 8.3 | 19.9 -> 11.8 ms |
| H | 21.67 -> 6.04 ms | 20.09 -> 3.40 ms | 258.7 -> 44.2 | 38.2 -> 33.4 ms |

(The two fights are not the same fight -- H has 94.0 units alive on average OLD and 107.7 NEW, and `build_scene` grows
with them -- which is why the whole CPU frame falls less than the step.)

The drift: `protekt-framecost drift [out-dir [seed...]]` runs each battle headless for 3000 ticks and prints shots,
kills and who is left; run on both sides of the change over eight seeds (42 7 99 1024 5 11 23 31):

| size | shots OLD / NEW (8 seeds) | kills OLD / NEW | player ahead at the end OLD / NEW |
|---|---|---|---|
| S | 2530 / 2570 | 186 / 171 | 6 / 6 |
| M | 4175 / 3506 | 359 / 319 | 6 / 7 |
| H | 4282 / 3935 | 598 / 611 | 6 / 7 |

On seed 42, every frame from tick ~407-424 differs (2571 / 2575 / 2593 of 3000 at S / M / H), 505 / 635 / 614 of them in
the shots fired; shots 311 -> 406 (S), 471 -> 591 (M), 616 -> 368 (H); kills 19 -> 28, 36 -> 46, 80 -> 76; first impact
437 -> 452, 434 -> 443, 413 -> 420. The WINNER of an individual fight is chaotic in the timing and does change on a
given seed (S 42: player 8 vs 2 -> 0 vs 10; H 42: 0 vs 30 -> 23 vs 2); in aggregate the fights are alike.

## N10: the same cache, for vehicles

`BuildingMeshCache` has a second table, keyed by `VehicleMeshKey`: seed, place (`x.raw`, `y.raw`), hull variant, kind,
side, faction (`alien`), the hull heading and the turret bearing BIT FOR BIT, and the walk phase for the two legged
kinds. An entry is the live path's own output for that pose (`build_unit_body`, the one dispatch both paths call, then
scale and tilt as the live path does, then smoothed alone), and its range is stepped over by `smooth_vertex_normals`.
Every vehicle kind (16) is cacheable: the builders read only id, kind, side and place, so no kind has per-unit vertices
the key does not name. Drawn live: a marked vehicle, a lifted one, the DRIVEN one, and a site.

**Not a template plus a transform.** The brief asked for one smoothed template per (kind, faction, side), moved by
translation, yaw and tilt. That is not byte-exact (rotation about each part's pivot, then the ground tilt, in floats;
and the smoothed normals are weighted sums that change in the last bits once faces turn), and this project compares
frames byte for byte. So the key carries the pose: a vehicle that holds still is a hit, one that MOVES or whose gun
turns is a miss every frame (built and smoothed alone, the same work the pass did, so the smoothing time MOVES from
"smooth_vertex_normals" to "units + buildings" for those).

**Byte-identical**: `protekt-game --headless --no-sound --seed 42 --frames 200 --scenario mix.toml --capture` (3 Tank,
2 Hover, 2 Mortar, 2 Rocket, 2 Scavenger, Recycler, 2 Scout, 2 Turret, Factory) is
`sha256 032664f8...456f` before and after; the same with `--seed 7 --alien` is `sha256 0cf59b3d...dae6` before and after
(game hashes 14475664809902220616 and 5296562310756344311).

## N11: the emission and the render, split -- and the floor as a LINE

Measurement only: nothing is optimised, no recorded battle moves. Recorded 2026-11-18, Release
`-O3 -DNDEBUG`, seed 42, 1280x720, radius 24, **renderer = `llvmpipe (LLVM 19.1.7, 256 bits)`, a
SOFTWARE rasteriser: this machine has no GPU (`/dev/dri` has a render node, `devices()` lists llvmpipe
only). The GTX 1060 figures quoted in the N11 brief were NOT measurable here.** Every number below is
this machine's. The CPU-side ones (the emission) have the same shape on any box; the render ones are
llvmpipe's and say nothing about a GTX 1060's absolute cost.

Tool: `protekt-framecost floor` (new; also part of `all`) and the new rows of `protekt-framecost battle`.
Hooks: `FrameStats::times` (renderer.hpp, filled by `Renderer::render`), and the N11 fields of
`FrameTimings` (scene.hpp). Raw output of both runs is pasted below.

### The floor, as a line (`protekt-framecost floor`)

Triangles are drawn from base C's real world (every k-th triangle, repeated past the end), 10 warm-up
and 60 counted frames per point, run ascending and then descending to catch an ordering effect (there
is none: the two runs agree to 0.3 ms).

| triangles | mean ms | median | min | upload | record | submit | wait (GPU) | readback |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 14.433 | 14.228 | 14.095 | 0.000 | 0.031 | 0.007 | 13.811 | 0.581 |
| 1 | 14.699 | 14.482 | 14.243 | 0.001 | 0.035 | 0.008 | 14.057 | 0.597 |
| 100 | 14.837 | 14.637 | 14.456 | 0.002 | 0.032 | 0.023 | 14.193 | 0.584 |
| 1,000 | 17.787 | 17.575 | 17.180 | 0.017 | 0.032 | 0.008 | 17.143 | 0.585 |
| 10,000 | 35.745 | 35.318 | 34.613 | 0.165 | 0.034 | 0.010 | 34.955 | 0.579 |
| 50,000 | 63.634 | 59.427 | 57.041 | 0.784 | 0.037 | 0.014 | 62.214 | 0.583 |

Least squares over all six points: **intercept 17.0 ms, slope 0.97 ms per 1,000 triangles (~323 ns per
vertex), R^2 0.95** (descending run: 17.0 ms, 0.92, 0.94). R^2 is only 0.95 because the cost is NOT
linear: 0 -> 100 triangles adds 0.4 ms, 100 -> 1,000 adds 3.0 ms, 1,000 -> 10,000 adds 18 ms (2.0 ms per
1,000), 10,000 -> 50,000 adds 28 ms (0.7 ms per 1,000). A line through that is a summary, not a law:
the pixel coverage of a triangle (terrain triangles are big, model triangles small) matters more than
the count. The directly MEASURED fixed cost is the 0-triangle row, **14.4 ms**; the fit's 17.0 is
higher only because the line is pulled up by the curve's bend.

**The old 15.3 ms "floor" was the real fixed cost, not a mis-timed call and not noise.** The old row
timed one triangle (in the HUD pass) and read 14.5-15.3 ms across runs; the 1-triangle row here is
14.7 ms and the empty frame 14.4 ms. It was correctly measured, and "one triangle" was an accurate
description of what it measured; what it could not do was say that the cost is fixed and not
per-vertex. The line does. On llvmpipe the fixed part is almost all `wait`: 13.8 of the 14.4 ms is the
frame's two full-screen passes (clear, the FXAA filter) and the copy into the readback buffer being
rasterised on the CPU. `readback` (the 3.7 MB result vector allocated, zero-filled, copied) is a steady
0.58 ms; `upload` is 5 ns per vertex (0.78 ms at 150,000 vertices); `record` and `submit` are 0.04 ms
together. **What this cannot say: how much of the GPU-side fixed cost is the clear, the filter or the
copy** -- there are no GPU timestamp queries, and on a real GPU those full-screen passes cost a small
fraction of what the software rasteriser charges.

### The emission, inside `build_scene` (`protekt-framecost battle`, H: heavy battle)

Same scenario as the N10 table (115,023 vertices in the last frame, which matches the brief's count
exactly, so the same fight). This machine's `build_scene` is 17.2 ms against the brief's 14.1: slower
box, same proportions.

| phase of H's frame | mean ms | note |
|---|---:|---|
| build_scene | 17.232 | |
| units + buildings | 11.912 | 69% of build_scene |
| &nbsp;&nbsp;cache key + find | 0.011 | the `std::map` walk is nothing |
| &nbsp;&nbsp;template build (a MISS) | **10.001** | 20.9 misses per frame (max 36), 56,452 vertices built per frame |
| &nbsp;&nbsp;&nbsp;&nbsp;builder (`build_unit_body`) | 2.000 | |
| &nbsp;&nbsp;&nbsp;&nbsp;scale + tilt | 1.741 | |
| &nbsp;&nbsp;&nbsp;&nbsp;`smooth_vertex_normals` on the miss | **6.250** | |
| &nbsp;&nbsp;&nbsp;&nbsp;map insert, move | 0.011 | |
| &nbsp;&nbsp;copy of a cached mesh into the scene | 1.865 | `vector::insert`, growth included |
| &nbsp;&nbsp;live (uncached) path | 0.000 | no unit takes it in this battle |
| &nbsp;&nbsp;dispatch (the rest) | 0.035 | visibility, selection, heading, `gun_yaw` |
| smooth_vertex_normals (the pass) | 1.933 | |
| hud | 1.959 | |
| fog (both passes) | 0.648 | |
| terrain, sky + shadows, world effects | 0.492, 0.180, 0.080 | |

Worst frame's |sum of scene phases - total| is 0.0004 ms; the units sub-rows sum to `units_ms` within
0.04 ms (that gap is the dispatch row, by construction), and the per-unit clock reads are inside the
rows they bracket.

**What this says, and it is not what the N10 table suggested.**
1. **The cost is not "copying templates into a buffer": it is BUILDING meshes from nothing, ~21 times a
   frame.** A vehicle that moves, or whose turret turns, has a new cache key every frame, so it is a
   miss every frame (the N10 design says so openly). About 21 units miss per frame in H (max 36); the tool does not separate moving from turning from newly seen, so "moving" is the likely cause, not a measured one.
2. **The smoothing did not go from 16.7 ms to 1.8: it MOVED.** The 1.9 ms row is only what is left in
   the world pass; the smoothing of every missed mesh happens inside `vehicle_mesh` and is counted under
   `units`. Smoothing is 1.9 + 6.25 = **8.2 ms** of H's frame here, i.e. most of it. `FrameTimings`'s
   `smooth_ms` is now an under-report of smoothing; the `template_smooth_ms` field is the rest.
3. **Capacity behaviour.** `build_scene` constructs a fresh `Scene` each frame and the loop
   move-assigns it over the last one, so `vertices` starts at capacity 0 every frame and no `reserve`
   exists anywhere in the pass. Final capacity 117,804 vertices (6.1 MB) for 115,023; 5.0 capacity
   changes seen per frame (checked at unit and phase boundaries -- a LOWER bound on reallocations).
   Measured on the frame's own vertices appended 1,000 at a time: growing from empty **1.96 ms**,
   reserved first **0.90 ms**, a vector that kept last frame's capacity **0.89 ms**, one straight copy
   **0.89 ms**. So growth/realloc is worth about **1.1 ms** of H's frame; the other 0.9 ms is the
   copying and page-touching of 6 MB that no allocation policy removes.
4. The HUD (1.96 ms) and the smoothing pass over the rest of the world (1.93 ms) are the other fixed
   lumps; terrain and fog are small.

### The render, split (H, per counted frame, llvmpipe)

| Renderer::render | mean ms |
|---|---:|
| upload (grow check + memcpy of 6 MB) | 0.953 |
| record | 0.041 |
| submit | 0.013 |
| wait (the GPU runs both passes, the filter, the copy) | 65.052 |
| readback (alloc + copy 3.7 MB) | 0.657 |
| sum / measured total | 66.716 / 66.719 |

The vertex buffer grew in 4 of the 200 counted frames. There is no present in this path: the window
copies `render`'s returned pixels into a staging buffer and blits them to the swapchain AFTER
`Renderer::render` returns, and that step is not in any number here (no display on this machine).
`Renderer::render` blocks on `vkWaitForFences` straight after the submit, so a frame is CPU THEN
GPU, strictly in series: the loop's own wall clock per frame (90.203 ms) equals CPU + render
(23.482 + 66.719 = 90.201 ms) to 0.002 ms.

### What the numbers SUPPORT and what they do not

Support: (a) the heavy battle's CPU side is dominated by rebuilding ~21 moving vehicles' meshes a
frame (10.0 of 17.2 ms of `build_scene`, 6.25 of it smoothing), not by emission copies (1.9 ms) or
buffer growth (~1.1 ms); (b) `smooth_ms` hides most of the smoothing now; (c) the frame is serial, CPU
then GPU, and `wait` is the only large render phase -- upload, record, submit and readback together
are ~1.7 ms at H; (d) on llvmpipe the render has a 14.4 ms fixed part plus a per-triangle part that is
not linear.

**Is 38 ms reachable to 33 ms or below by CPU work alone? The numbers support "plausibly yes", but
cannot prove it on the GTX 1060.** The brief's H frame is 18.25 ms CPU + 19.99 ms render; 33 ms needs
5.3 ms out of the CPU side. Scaling this machine's proportions to the brief's `build_scene` (14.1 ms),
the miss path is ~8 ms of it, so making a moving vehicle stop costing a build-plus-smooth-from-scratch
each frame has more than enough in it -- the plain `reserve`/reuse of the vertex list alone is worth
~1 ms, about 0.9 ms on the brief's scale. Against that: this box cannot say what the GTX 1060's fixed
render cost is (the 20 ms it showed for H may be mostly fixed, mostly per-vertex, or mostly the blit
and readback that sit outside `Renderer::render`), and a CPU change that must stay byte-identical (this
project compares frames byte for byte, which is why N10's key carries the pose) cannot simply
translate a template. **If the CPU target proves out of reach, the thing the code and the
serial-frame identity above do support is that CPU and GPU never overlap: overlapping frame N's
render with frame N+1's CPU work would cost max(18.3, 20.0) rather than the sum (~20 ms, ~50 fps on
the brief's numbers) without reducing either side -- but that is a change to the render path
(a second vertex and readback buffer), so it is named here and not done.**

### Raw output

`protekt-framecost floor` (ascending and descending tables above; fits):

```
fit ascending run   mean    intercept 16.988 ms, slope 0.9690 ms/1000 tri (323.01 ns/vertex), R^2 0.9478
fit ascending run   median  intercept 16.896 ms, slope 0.8886 ms/1000 tri (296.20 ns/vertex), R^2 0.9325
fit ascending run   wait    intercept 16.354 ms, slope 0.9533 ms/1000 tri (317.77 ns/vertex), R^2 0.9461
fit ascending run   upload  intercept  0.002 ms, slope 0.0157 ms/1000 tri (  5.22 ns/vertex), R^2 0.9999
fit descending run  mean    intercept 17.037 ms, slope 0.9228 ms/1000 tri (307.62 ns/vertex), R^2 0.9377
fit descending run  median  intercept 16.871 ms, slope 0.8676 ms/1000 tri (289.20 ns/vertex), R^2 0.9279
```

`protekt-framecost battle`, S: skirmish (53,253 vertices): build_scene 5.105 ms (units 1.127: key+find
0.001, template build 0.935 = builder 0.183 + scale/tilt 0.160 + smooth 0.591, copy 0.143, live 0.042;
smoothing pass 1.999, hud 1.304); growth from empty 0.854 ms vs reserved 0.323 ms; render 33.309 ms
(upload 0.301, record 0.036, submit 0.011, wait 32.339, readback 0.618; sum 33.306); CPU 6.588 + render
33.309 = 39.896 ms = 25.1 fps on llvmpipe.

### Sabotage, recorded

Both measurements were broken on purpose and the report's own arithmetic stopped adding up, then were
restored and added up again. (1) `wait_ms` forced to 0: "sum of the render phases 1.657 ms (mean total
66.184)", against 66.716 / 66.719 restored. (2) `template_smooth_ms` dropped: the residual row
"template insert etc." jumped from 0.011 ms to 6.294 ms (the smoothing, now unaccounted), and "of which
smooth alone" read 0.000; restored it reads 6.250 and 0.011.

## N12: the frame is read back only when someone asks for it

`FrameRequest::readback` (default true) gates the GPU-to-host copy. False records no
`vkCmdCopyImageToBuffer` and no host barrier, does not allocate or memcpy the result, and returns an
empty vector; `FrameStats::readback_copies` (0/1) and `FrameStats::completed` say what happened.
Setters: `protekt-game` headless sets it only with `--capture` (`PresenterOptions::readback`);
`protekt-framecost` defaults to NO and `--readback` restores the old frame. Everything else (tests,
`protekt-render`, the window, which needs the pixels to blit) keeps the default true.
Layout: the HUD pass still ends in TRANSFER_SRC_OPTIMAL; with no copy the image stays there unread,
and the next frame's filter pass declares initialLayout UNDEFINED on it, so nothing depends on it.
The fence is STILL waited on (the frame must finish before its vertex buffer and command buffer are
reused); what is gone is the copy, its barrier, and the allocate+memcpy.

Measured HERE (llvmpipe, software Vulkan, Release -O3; NOT his GTX 1060), same scenarios and flags:

| | OLD (`--readback`) | NEW |
|---|---|---|
| floor intercept (mean, asc/desc fit) | 17.14 / 17.11 ms | 15.96 / 15.88 ms |
| floor, 0 triangles, render mean | 14.70 ms | 12.89 ms |
| floor readback phase, 0 triangles | 0.606 ms | 0.000 ms |
| S skirmish render / readback / whole frame | 32.994 / 0.625 / 39.298 ms (25.4 fps) | 31.556 / 0 / 37.956 ms (26.3 fps) |
| M battle render / readback / whole frame | 40.667 / 0.667 / 50.418 ms (19.8 fps) | 38.899 / 0 / 48.727 ms (20.5 fps) |
| H heavy render / readback / whole frame | 72.736 / 0.689 / 96.570 ms (10.4 fps) | 74.798 / 0 / 99.975 ms (10.0 fps) |

**What this does NOT show.** On llvmpipe the readback is a 0.6 ms memcpy of host memory, not 10-15 ms,
so the floor's intercept did not collapse here (about 1.2 ms of 17), and H got no faster (the 2 ms
difference is inside run-to-run noise; its worst frame is 114-117 ms). The finding that the readback is
~11-15 ms on his GTX 1060 is a property of that driver's mapped memory and is NOT reproduced on this
machine; it has to be re-measured there (`protekt-framecost floor` vs `... floor --readback`). Also,
the windowed game still reads back every frame, because the window blits the returned pixels.
