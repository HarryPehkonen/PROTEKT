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
