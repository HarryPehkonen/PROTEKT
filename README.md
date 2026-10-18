# PROTEKT

PROTEKT — a single-player, first-person hover-tank game in the spirit of *Battlezone* and
*Battlezone II: Combat Commander*: you drive one vehicle, command base units and turrets, and fight an
opponent on a world that is **reproducible from a seed**.

The seed is the point. The same seed produces the same world, bit-for-bit, on any machine at any
optimisation level — which is why world generation is a pure integer function, and why the simulation
uses fixed-point arithmetic and no floating point anywhere.

## How to play

You are one tank. Everything else you command.

| input | what it does |
|---|---|
| `W` / `S` | throttle forward / back |
| `A` / `D` | turn left / right |
| **left click** with the mark on an own unit | select it |
| **left click** + `Shift`, mark on a unit | add it to the selection, or remove it |
| `Tab` / `Shift`+`Tab` | cycle the selection through your units |
| `Escape` | clear the selection |
| `F11` or `Alt`+`Enter` | full screen, and back again |
| `Ctrl`+`Q` | quit from anywhere |
| **right click**, mark on the ground | order every selected unit there |
| **right click**, mark on a hostile | order every selected unit to attack it |
| **left click**, mark on a hostile | shoot at it: the gun fires at whatever the mark covers |

The tank you are sitting in is **never** moved by a click. It is driven by `W`/`A`/`D` every tick, so
an order from the mouse would fight your own hands. Selecting it is fine, and firing is the one thing
a click may tell it to do — that order sets a target and nothing else.

Your gun also fires on its own at any hostile within twelve tiles. Clicking a hostile is how you
*choose* between them; `Escape` and Stop are how you hold fire.

Selected units are drawn brighter than the rest of their side, so you can see what you have hold of.

**The mark is the gun.** It sits in the middle of the screen horizontally, and the mouse moves it up
and down — the tank's gun is on its centre line, so **turning with `A`/`D` is how you aim sideways**:
bring what you want under the mark, then click. The mark's colour says what a click would do before you
make it:

| mark | what a click does |
|---|---|
| **white** | select nothing, or order your selection to that ground — nothing is harmed |
| **green** | select one of your own units (or, with `Shift`, add it to the selection) |
| **red** | **fire** at the hostile under it |

A small square outline, and the only thing drawn close to your eye: if you can see it, that is where
your click lands. The pointer does not aim — a click anywhere on the screen does what the mark shows,
which is why the mark cannot promise something the shot cannot do.

## The radar

Bottom-left corner, and it is a **radar, not a map**. It is centred on the tank you are sitting in
and reaches **40 tiles** in every direction, and what is inside that radius is all it will ever show
you. A hostile beyond it is not dimmed, not on the rim and not hinted at: outside the radius you know
nothing, which is the entire reason the radius exists.

- **green triangle** at the middle — you, always pointing up: **the panel turns with you**, so a
  mark's direction is a direction to turn, with no mental arithmetic in the middle of a firefight.
- **green square** — one of your units, in range.
- **red square** — a hostile, in range. The only way a hostile ever appears.
- **green outline** — your base, in range: a shape as well as a colour, so it reads at a glance.
- **green arrow at the rim** — your base, out of range. Your own base is the one thing you are never
  lost about, so it is always drawn or always pointed at. Enemy assets get no arrow, ever.

Distance on the panel is to scale (half the radius is half the way out); the marks themselves are
sized to be read rather than to be exact.

## Sound

Synthesised, with **no assets**: every sound is arithmetic (a seeded noise, a decay and a sine), the
same bytes on every machine, so there is nothing to license and nothing to go missing from a
checkout. It plays through SDL3 when there is a window and an output device; with neither the game
says so once at startup and runs silent. `--no-sound` never opens the device.

What you hear:

- **your gun** — a crack over a falling tone, full volume and in the middle. So is a friend's, while
  they are within earshot.
- **being hit** — a low thud with a sharp attack and a high ping on top, full volume, in the middle.
  It is the same whichever way the shot came from, because the thing that is hurting you is not
  something to locate, only to know.
- **a hostile's gun that missed** — the same crack, quieter with distance and placed left or right
  by where it came from. Dead ahead and dead behind sound alike: there is a left and a right and
  nothing else.
- **something dying** — a roar. Yours is at full volume; anyone else's is quieter with distance and
  placed like a gun, and carries 90 tiles where a shot carries 60.

What you do not hear: **anything beyond earshot — not quiet, not there.** It is the radar's promise
made to the ears: a battle you cannot perceive must not be audible, or the ears become a map of
where the enemy is.

## Status

**Playable, incomplete.** What genuinely works, and what does not — because a status line nobody can
check is decoration:

**Works.** Drive the tank over a seed-generated world; select units and give orders; units fight, take
damage and die; the enemy commander advances, engages and withdraws; the whole simulation is
deterministic (a seed plus its orders replays byte-for-byte); the window and the headless renderer
produce byte-identical frames **and the same world state** — for a scripted run, because `--autodrive`
drops real input: a stray key arriving at a window that has just taken focus must not be able to
change a verification run.

**Not there yet.**

| gap | what it means when you play |
|---|---|
| **No route planning** | a unit goes straight at its goal, slides along a rim, and takes a short detour around a corner it cannot pass. It does not plan a path: a concave corner cannot hold a unit, but a U-shaped crater field can still keep one inside the pocket. `tests/test_steering.cpp` draws the shapes, `tests/test_navigation.cpp` pins the open-ground and corner cases |
| **No economy** | no resources, no harvester, no building. A factory is a 400 hp target and nothing more |
| **No way to win or lose** | nothing ends a match. An undefended factory can be destroyed by a single tank — factories are unarmed on purpose and defence is what turrets are for — but there is no objective beyond that |
| **No HUD** | refusals ("that order would move the tank you are in") are printed to the console, not to the screen. There is a mark at the aim point and nothing else: no health, no marker on the unit your gun is engaging, no counters |
| **No on-screen feedback** | there is sound (see above) and no on-screen sign that a shot happened — the reticle says what a click *would* do, not what one just did |
| **No positional audio beyond left and right** | a sound is placed on a line between the speakers: no front or back, no height, no echo or occlusion. A shot behind a hill sounds exactly like one in the open |
| **Sounds are generated, not recorded** | they are made of noise and sine waves and sound like it. A recording would sound better and bring files, licences and a loader; that trade has not been made |
| **Silence beyond earshot is deliberate** | 60 tiles for a shot, 90 for a death; past it there is no voice at all. It is not a bug to report when a distant fight makes no noise |
| **Sounds queue, they do not overlap across frames** | the device plays what it is given in order, so a burst of fire within one sound's length waits its turn, and a buffer arriving with more than about three quarters of a second already queued is dropped rather than played late |
| **The radar is range-based, not line-of-sight** | a hostile behind a hill inside the 40 tiles is still on it. Battlezone's radar worked the same way, and this is the version to play before adding occlusion |
| **No compass** | the panel turns with the tank and there is no north tick: there is nothing to correlate a bearing with until there is a map |
| **Two units on one panel pixel** | the depth test decides, so one colour wins. A hostile within about a tile of you hides under your own mark |
| **No mouse look** | the mark is the middle of the screen horizontally and the mouse only moves it up and down; nothing turns the view but `A`/`D` — deliberately, until it has been played and judged |
| Text is not drawn at all | there are coloured boxes and a horizon, and no font. Flat colours, no textures, no lighting |

## Requirements

- CMake ≥ 3.20, a C++20 compiler (clang or gcc), GoogleTest (Debian: `libgtest-dev`)
- `clang-format` and `clang-tidy` for the full gate
- **optional:** SDL3 (a real window) and Vulkan (rendering). Both are optional by design — without them
  the game still builds and runs headless, which is how the gate tests it on a machine with no GPU

## Build, run, watch

```bash
cmake -S . -B build
cmake --build build -j"$(nproc)"
```

Then any of:

```bash
# the game, headless: render a frame to a PPM, and prove the world moves while you drive
./build/protekt-game --headless --seed 42 --frames 300 --autodrive --capture /tmp/frame.ppm

# the game with a window (needs SDL3 + Vulkan; --gpu picks a device by substring)
./build/protekt-game --seed 42

# full screen from the start (F11 and Alt+Enter toggle it while playing; Ctrl+Q quits)
./build/protekt-game --seed 42 --fullscreen

# a run nobody is sitting at: press one key on the first frame, so a keypress is reproducible
./build/protekt-game --headless --seed 42 --frames 90 --autodrive --press ctrl+q

# the radar off, for comparing two frames pixel by pixel (the game always has one)
./build/protekt-game --headless --seed 42 --frames 60 --autodrive --no-radar --capture without.ppm

# no sound device opened, whatever the machine has (a window run says so once if it finds none)
./build/protekt-game --seed 42 --no-sound

# a frame with the mark in it, on a machine with no mouse at all: --cursor places the pointer (the
# mark stays in the middle horizontally and takes only the height, so the x of --cursor is ignored)
./build/protekt-game --headless --seed 42 --frames 300 --autodrive --cursor 640 360 --capture /tmp/aim.ppm

# the battle as text: the simulation printed by a human-readable log
./build/protekt-sim --seed 42 --ticks 1800 --commander utility    # scripted | utility | none

# the world as a map
./build/protekt-map --seed 42 --size 32 --chunk 0 0

# the commander comparison: "is the AI better?" as numbers, with a do-nothing arm
./build/protekt-measure 2400
```

`--autodrive` holds `W`, which is how you check that driving actually moves the world:
`protekt-game --headless --seed 42 --frames 30 --autodrive --capture a.ppm` and the same at 300 frames
must differ.

`protekt-measure` is deliberate about its arms: one of them gives the enemy **no orders at all**,
because the obvious metric ("units lost") is won by a commander that does nothing. See
`docs/PLAN.md` for the measured table.

**On a machine with Vulkan but no development symlink** (some Fedora/Nobara installs): CMake falls back
to a hand-rolled probe, but a *stale* `build/` cache holds the old failed result. After any change to
`CMakeLists.txt`, delete it: `rm -rf build`.

## The gate

No external CI. One script is the single definition of "green", and the git hooks call it:

```bash
./tools/ci.sh              # every stage
./tools/ci.sh --list       # the stages
git config core.hooksPath .githooks   # once per clone: enables pre-commit/pre-push
```

Stages: `tree format kitprobes build tests release version asan tsan tidy pristine`. `pristine` is the
stage that matters most and looks the least impressive — it builds a clean clone of what git actually
carries, which is the only way to catch a file that passes locally because it is on your disk.

Per-machine settings go in `.ci.env` (gitignored; see `.ci.env.example`).

## Layout

```
include/protekt/  public headers, one directory per layer
src/              implementation: world/ sim/ ai/ cmd/ game/ input/ render/ app/
tests/            unit, integration and characterization tests
fuzz/             libFuzzer targets
tools/ci.sh       the gate
docs/PLAN.md      the plan, the measured results, and the known gaps
```

The architecture in one line: **the simulation is the game, and every front end is a client of it.**
The renderer, the text log, the AI and your own hands all read the same state and issue orders through
the same `ICommander` interface.

## License

[The Unlicense](LICENSE) — public domain.
