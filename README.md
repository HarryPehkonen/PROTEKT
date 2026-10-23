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
| **mouse** | turn the turret, relative to the body: the pointer is captured, and the crosshair is always the centre of the screen, where the turret points |
| `A` / `D` | turn the body of the tank left / right, on the spot or while driving; the turret goes round with it |
| `E` (**held**) | boost: lift the hull, and settle back down when you let go |
| `C` | toggle the colour calibration view: three red/green/blue bars, a check on the display path |
| **left click** | **fire**, wherever the mark is (see below) |
| `Q` | switch weapon: gun -> rocket -> mortar -> bouncer -> gun. The crosshair and the label beside it change; the mortar's crosshair is a range ladder, each rung the distance in tiles a shell fired at that view pitch lands on level ground, the lit rung being where you look now. The trigger fires the gun's and the rocket's auto-aimed shot as ever; with the mortar up it lobs a shell along your own view (heading and pitch, no aim-assist, no lead): point up, fire, and the rung you lit is where it lands. The bouncer is lobbed the same way, by hand, and fires a bouncing round instead of a contact one |
| mouse wheel | switch weapon, one per notch: wheel up = previous, wheel down = next, the same wrap as `Q` |
| `Space`, mark on an own unit | add it to the selection, or remove it if it is already in (`Shift`+`Space` does the same); ground, enemies and the tank you drive are ignored |
| `F1`-`F10` | select every unit of one KIND you own: the function keys are dealt from a pool in spawn order, and a kind that loses its last unit returns its key to the pool (so a wipe can move a kind down a key); the strip along the top shows each kind's key, name, count and health. The digit row no longer selects vehicles: digits answer `G`, `T` and the attack list |
| `X`, mark on an own armed unit | swap: drive that unit instead; the one you leave stops, and becomes an ordinary unit you can command. Any of your tanks, hover tanks, turrets, mortars and rockets can be driven. It refuses an enemy unit, your factory (unarmed and immobile, so nothing to drive), a dead unit, the unit you are already driving, and an empty mark |
| `F`, mark on a unit | every selected unit follows that unit: it holds within 4 tiles of it, closing when it is further and standing still inside, and its gun fires at whatever is in range meanwhile. Any living unit of either side may be followed (so a wingman can shadow the hull you left after `X`), but not the follower itself. It refuses with nothing selected, with nothing living under the mark, or when you are driving the only selected unit |
| `N` | drop a numbered nav mark (0-9) on the ground under the crosshair, or clear the one you are aiming at. Marks are your own annotation: the sim never sees them, they show on the radar and the world map with their numbers, and ten is the limit |
| `G`, then `0`-`9` | send every selected unit to that nav mark, as a move order. `G` waits for one key: a digit answers it, `Escape` cancels, and any other key cancels and then does its own job. It refuses when that number is not placed, or with nothing selected |
| `M` | order every selected unit to move to the ground under the mark, even when a unit is in front of it |
| `T` | order every selected unit to attack the unit under the mark -- a hostile, or one of your own. With nothing hostile under the mark, arm the attack list instead: the nearest enemies within radar range are numbered 1-9 and 0, and the next digit attacks that row. A hostile under the mark is always attacked directly |
| `Tab` or `Escape` | clear the selection |
| `R` | open or close the map: the radar's view switches to the whole world as a square, north up, in the ground's own colours, with your units and buildings (the one you drive ringed in white) and your nav marks with their numbers. R again brings the radar back. It shows everything EXCEPT the enemy assets: the map is the view of what is yours, the radar is the view of what is near. It is only a picture: it gives no orders and cannot be clicked (a click still acts on the world behind it). `O` is BZ2's objectives key and does nothing yet |
| `F11` or `Alt`+`Enter` | full screen, and back again |
| `Ctrl`+`Q` | quit from anywhere |
| **right click**, mark on the ground | order every selected unit there |
| **right click**, mark on a hostile | order every selected unit to attack it |

Your other tanks retreat to your factory for repairs on their own (at 20 hp or less) and return to
their last order when healed (80 hp); the tank you are driving never does.

The tank you are sitting in is **never** moved by a click. It is driven by `W`/`S` and `A`/`D`
every tick, so an order from the mouse would fight your own hands: the tank always drives the way its
body points, and looking around with the mouse never changes where it goes. Selecting it is
fine, and firing is the one thing a click may tell it to do — one bolt, and nothing else. Looking keeps working after the tank is destroyed, and the radar and health bar stay up.

Your gun also fires on its own at any hostile within twelve tiles. **A click fires one shot**, and
keeping the button going keeps firing; the gun is back to the nearest hostile in between. `Escape`
and Stop are how you hold fire. It will **not** fire on your own units on
its own -- only an order crosses that line (see Display/HUD below).

Selected units are drawn brighter than the rest of their side, so you can see what you have hold of.

**The mark is the gun.** It is the crosshair, fixed at the centre of the screen, and you aim by
**looking**: the mouse swings the turret, and turning the body with `A`/`D` carries the turret round with it. **The shot goes where the
crosshair points and nothing aims for you.** A click on a unit means "that is what I am shooting at",
not "fly to it": it decides whom the shot may hurt (your own units included), never where it goes.
Bring the crosshair onto the target, and lead a moving one yourself. The mark's colour says what is
under it before you click:

| mark | what a click does |
|---|---|
| **white** | nothing is under it: the shot flies on and harms what it meets |
| **green** | **your own** unit is under it: you are shooting at it. You can destroy your own tank on purpose, though it takes repeated clicks |
| **red** | a hostile is under it |

The unit under the mark is tinted as well, so the warning is on the thing itself and not only on the
mark: **amber** for one of yours ("do not shoot this"), **salmon** for a hostile.

**The left button is the trigger, always.** It never selects: `Space` (and `Shift`+`Space`) does that.

**A shot is a real projectile, and it takes time to arrive.** The simulation owns it: it leaves the
gun as a small bright box, flies in a straight line at **16 tiles a second**, and the damage lands
when it gets there rather than when you pull the trigger — so a long shot is visibly slower than a
close one. It does **not** lead or follow anything — that is
the job of the gun's own auto-fire, not yours. It **inherits your own
velocity**, so firing sideways while you are driving throws the shot off by roughly your speed over
the flight, exactly as a real one would. It **keeps flying if its target dies first** and can hit
whatever else it meets, and it **fizzles after 16 tiles**, having harmed nothing. Your bolts are the
bright cyan ones and the enemy's are orange-yellow. A shot at the ground or the sky is still a shot:
you see it and hear it, it can hit anything in its path (your own side excepted, unless you named it)
and it fizzles at range. The button is rate-limited to about four shots a second;
your gun's own auto-fire is not. When you are hit the screen flashes red and fades over 0.4 seconds.

**Shooting never costs you your selection.** A click on the ground, the sky, a hostile or your own unit leaves it alone --
only `Tab` and `Escape` clear it. `T` reads the mark fresh every time it is pressed, so after a kill you can
put the mark on the next enemy and press `T` again without re-selecting anything.

The crosshair is the only thing drawn close to your eye. There is no pointer on screen — the mouse
turns the turret — so the crosshair is both where you look and where your click lands.

## The radar

Bottom-left corner, and it is a **radar, not a map** -- and **round**, a disc with a ring border, so the
corners of the square it sits in are not painted. It is centred on the tank you are sitting in
and reaches **40 tiles** in every direction, and what is inside that radius is all it will ever show
you. A hostile beyond it is not dimmed, not on the rim and not hinted at: outside the radius you know
nothing, which is the entire reason the radius exists.

- **green triangle** at the middle — you, always pointing up: **the panel turns with you**, so a
  mark's direction is a direction to turn, with no mental arithmetic in the middle of a firefight.
- **green square** — one of your units, in range.
- **red square** — a hostile, in range. The only way a hostile ever appears.
- **green outline** — your base, in range: a shape as well as a colour, so it reads at a glance.
- **numbered plate** — one of your nav marks, with its number, in range or pinned to the rim.
- **green arrow at the rim** — your base, out of range. Your own base is the one thing you are never
  lost about, so it is always drawn or always pointed at. Enemy assets get no arrow, ever.

Distance on the panel is to scale (half the radius is half the way out); the marks themselves are
sized to be read rather than to be exact.

## Display/HUD

The panel above is most of the HUD. Beyond it the screen carries a small amount of text, in a built-in
five-by-five block font: the weapon's name beside the crosshair, the mortar ladder's range in tiles
on each rung, a strip along the top with each living unit's key, kind and hit points, a
`DEPLOYING` plate over one of your turrets that is not yet mounted, and a notice line for a
refused roster or nav-mark key. The health bar below, and the radar, are colour and length only.

### The health bar

Directly above the panel, the same width as it: **the hull you are sitting in, and only yours.** A dark
track with a fill from the left -- **green** from half health up, **amber** from a fifth, **red** below.
The length is the proportion and the colour says which band you are in, so it reads at a glance while you
are being shot at. It takes the hull's hit points against its profile's maximum, the same numbers the sim
fights with, so it cannot drift from the truth. A hull at zero has no bar at all rather than an empty one,
because a dead unit is not on the screen to read.

### The colour calibration view

`C` toggles a diagnostic screen: three full-height vertical bars, left to right pure **red**, pure
**green**, pure **blue**. It exists because the finished frame reaches your screen through a
channel-order hand-off in the presentation path that **no unit test can see** -- the whole suite passed
while red and blue were swapped on a real display. The calibration view is the only check that goes all
the way to the monitor. On a correct build the bars read red, green, blue from the left; if red and blue
are the wrong way round on your screen, the swap is in the presentation path and not in the palette.
`C` again returns to the game. It never touches the simulation, so it costs no ticks and cannot change a
verification run.

### Friendly fire

**Your units will not fire on your own side on their own initiative.** Auto-engagement only ever picks a
hostile: a unit with nothing but friendlies in range holds fire. An explicit order is the one thing that
crosses the line -- `T` with the mark on one of your own units orders an attack on it, and so does an
explicit Attack order on a friendly. That is deliberate: it means you can destroy your own tank or your
own factory if you mean to, without your units ever turning on each other without being told. A unit may
never name *itself*, though, so an accidental order cannot make a tank shoot itself.

The mark still warns you: **amber** over one of your own units ("do not shoot this"), **salmon** over a
hostile. The warning is on the unit itself as well as on the mark.

## Sound

Synthesised, with **no assets**: every sound is arithmetic (a seeded noise, a decay and a sine), the
same bytes on every machine, so there is nothing to license and nothing to go missing from a
checkout. It plays through SDL3 when there is a window and an output device; with neither the game
says so once at startup and runs silent. `--no-sound` never opens the device.

What you hear:

- **your gun** — a short noise crack over a low thump that sags from 120 Hz to 60 Hz, full volume and
  in the middle. So is a friend's, while they are within earshot.
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
damage and die; a shot is a real projectile that takes time to arrive, leads its target, is carried by
the velocity of the tank that fired it and is drawn in flight; the enemy commander advances, engages
and withdraws; the whole simulation is
deterministic (a seed plus its orders replays byte-for-byte); the window and the headless renderer
produce byte-identical frames **and the same world state** — for a scripted run, because `--autodrive`
drops real input: a stray key arriving at a window that has just taken focus must not be able to
change a verification run.

**Not there yet.**

| gap | what it means when you play |
|---|---|
| **Unreachable goals are not planned for** | a move order is planned as a route over the tile grid (deterministic A*), so units go round craters and rims and out of pockets. A goal that cannot be reached at all gets no route: the unit steers straight at it and may stop against what is in the way. `tests/test_routing.cpp` pins the plan, `tests/test_navigation.cpp` and `tests/test_steering.cpp` the local detour it falls back on |
| **No economy** | no resources, no harvester, no building. A factory is a 400 hp target and nothing more |
| **No way to win or lose** | nothing ends a match. An undefended factory can be destroyed by a single tank — factories are unarmed on purpose and defence is what turrets are for — but there is no objective beyond that |
| **Most refusals are on the console, not the screen** | text is drawn (see Display/HUD), but only a refused roster F-key or nav-mark digit puts its sentence on screen; every other refusal ("that order would move the tank you are in") goes to the console. The radar and the health bar carry no text, and nothing marks the unit your gun is engaging |
| **Little feedback that YOUR shot happened** | the bolt is drawn, your gun is heard, the reticle spreads for 0.2 seconds when your shot damages an enemy and a mortar shell leaves a landing mark. There is no muzzle flash and no recoil, and a shot that misses gives no reticle response |
| **No positional audio beyond left and right** | a sound is placed on a line between the speakers: no front or back, no height, no echo or occlusion. A shot behind a hill sounds exactly like one in the open |
| **Sounds are generated, not recorded** | they are made of noise and sine waves and sound like it. A recording would sound better and bring files, licences and a loader; that trade has not been made |
| **Silence beyond earshot is deliberate** | 60 tiles for a shot, 90 for a death; past it there is no voice at all. It is not a bug to report when a distant fight makes no noise |
| **Sounds queue, they do not overlap across frames** | the device plays what it is given in order, so a burst of fire within one sound's length waits its turn, and a buffer arriving with more than about three quarters of a second already queued is dropped rather than played late |
| **The radar is range-based, not line-of-sight** | a hostile behind a hill inside the 40 tiles is still on it. Battlezone's radar worked the same way, and this is the version to play before adding occlusion |
| **Compass** | the panel turns with the tank; N, E, S, W sit on the ring for the true world directions, from the same yaw as the marks |
| **Two units on one panel pixel** | the depth test decides, so one colour wins. A hostile within about a tile of you hides under your own mark |
| **Lighting is one sun, no shadows** | the ground is painted from textures (six variants of each terrain type, each tile turned one of eight ways) and units are built from several lit prisms, all shaded by one directional sun. Nothing casts a shadow, and the textures are generated, not authored art |

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

# the weapon switched N times before the frame (0 gun, 1 rocket, 2 mortar, 3 bouncer), to capture a reticle
./build/protekt-game --headless --seed 42 --frames 30 --cursor 640 360 --switch 2 --capture /tmp/mortar.ppm
# a mortar shell in the air: view tilted up 1.40 rad (clamps at the 1.3 limit), trigger on frame 18
./build/protekt-game --headless --no-sound --seed 42 --switch 2 --look-up 140 --fire 18 --frames 40 --capture /tmp/lob.ppm

# the same shell at the top rung (1.28 rad, lands 13.9 tiles out), the view levelled on frame 25 to watch it land
./build/protekt-game --headless --no-sound --seed 42 --switch 2 --look-up 128 --fire 18 --look-level 25 --frames 90 --capture /tmp/lob-high.ppm

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

## The texture atlas (assets/)

The atlas is a 512x512 RGBA PNG of 8x8 tiles of 64x64, in four 4x4 quadrants (tiles 0-15 are the
top-left one, 16-31 the top-right, and so on; unassigned tiles are magenta), and
`assets/atlas_manifest.txt` says which tile each tank part uses. At startup the game looks for `atlas.png` and `atlas_manifest.txt` in
`./assets` (the working directory) and then in `assets/` next to the executable; a file that is
missing or unusable falls back, with one log line, to the copy embedded in the binary, so the game
never refuses to start over an asset. Drop in a different PNG of the same layout and it takes
effect on the next start, with no recompile.

`assets/atlas.png` is generated, not hand-edited: `./build/protekt-atlas-gen [--seed N] [--out PATH]`
writes it (deterministically) from the same in-memory builder the embedded fallback uses. The tiles
are painted, not flat: three hull camo variants (`tank_hull_a` splodges, `_b` stripes, `_c` blocks;
each tank wears one, picked from the game seed and its unit id), a track tile with link stripes, and
metal and building tiles with a few percent of tone variation, and the hover tank's bright skirt
band (`hover_skirt`). `--seed` repaints the patterns; the
committed file is the default seed, 1.

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

**Third-party code.** `third_party/stb/` holds two single-header libraries by Sean Barrett,
[stb](https://github.com/nothings/stb): `stb_image.h` (v2.30) and `stb_image_write.h` (v1.16). stb is
dual-licensed, public domain (Unlicense) or MIT, at your option; the full text is at the end of each
header. They are used only to read and write the texture atlas PNG.
