# Command-line reference

Every binary in `build/`, every flag each one accepts, and what each refuses and says. Everything
here was read out of the parsers and then run; the outputs pasted below are real. A test
(`CliDoc.EveryFlagAParserAcceptsIsInTheDocumentAndNothingElseIs`, in `tests/test_cli.cpp`) fails if a
parser gains or loses a flag and this file does not follow.

There is no help flag on any binary: `-h`, `-x` and any other unrecognised option are refused the
same way. Real output of an unrecognised option, one per binary:

```
$ protekt-game -x
protekt-game: unknown option '-x'
[exit 2]
$ protekt-sim -x
protekt-sim: unknown argument '-x'
[exit 2]
$ protekt-render -x
protekt-render: unknown argument '-x'
[exit 2]
$ protekt-atlas-gen -x
usage: protekt-atlas-gen [--seed N] [--out PATH]
[exit 2]
```

Flags belong to ONE binary each. A flag given to the wrong binary is refused, not ignored (except by
`protekt-map`, see its section):

```
$ protekt-game --status-period 60
protekt-game: unknown option '--status-period'
[exit 2]
```

(`--status-period` is `protekt-sim`'s.)

| binary | what it is for | reach for it when |
|---|---|---|
| `protekt-game` | the game: a window, or `--headless` frames | you want to play, or capture a frame of the real game (HUD, units, sky) |
| `protekt-sim` | the battle with no picture: prints the battle log, records and replays | you want the text log, a determinism check, or to watch the AI fight |
| `protekt-render` | one terrain frame, straight from the renderer, to a PPM | you are testing the renderer or a GPU, not the game |
| `protekt-map` | one chunk of the world as ASCII, plus a region hash | you want to see a seed's terrain, or check that two processes agree on it |
| `protekt-measure` | the commander measurement: scripted vs utility AI, over ten seeds | you want "is the AI better" as a number |
| `protekt-meshcost` | a timing probe for the radar wireframe | you are tuning the radar mesh; it is run by hand and asserts nothing |
| `protekt-atlas-gen` | writes the texture atlas as a PNG | the atlas pixel builder changed |

`protekt-render` is only built when the renderer is (`PROTEKT_RENDER`); the others always are.

## Determinism, in three lines

1. The seed owns the world: terrain, both bases, where the enemy sits.
2. The world is a pure function of `(seed, chunk)`: `protekt-map --seed 42 --size 8` prints the same
   bytes in every process.
3. The same seed plus the same commands is the same battle (`protekt-sim` prints the state hash on its
   last line to prove it).

The honest caveat on (3) is in [Replay a battle](#4-replay-a-battle).

## Exit codes

They are not a uniform 0/1/2. Each was produced by the command shown.

| code | meaning | produced by |
|---|---|---|
| 0 | success | `protekt-sim --seed 42 --ticks 300` |
| 1 | failed at the end, after starting | `protekt-game --headless --frames 1 --capture /nonexistent/x.ppm` ("could not write"); `protekt-game` on a scenario with no player tank; `protekt-sim --ticks 10 --replay-out /nonexistent/x.replay`; `protekt-render --out /nonexistent/x.ppm` |
| 2 | usage: the command line was refused before any work | `protekt-game -x`, `protekt-sim --seed abc`, `protekt-map --seed abc`, `protekt-render --width 0`, `protekt-measure abc`, `protekt-atlas-gen -x`; and `protekt-game --scenario FILE` when the file is unreadable or invalid |
| 3 | an input or output file was refused | `protekt-sim --replay /nonexistent.replay` and `--replay` of a file that is not a replay; `protekt-atlas-gen --out /nonexistent/a.png` |
| 4 | no usable Vulkan device or window | `protekt-game` with no display and no `--headless`; `protekt-game --headless --frames 1 --gpu nosuch`; `protekt-render --gpu nosuch`; `protekt-render --list-gpus` with no Vulkan driver |

Printed output of the 4s:

```
$ protekt-game --headless --frames 1 --gpu nosuch
protekt-game: no Vulkan device matches 'nosuch'; available: 'llvmpipe (LLVM 19.1.7, 256 bits)'
[exit 4]
$ protekt-game        # DISPLAY and WAYLAND_DISPLAY empty
protekt-game: SDL_Vulkan_CreateSurface failed: Vulkan can't find a valid physical device (gpu).
protekt-game: no window, no game; try --headless to run without a display
[exit 4]
$ VK_ICD_FILENAMES=/nonexistent VK_DRIVER_FILES=/nonexistent protekt-render --list-gpus
protekt-render: no Vulkan device: vkCreateInstance failed (-9): no Vulkan loader or driver is usable
[exit 4]
```

On a first run from a directory with no `assets/`, `protekt-game` and `protekt-render` also print two
`protekt: no usable assets/...; using the embedded ...` lines to stderr. They are noise, not
failures. The outputs below were run from the repo root, where they do not appear.

---

## protekt-game

```
protekt-game [flags]
```

Defaults: seed 42, 1280x720, one tick per frame. With no `--frames` it runs until the window closes,
which needs a display; **`--headless` always needs `--frames N`**:

```
$ protekt-game --headless
protekt-game: --headless needs --frames N (there is no window to close)
[exit 2]
```

A run with `--frames N` is a scripted run: exactly `N` frames, no wall clock, so a capture is
reproducible. Nothing is written unless `--capture` is given.

Every flag that takes a value, given as the last argument with nothing after it, prints
`protekt-game: <flag> needs a value`; the numeric ones then ALSO print their own `wants` line (two
lines, exit 2). `--seed`, `--scenario`, `--capture` and `--gpu` print only the first. The tables give
the `wants` line (the refusal for a bad value). All refusals are exit 2 unless noted.

"Frame" arguments are 1-based: frame 1 is the first frame drawn.

### World and determinism

| flag | argument | what it does | refusal |
|---|---|---|---|
| `--seed` | `N` (unsigned 64-bit) | the world seed (default 42) | `protekt-game: --seed wants a number, got 'abc'` |
| `--scenario` | `FILE` | play the composition in a TOML file instead of the built-in one; there is no search path | `protekt-game: --scenario FILE: <reason>` (exit 2, before any window opens) |
| `--ticks-per-frame` | `N` (positive) | sim ticks per frame in a scripted run (default 1); with no `--frames` it has no effect, play runs on the wall clock at 30 Hz | `protekt-game: --ticks-per-frame wants a positive number` |

### Camera and capture

| flag | argument | what it does | refusal |
|---|---|---|---|
| `--headless` | none | no window: render offscreen. Needs `--frames`. Must come BEFORE `--fullscreen` | `protekt-game: --headless needs --frames N (there is no window to close)` |
| `--frames` | `N` (0 = until the window closes) | run exactly `N` frames | `protekt-game: --frames wants a number` |
| `--width` | `N` (positive) | frame width in pixels (default 1280); in a window, the window may override | `protekt-game: --width wants a positive number` |
| `--height` | `N` (positive) | frame height in pixels (default 720) | `protekt-game: --height wants a positive number` |
| `--capture` | `FILE` | after the last frame, write it as a binary PPM (P6) and print `wrote FILE (WxH, tick T, hash H)` | `protekt-game: could not write FILE` (exit 1); `protekt-game: nothing was rendered, so there is nothing to capture` (exit 1) |
| `--look-up` | `N` (unsigned, hundredths of a radian) | tilts the view up by `N/100` rad on the first frame. UNSIGNED: it cannot look down | `protekt-game: --look-up wants hundredths of a radian` |
| `--look-yaw` | `N` (signed, hundredths of a radian) | turns the camera `N/100` rad from where the turret points, on every frame; negative is left. Camera only: the sim hash does not change | `protekt-game: --look-yaw wants hundredths of a radian` |
| `--look-level` | `FRAME` (0 = never) | on that frame, brings the view down by exactly what `--look-up` raised it | `protekt-game: --look-level wants a frame number` |
| `--no-radar` | none | no radar (34102 pixels differ from the default at seed 42, 3 frames) | none |
| `--no-radar-mesh` | none | keep the radar but drop its terrain wireframe (14945 pixels differ) | none |
| `--pulse` | `PCT` (0 to 100) | hold the reticle's pulse at that percentage (190 pixels differ at 50) | `protekt-game: --pulse wants a percentage from 0 to 100` |
| `--gpu` | `NAME` | use the Vulkan device whose name matches; a part of the name works (`llvm` matched `llvmpipe (LLVM 19.1.7, 256 bits)`) | `protekt-game: no Vulkan device matches 'NAME'; available: '...'` (exit 4) |
| `--cursor` | `X Y` (integers) | **accepted and validated, and has no effect**: see [Discrepancies](#discrepancies-between-a-flags-name-or-comment-and-what-it-does) | `protekt-game: --cursor wants two numbers: pixels from the left and the top` |

### Scenario and replay

`--scenario` and `--log-spawns` are the scenario flags and are listed here for lookup; the world
table above lists `--scenario` too.

| flag | argument | what it does | refusal |
|---|---|---|---|
| `--log-spawns` | none | print every spawned unit once, after the world is built and before frame 1, in the battle log's own `tick 0 status unit ...` grammar | none |

`protekt-game` has **no replay flag**: it neither records nor replays. See
[Replay a battle](#4-replay-a-battle).

### Orders and gestures

Scripted input for a run with nobody at the keyboard. Within one frame the keys arrive in a fixed
order: `--autodrive`'s W, then `--nav`, `--go`, `--map`, `--targets`, then `--digit`s, then `--key`s.

| flag | argument | what it does | refusal |
|---|---|---|---|
| `--autodrive` | none | holds W for the whole run (the sim hash changes) | none |
| `--switch` | `N` (0 = none) | a right click on each of the first `N` frames: switches weapon (gun, rocket, mortar, bouncer, shotgun) | `protekt-game: --switch wants a number of weapon-key presses` |
| `--fire` | `FRAME` (0 = never) | a left click on that frame: pulls the trigger. What happens depends on the weapon: with the mortar just selected, `--fire 6` printed `protekt-game: the mortar is reloading.` and left the hash unchanged | `protekt-game: --fire wants a frame number` |
| `--nav` | `FRAME` (repeatable) | presses N on that frame: arms nav placement | `protekt-game: --nav wants a frame number (1-based)` |
| `--go` | `FRAME` (repeatable) | presses G on that frame; answer it with a `--digit` on a later frame | `protekt-game: --go wants a frame number (1-based)` |
| `--map` | `FRAME` (repeatable) | presses R on that frame: opens the map, closes it on a second use | `protekt-game: --map wants a frame number (1-based)` |
| `--targets` | `FRAME` (repeatable) | presses T on that frame: arms the attack list | `protekt-game: --targets wants a frame number (1-based)` |
| `--digit` | `FRAME N` (repeatable; `N` 0 to 9) | presses the number key `N` on that frame | `protekt-game: --digit wants a frame number (1-based) and a key 0-9` |
| `--key` | `FRAME NAME` (repeatable) | presses a named key on that frame; `NAME` is `space`, `tab`, `tilde` or `f1` to `f10`, lower case only. A selection is made this way: the digits cannot select a vehicle | `protekt-game: --key wants a frame number (1-based) and one of space, tab, tilde, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10` |

### Sound and window

| flag | argument | what it does | refusal |
|---|---|---|---|
| `--fullscreen` | none | start in full screen | `protekt-game: --fullscreen needs a window; --headless has none` (only when `--headless` came earlier on the line) |
| `--no-sound` | none | never open the sound device. Headless runs never open one anyway, so there it changes nothing | none |
| `--press` | `f11`, `ctrl+q` or `alt+enter` | presses that key; `ctrl+q` quits at once: `protekt-game: Ctrl+Q, so quitting at frame 0, tick 0` (exit 0). See the discrepancy note: the key is pressed on EVERY frame | `protekt-game: --press wants one of f11, ctrl+q, alt+enter` |

### Diagnostics

`--log-spawns` (above), `--capture` (above) and `--gpu` (above) are the diagnostics. The line
`protekt-game: headless on llvmpipe (LLVM 19.1.7, 256 bits)` names the device a headless run used, and
`protekt-game: wrote ... hash H` carries the sim's state hash: two runs with the same seed, the same
flags and the same frame count print the same hash.

Unknown flag (exit 2): `protekt-game: unknown option '-x'`.

---

## protekt-sim

```
protekt-sim [--seed N] [--ticks N] [--commander none|scripted|utility]
            [--status-period N] [--replay FILE] [--replay-out FILE]
```

The built-in skirmish with no picture, printed as a text log on stdout. Every flag takes a value.
Defaults: seed 42, 900 ticks, status every 60 ticks, scripted commander on both sides. Any problem
prints `protekt-sim: <reason>` to stderr and exits 2 (a bad or missing replay file exits 3).

| flag | argument | what it does | refusal |
|---|---|---|---|
| `--seed` | `N` | the world seed (ignored when `--replay` is given: the replay carries its own) | `protekt-sim: bad --seed value 'abc'` |
| `--ticks` | `N` (0 to 1000000) | how many ticks to run | `protekt-sim: bad --ticks value '1000001'` |
| `--commander` | `none`, `scripted` or `utility` | who gives orders. `none`: nobody does. `scripted`: both sides. `utility`: scripted player against the utility AI as the enemy. Ignored when `--replay` is given | `protekt-sim: bad --commander value 'bogus' (none\|scripted\|utility)` |
| `--status-period` | `N` (0 to 4294967295) | a `status unit` line for every unit every `N` ticks; `0` prints events only | `protekt-sim: bad --status-period value '99999999999'` |
| `--replay` | `FILE` | run the battle a replay file describes (its commands stand in for the commanders) | `protekt-sim: cannot read replay 'FILE'` / `protekt-sim: bad replay 'FILE': <reason>` (both exit 3) |
| `--replay-out` | `FILE` | after a live run, write the seed and every order of both sides to `FILE` | `protekt-sim: cannot write replay 'FILE'` (exit 1, after the log was printed) |

Missing value: `protekt-sim: missing value for --seed`. Unknown argument, including a stray
positional: `protekt-sim: unknown argument 'extra'`.

---

## protekt-render

```
protekt-render [--seed N] [--width N] [--height N] [--out FILE] [--gpu NAME] [--list-gpus]
```

One frame of terrain for a seed, rendered with no game around it (no units, no HUD, no camera
control), written as a PPM. Defaults: seed 42, 640x400, `frame.ppm`. **There is no camera flag here:
no `--look-yaw`, no `--look-up`.** To aim at the sky, use `protekt-game`.

| flag | argument | what it does | refusal |
|---|---|---|---|
| `--seed` | `N` | the world seed | `protekt-render: --seed wants an unsigned integer, got 'abc'` |
| `--width` | `N` (positive) | frame width | `protekt-render: --width wants a positive integer, got '0'` |
| `--height` | `N` (positive) | frame height | `protekt-render: --height wants a positive integer, got '0'` |
| `--out` | `FILE` | where to write the PPM | `protekt-render: could not write 'FILE'` (exit 1) |
| `--gpu` | `NAME` | the Vulkan device to use | `protekt-render: cannot create a renderer: no Vulkan device matches 'NAME'; available: '...'` (exit 4) |
| `--list-gpus` | none | print the devices (`NAME (discrete\|integrated\|cpu)`) and exit | `protekt-render: no Vulkan device: <reason>` (exit 4) |

Missing value: `protekt-render: --seed needs a value`. Unknown: `protekt-render: unknown argument '-x'`.

---

## protekt-map

```
protekt-map [--seed N] [--size N] [--chunk CX CY]
```

Prints `seed 0x..  chunk (cx,cy)  SxS`, then the top-left `S` by `S` tiles of the chunk as ASCII
(`~` deep water, `-` shallow, `.` flat, `+` rising, `^` high), then `hash 0x...`, which is the world's
region hash at `(CX+1, CY+1)`. Defaults: seed 1, size 32, chunk (0,0). A chunk is 64 tiles a side.
Any `int` chunk is accepted, including `2147483647` and `-2147483648`. The hash covers every chunk in
`[0, CX+1) x [0, CY+1)`, so it costs time proportional to that area: a large positive `CX` *and* `CY`
runs for a very long time, while a coordinate of `-1` or below makes the region empty and the run instant.

| flag | argument | what it does | refusal |
|---|---|---|---|
| `--seed` | `N` | the world seed (default 1, not 42) | `protekt-map: --seed wants an unsigned integer, got 'abc'` |
| `--size` | `N` | how many tiles to print per side; clamped to 2..64 | `protekt-map: --size wants an integer, got 'abc'`; a number that parses is clamped, not refused (`--size 1` prints 2, `--size 9999` prints 64), but one that does not fit an `int` is refused |
| `--chunk` | `CX CY` | which chunk | `protekt-map: --chunk wants two integers, got 'a' 'b'` |

An unknown option, a bad number, or an option missing its value is refused with exit code 2 and
nothing on stdout. A missing value reads `protekt-map: --seed needs a value` (likewise `--size`,
`--chunk`), an unknown option `protekt-map: unknown option '-x'`:

```
$ protekt-map --seed abc --size 4 ; echo $?
protekt-map: --seed wants an unsigned integer, got 'abc'
2
$ protekt-map -x --size 3 ; echo $?
protekt-map: unknown option '-x'
2
```

---

## protekt-measure

```
protekt-measure [TICKS]
```

No flags: one optional positional, the tick count (default 2400). It runs five arms (placebo enemy,
scripted, utility enemy, utility player, utility vs utility) over ten fixed seeds and prints units
lost and first contact per seed, then a total per arm. It runs for a while at the default.

```
$ protekt-measure 600
commander measurement, 600 ticks, 10 seeds, scenario skirmish(seed)

placebo enemy -- the enemy side gets NO orders at all
  seed 7  units lost: player 0, enemy 0   first contact 595   first kill never
  seed 11  units lost: player 0, enemy 0   first contact never   first kill never
  ...
  TOTAL: player lost 0, enemy lost 0, seeds where anything was fired at all: 1/10
```

(the other four arms follow; their totals at 600 ticks are 19/16, 18/5, 5/15 and 9/3, player lost / enemy lost.)

A non-number, an empty string or a negative number prints `usage: protekt-measure [ticks]  (default
2400)` and exits 2. A number followed by junk is accepted (`5x` ran 5 ticks), and a second argument
is ignored.

---

## protekt-meshcost

No flags and no arguments: any arguments are ignored. It times the radar's wireframe at four grid
steps and the whole frame around it, prints the table, and exits 0 (about five seconds here). It
asserts nothing.

---

## protekt-atlas-gen

```
protekt-atlas-gen [--seed N] [--out PATH]
```

Writes the 512x512 texture atlas as a PNG. Defaults: seed 1 (the game's own) and `assets/atlas.png`,
relative to the working directory, so run it from the repo root. The default seed reproduces the
committed `assets/atlas.png` byte for byte (checked with `cmp`).

| flag | argument | what it does | refusal |
|---|---|---|---|
| `--seed` | `N` | picks the camo and metal patterns | `protekt-atlas-gen: --seed needs a whole number` (exit 2) |
| `--out` | `PATH` | where to write the PNG | `protekt-atlas-gen: cannot write PATH` (exit 3) |

Anything else, and a flag missing its value, prints `usage: protekt-atlas-gen [--seed N] [--out PATH]`
and exits 2. Success prints `protekt-atlas-gen: wrote PATH (512x512, seed 1)`.

---

## Recipes

All run from the repo root with `build/` built. The captures here use seed 42 and seed 7.

### 1. Capture a frame, headless

```
$ protekt-game --headless --frames 3 --seed 42 --capture /tmp/f.ppm
protekt-game: headless on llvmpipe (LLVM 19.1.7, 256 bits)
protekt-game: wrote /tmp/f.ppm (1280x720, tick 3, hash 15175589382717783187)
```

The file is a P6 PPM, 2764816 bytes. Run it twice and `cmp` finds no difference. `--width 640
--height 360` gives a 640x360 frame of the same tick and the same hash.

The sky captures below came out identical at `--frames 1`, 2 and 3 (seed 7, moon: 9966 pixels each).
Not re-measured here, and from an earlier slice's notes rather than from this reference's runs: on frame 1
the session turns the spawn heading after the aim is computed, so scripted aiming of ORDERS (`--nav`,
`--key`) on frame 1 may land off-screen; put those on frame 2 or later.

**Aiming at the sky.** The sun and the moon are fixed in the world. `--look-yaw` is an OFFSET from the
camera's own heading, not a compass bearing, and the heading at the start depends on the seed. The
world bearings are 70 degrees for the moon (elevation 28) and -139 degrees for the sun (elevation 58),
so the flag is `bearing in centiradians minus the start heading`. Measured, with the disc counted by
its own colour in the PPM:

| seed | body | flags | disc pixels | bounding box (x0..x1, y0..y1) |
|---|---|---|---|---|
| 7 | moon | `--look-yaw 122 --look-up 49` | 9966 | 586..698, 228..341 |
| 7 | sun | `--look-yaw -243 --look-up 101` | 4378 | 605..679, 245..320 |
| 42 | moon | `--look-yaw 6 --look-up 49` | 10053 | 630..742, 227..340 |
| 42 | sun | `--look-yaw 278 --look-up 101` | 4388 | 602..677, 245..320 |

```
$ protekt-game --headless --frames 3 --seed 7 --look-yaw 122 --look-up 49 --capture /tmp/moon.ppm
protekt-game: headless on llvmpipe (LLVM 19.1.7, 256 bits)
protekt-game: wrote /tmp/moon.ppm (1280x720, tick 3, hash 406304520045208418)
```

Seed 7 starts facing bearing 0 (+z), so the world bearings work directly: 122 and -243 are 70 and -139
degrees in hundredths of a radian. Seed 42 starts facing about 116 centiradians (66 degrees; inferred from where the disc lands), so
the moon is nearly dead ahead at `6` and the sun is `-243 - 116 = -359`, i.e. `278` the other way
round (the yaw wraps). With another seed, sweep `--look-yaw` in steps of 40 and look for the disc;
`--look-up 49` and `101` are the elevations of the moon and sun in the same units.

`--look-up` is unsigned, so the view can be lifted but not lowered: `--look-up -5` is refused. A
negative `--look-yaw` is fine.

### 2. Run a scripted scenario

A scenario is a list of `[[group]]` tables. Each needs `kind` (a quoted name), `count` (1 to 64),
`dx` and `dy` (-32 to 32); all four are required. Both sides get the same list, the enemy mirrored.
There must be a `tank`: the first one is the one you drive. Up to 64 units a side. The kinds are
`tank`, `turret`, `factory`, `hover_tank`, `mortar_bike`, `rocket_tank`, `scavenger`, `recycler`,
`deployed_recycler`, `scout`.

`duel.toml`:

```toml
# One tank to drive, two turrets, mirrored for the enemy.
[[group]]
kind = "tank"
count = 1
dx = 0
dy = 0

[[group]]
kind = "turret"
count = 2
dx = 4
dy = 0
```

```
$ protekt-game --headless --frames 1 --scenario duel.toml --log-spawns
protekt-game: headless on llvmpipe (LLVM 19.1.7, 256 bits)
tick 0 status unit 1 tank side player pos 25.0 45.0 hp 100 stance idle
tick 0 status unit 2 turret side player pos 29.0 45.0 hp 150 stance idle
tick 0 status unit 3 turret side player pos 28.0 44.0 hp 150 stance idle
tick 0 status unit 4 tank side enemy pos 147.0 166.0 hp 100 stance idle
tick 0 status unit 5 turret side enemy pos 143.0 166.0 hp 150 stance idle
tick 0 status unit 6 turret side enemy pos 142.0 165.0 hp 150 stance idle
```

Exit 0. The second turret was asked for at (4, 0) and stands at (28, 44): the placement rule nudges
each unit to the nearest free passable tile. Without `--scenario` the built-in composition plays
(ten units a side; 22 unit lines are printed at seed 42).

What a bad file does (all before any window opens, exit 2, nothing played):

```
$ protekt-game --headless --frames 1 --scenario bad.toml     # count = 0
protekt-game: --scenario bad.toml: line 3: 'count' is 0, outside 1..64
$ protekt-game --headless --frames 1 --scenario empty.toml
protekt-game: --scenario empty.toml: the file has no [[group]]: a scenario needs units, 1..64 a side
$ protekt-game --headless --frames 1 --scenario missing.toml
protekt-game: --scenario missing.toml: cannot read 'missing.toml'
```

A file that parses but has no tank is the one scenario refusal that is exit 1, after the window
and device are up: `protekt-game: the scenario has no player tank to drive`.

### 3. Get the battle log out

There are two sources, and only one carries a battle.

**`protekt-sim` is the observer text.** Its log is `sim::BattleLog::to_text` (`src/sim/observer.cpp`)
and carries four kinds of line (counts from a utility-commander run, seed 42, 1800 ticks): `ordered
unit`, `fired unit`, `hit unit`, `killed unit`, then a final `end tick ... hash ...`. The hash is
what to compare between runs.

```
$ protekt-sim --seed 42 --ticks 1800 --commander utility --status-period 0 | tail -3
tick 1785 ordered unit 20 move
end tick 1800 seed 42 units 14 hash 7755221147426015023
```

and the first events:

```
tick 439 fired unit 2 -> unit 13 hp 100
tick 439 fired unit 13 -> unit 2 hp 100
tick 451 fired unit 12 -> unit 2 hp 100
```

With the default `--status-period 60` it also prints every unit's state each 60 ticks, in the
grammar `tick T status unit ID KIND side SIDE pos X Y hp H stance S`.

**`protekt-game --log-spawns` is the same grammar, tick 0 only.** It prints the composition the
scenario produced (recipe 2) and nothing after: `protekt-game` prints no `fired` or `killed` lines, so
it cannot give you a battle log. Use `protekt-sim` for that. `protekt-sim` has no `--scenario`.

### 4. Replay a battle

Only `protekt-sim` records and replays.

```
$ protekt-sim --seed 42 --ticks 300 --status-period 0 --replay-out a.replay > live.txt
$ protekt-sim --replay a.replay --ticks 300 --status-period 0 > re.txt
$ cmp live.txt re.txt && echo IDENTICAL
IDENTICAL            # 161 lines, ending: end tick 300 seed 42 units 22 hash 13397508810464594858
```

The file is text: a header, the seed, then one line per order of BOTH sides (a replay without the
enemy's orders would replay a different battle):

```
# protekt replay v1
seed 42
0 attack 1 13
0 guard 6 1966080 2752512
...
```

Things to know:

* **The replay records the seed and the commands, and nothing else.** It does not record which
  composition was played. `include/protekt/sim/scenario.hpp:56-58` says it outright: a seed plus a
  command list is the whole battle only for the built-in composition, a battle on any other needs the
  data recorded too, and "the replay format records the seed alone today, so it replays the built-in
  one." So: a battle played in `protekt-game --scenario FILE` cannot be recorded at all (there is no
  recorder there), and a replay file always replays the built-in skirmish. `protekt-sim` has no
  `--scenario`, so today the limit costs nothing there, and it is what stops a scenario battle from
  ever being replayed.
* `--seed` and `--commander` are ignored with `--replay`; the file's seed and commands are used:
  `--replay a.replay --seed 99 --commander none --ticks 300` ended `end tick 300 seed 42 ... hash
  13397508810464594858`, the same as the live run.
* `--ticks` is not in the file. Replay for the same `--ticks` you recorded. The replay holds orders
  only up to tick 300, so at `--ticks 600` the replay ended `units 20 hash 4892124221784215595` where
  a live 600-tick run ended `units 19 hash 9673985124876689061`: the live run kept ordering after tick 300
  and the replay did not.
* A file that is not a replay: `protekt-sim: bad replay 'bad.replay': line 1: missing header '# protekt
  replay v1'`, exit 3.

---

## Discrepancies between a flag's name or comment and what it does

Found by running each flag against its source comment. The table above states what each DOES.

* **`--cursor X Y` does nothing.** Its comment says it places a pointer "for a frame that has no
  mouse: a run that wants to show (or capture) a reticle". The values are validated and stored into
  the scripted input frame, but no code reads them (`cursor_x`, `cursor_y` and `cursor_known` are
  written by the window and the flag and read by nothing; `src/game/aim.cpp` says the cursor means
  nothing to the aim). A capture with `--cursor 100 100` was identical to one without, 0 of 921600
  pixels differing. The README's examples pass it.
* **`--press KEY` presses the key on every frame, not the first.** Its comment says "One key pressed
  on the first frame"; `script_hands` pushes it on every frame (`src/app/game.cpp`, no frame test).
  `ctrl+q` quits at frame 0, so it cannot tell the difference; `f11` and `alt+enter` toggle full screen
  each frame in a window. This was read from the code. It could not be observed here, because there
  is no display.
* **`--switch N` says "weapon-key presses" in its refusal, but there is no weapon key.** It is a right
  click on each of the first `N` frames (the comment says so, the error text predates it).
* **`--fire FRAME` need not fire.** It is a trigger pull, and a weapon that is reloading refuses it
  (see the table).
* **`--no-sound` is a no-op headless.** It only matters when a window is open.
* **`--seed` on `protekt-map` defaults to 1, everywhere else to 42.**
* **`--look-yaw`'s bearings are offsets, not compass bearings.** A commit message that says "-243
  reaches the sun" is true for seed 7 and false for seed 42, where the camera starts facing 116
  centiradians round. See recipe 1.
* **`--headless` then `--fullscreen` is refused; `--fullscreen` then `--headless` is not.** The check
  runs when `--fullscreen` is parsed and looks at whatever came before it.
