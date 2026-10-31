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
| **mouse** | turn the turret, relative to the body: the pointer is captured, and the crosshair is always the centre of the screen, where the turret points. The eye sits on the barrel's own line (2026-10-30), `kEyeAboveBarrelMetres` above it (0 for now, Harri's to tune), so the end of the barrel is on the crosshair's line rather than 48 degrees below the bottom of the screen. In a hover tank the mouse turns the hull itself and the turret stays on the centre line |
| `A` / `D` | in a plain tank (which you start in): turn the body left / right, on the spot or while driving; the turret goes round with it. In a hover tank: strafe left / right, and the mouse turns the hull |
| `E` (**held**) | boost: lift the hull, and settle back down when you let go |
| `C` | toggle the colour calibration view: three red/green/blue bars, a check on the display path |
| **left click** | **fire**, wherever the mark is (see below) |
| **right click** | switch weapon: gun -> rocket -> mortar -> bouncer -> shotgun -> gun. The right button is the ONLY switch (2026-10-23: `Q` and the mouse wheel stopped doing this, and the button no longer gives orders -- the orders are on `G`, `M`, `Space` and `F`). The crosshair and the label beside it change; the mortar's crosshair is a range ladder, each rung the distance in tiles a shell fired at that view pitch lands on level ground, the lit rung being where you look now. Every weapon flies the barrel (2026-10-30): the trigger sends the gun's, the shotgun's and the rocket's bolt along your own view (heading AND pitch), so point up and the shot goes up, and the tank's drawn barrel is raised to the same elevation. Nothing is aimed -- no aim-assist, no lead, no correction toward what is under the crosshair, because the bolt takes time and the target may have moved; the unit under the mark is only named for credit. The muzzle sits below the eye and the shot does not correct for it, so at range it runs parallel to the crosshair (Harri said to ignore the offset). With the mortar up it lobs a shell along the same view: point up, fire, and the rung you lit is where it lands. The bouncer is lobbed the same way, by hand, and fires a bouncing round instead of a contact one. The shotgun is a short-range cone of seven pellets, fanned about the barrel, with no aim assist -- close in, or waste the shells. A weapon belongs to the hull you are driving, so a vehicle that cannot carry one never offers it in the ring |
| `Space` | the selector; what it does depends on what you already hold (`Shift`+`Space` is the same key). **Nothing selected:** select the one unit of yours under the mark, if it is within 80% of the radar's reach (32 tiles; 64 in a scout); ground, enemies and the tank you drive are ignored. **Something selected that has weapons** (a tank, a turret, any armed kind) and the crosshair on an enemy asset -- or within about four degrees of one, `kAttackAimToleranceDegrees` -- the left menu's top row reads `SPACE Attack`, a red line runs from that row to the enemy, a small label of its kind (`TANK`, ...) stands over it and a health bar under the label (full width and bright green when whole; it shortens from the right, the empty part black, and goes yellow, red and very dark red as the enemy is hurt; gone at zero); `Space` then orders the selection to attack it. A friendly under the crosshair is never offered. Otherwise an own tank under the mark is followed by the selection, and anything else does nothing. It never adds to a selection: a selection of several comes from an `F` key |
| `F1`-`F10` | select the group of vehicles under that key, wherever they are. The command row along the top (`~`, `1` `2` `3`, then `F1`-`F10`) keeps every key in the same spot: a new vehicle takes the first empty `F` key, a group that is wiped out leaves its key blank (nothing moves up), and a key that holds nothing is not drawn. Each drawn key shows the key, the count, the kind and a row of upright health ticks under it (one per living vehicle, left-justified, coloured by its hit points); `2` is the factory, `~` the nav marks, and `1` is your deployed recycler once you have deployed one. With nothing selected (no left menu open), `1` brings up the recycler and `2` the factory from anywhere on the map -- no pointing -- and says so if you have none; an un-deployed recycler answers `1` too. With a selection the digits are that menu's rows, as the top menu is closed. The STARTING force takes one key per kind, so the starting tanks share `F1`; a vehicle built later takes its own key, because nothing queues a build batch yet (the shift-queued build is unbuilt). The digit row selects no vehicle group: digits answer `G`, `T`, the attack list, the open menu -- and, with no menu open, the buildings on `1` and `2` |
| `X`, mark on an own unit | swap: drive that unit instead; the one you leave stops, and becomes an ordinary unit you can command. Every kind of yours can be driven except the factory: tanks, hover tanks, turrets, mortar bikes, rocket tanks, and also the unarmed scavenger and recycler. It refuses an enemy unit, your factory (immobile, so nothing to drive), a dead unit, the unit you are already driving, and an empty mark |
| `F`, mark on a unit | every selected unit follows that unit: it holds within 4 tiles of it, closing when it is further and standing still inside, and its gun fires at whatever is in range meanwhile. Any living unit of either side may be followed (so a wingman can shadow the hull you left after `X`), but not the follower itself. It refuses with nothing selected, with nothing living under the mark, or when you are driving the only selected unit |
| `N` | (from the top menu: no command menu, `G` prompt or attack list open down the left) arm a nav mark: the left menu shows one row, `SPACE GET NAV` and the distance to the ground under the crosshair (`17m`). `SPACE` places the mark there and closes it; `Tab` (or `Escape`) closes it without placing. Pointed at the sky, the armed state is drawn as nothing at all and comes back unchanged when the crosshair is on the ground again; no mark is ever placed off the ground. There are ten slots, numbered `1`-`9` and then `0` for the tenth, filled in turn: the eleventh mark goes into slot `1` and the mark that was there is gone for good, with no warning. Nothing else ever removes a mark. Marks are your own annotation: the sim never sees them. Each is a small green plus on the radar and on the world map, the same on both |
| `~` | (from the top menu, as for `N`, once a mark exists; the `~` key on the command row is drawn from the first mark on) open the nav menu: `SPACE` (reading `SPACE SELECT` while the crosshair is on a mark), `TAB CLOSE MENU`, then one `<digit> NAV <metres>` row per mark, in slot order. `SPACE` on a mark and that mark's digit do the same thing: toggle its highlight, which draws it yellow inside a yellow diamond on the radar and the map. Several can be highlighted at once; a newly placed mark is never highlighted. `Tab` (or `Escape`) closes the menu. While the nav menu or an armed `N` is open the top menu is shut: `F1`-`F10`, `G`, `T`, `N`, `~` and the digits do nothing until it closes; driving, looking, `R`, `X` and `M` work as ever, and the game never pauses |
| `3`, with a recycler selected | arm the deploy. The left menu's state row reads `SPACE DEPLOY` with the distance to the ground under the crosshair (`SPACE DEPLOY 68m`); `~` rotates the preview, which is the 2x2 tile square the building will stand on (a square rotates to itself, so rotation is only cosmetic for now), and `Tab` cancels. `SPACE` sends the recycler to that tile: it drives there, stands on the site for **ten seconds**, then becomes a deployed building in place, keeping the hull it arrived with. The spot is refused when it is off the map, on ground nothing may enter, or inside another building's footprint, and the recycler stays a working vehicle. A deployed recycler is a building on key `1`; the `F` key the vehicle was under goes blank and nothing moves up. Its 2x2 footprint is impassable and no unit can be spawned inside it. A second deploy is refused |
| `G`, then `0`-`9` | send every selected unit to that nav mark, as a move order, by the number the nav menu shows (`1`-`9`, `0` for the tenth). `G` waits for one key: a digit answers it, `Escape` cancels, and any other key cancels and then does its own job. It refuses when that number is not placed, or with nothing selected |
| `M` | order every selected unit to move to the ground under the mark, even when a unit is in front of it |
| `T` | arm the attack list, whatever the mark is on (the attack of the unit under the mark is `Space`'s now, and `T` no longer gives it): the nearest enemies within radar range (40 tiles; 80 in a scout) are numbered 1-9 and 0, and the next digit attacks that row. It refuses with nothing selected |
| `Tab` or `Escape` | clear the selection (with the nav menu or an armed `N` open: close that instead) |
| `R` | open or close the map: the radar's view switches to the whole world as a square, north up, in the ground's own colours, with your units and buildings (the one you drive ringed in white) and your nav marks, drawn exactly as the radar draws them. R again brings the radar back. It shows everything EXCEPT the enemy assets: the map is the view of what is yours, the radar is the view of what is near. It is only a picture: it gives no orders and cannot be clicked (a click still acts on the world behind it). `O` is BZ2's objectives key and does nothing yet |
| `F11` or `Alt`+`Enter` | full screen, and back again |
| `Ctrl`+`Q` | quit from anywhere |

Your other tanks retreat to your factory for repairs on their own (at 20 hp or less) and return to
their last order when healed (80 hp); the tank you are driving never does.

The tank you are sitting in is **never** moved by a click. It is driven by `W`/`S` and `A`/`D`
every tick, so an order from the mouse would fight your own hands: the tank always drives the way its
body points, and looking around with the mouse never changes where it goes. Selecting it is
fine, and firing is the one thing a click may tell it to do — one bolt, and nothing else. Looking keeps working after the tank is destroyed, and the radar stays up.

Your gun also fires on its own at any hostile within twelve tiles. **A click fires one shot**, and
keeping the button going keeps firing; the gun is back to the nearest hostile in between. There is
no way to hold fire: there is no Stop key and no hold-fire stance, and `Escape` only clears the
selection (and cancels a pending `G` or `T`). A unit that has been told to Stop (the one you leave
behind on `X`) still fires at what comes into range. It will **not** fire on your own units on
its own -- only an order crosses that line (see Friendly fire below).

Selected units are drawn brighter than the rest of their side, so you can see what you have hold of.

**The mark is the gun.** It is the crosshair, fixed at the centre of the screen, and you aim by
**looking**: the mouse swings the turret, and turning the body with `A`/`D` (in a plain tank) carries the turret round with it. **The shot goes where the
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
only `Tab` and `Escape` clear it. `Space` reads the mark fresh every time it is pressed, so after a kill you can
put the mark on the next enemy and press `Space` again without re-selecting anything.

The crosshair is the only thing drawn close to your eye. There is no pointer on screen — the mouse
turns the turret — so the crosshair is both where you look and where your click lands.

## The radar

Bottom-left corner, and it is a **radar, not a map** -- drawn as BZ2's **wide shallow oval**: a circular
ground disc seen from 21 degrees above, so it is about a third as tall as it is wide, with a light border.
It is centred on the tank you are sitting in
and reaches **40 tiles** in every direction -- **80 in a scout**, whose wider view is the point of it --
and what is inside that radius is all it will ever show you. A hostile beyond it is not dimmed, not
on the rim and not hinted at: outside the radius you know nothing, which is the entire reason the
radius exists.

- **green triangle** at the middle — you, always pointing up: **the panel turns with you**, so a
  mark's direction is a direction to turn, with no mental arithmetic in the middle of a firefight.
- **green square** — one of your units, in range.
- **red square** — a hostile, in range. The only way a hostile ever appears.
- **green outline** — your base, in range: a shape as well as a colour, so it reads at a glance.
- **green plus** — one of your nav marks, a small cross about 9 px across with no number on it, in range or pinned to the rim; **yellow, inside a thin yellow diamond**, when that mark is highlighted. The numbers are in the nav menu (`~`). The world map (`R`) draws the marks exactly the same way.
- **green arrow at the rim** — your base, out of range. Your own base is the one thing you are never
  lost about, so it is always drawn or always pointed at. Enemy assets get no arrow, ever.

Distance on the panel is to scale (half the radius is half the way out); the marks themselves are
sized to be read rather than to be exact.

## Display/HUD

The panel above is most of the HUD. Beyond it the screen carries a small amount of text, in a built-in
five-by-five block font: the weapon's name beside the crosshair, the mortar ladder's range in tiles
on each rung, a strip along the top with a plate for each key that holds something -- its key label
centred on the plate, the icon under it and a row of health ticks under that -- a `DEPLOYING` plate over one of your turrets
that is not yet mounted, and a notice line for a refused roster or nav-mark key. The radar is colour
and length only. The ship instrument below carries two numbers.

### The command menu

The menu down the left (a selection's rows, the nav menu, an armed `N`) is drawn as BZ2 draws it:
each row is two translucent **blue bars**, the key's and the verb's, lit from the top and fading
down, with white text outlined in black. The keys are right-aligned on one edge, so a digit sits
close to its verb and a wide key (`SPACE`, `TAB`) grows to the left; between the key bars and the
verb bars runs one straight **amber line** (the `SCRAP` label's colour) from below the state row
past the last row. The bars are rounded on their outer ends and square where the line runs. A row
that does not apply, or that you cannot afford, keeps its place and its key on a flat dark bar in
grey. The menu's text is smaller than the rest of the HUD's and smooth-edged (antialiased) inside
bars of the same size; a lowercase letter, where a row has one (the `m` of `17m`), is drawn as a
smaller capital on the same baseline.

A row that **completes an order** -- `Follow Me`, `Hold`, `Go to Nav`, `Attack` (through the attack list), `Recycle` -- closes the menu and clears the selection as soon as its orders are posted, which is what BZ2 does and what Harri asked for. The rows that **queue a build** or **arm a click** leave the selection alone: their sequence is not finished yet.

**The factory builds eight things.** Select your factory (`2`) and its rows are the keys `1`-`8`, each
drawn with its cost in scrap on the row and dimmed while you cannot pay it: `1 Mortar Bike 45`,
`2 MSL Scout 45`, `3 Tank 55`, `4 Rocket Tank 65`, `5 Assault Tank 70`, `6 Walker 100`, `7 APC 50`,
`8 Bomber 75`. Five of the kinds are new -- the **assault tank** (the tank's gun on a heavier hull,
150 hp), the **walker** (200 hp, hits twice as hard and reloads half as fast, at half a tank's
speed), the **APC** (the fastest ground vehicle; it carries no troops yet, so it has a light gun),
the **bomber** (a wide, flat hull that lobs the mortar's splash shell, 14 tiles) and the **MSL
Scout** (the scout's hull carrying the rocket tank's rocket; a vehicle of its own, not the Scout
at a second price). Every number here is a first proposal, **provisional** until it has been
played, and the new machines are placeholder shapes. The **scavenger** now has the tank's hull,
100 hp (it was 60) -- 25 scout shots to kill instead of 15 -- because a hauler should take a
beating.

**Names follow the faction, never the side.** A Scavenger and a Harvester are the same machine:
the sim has one kind for the pair, and the menu names it by whether the producer's faction is
human or alien. By default the player plays the humans and the enemy the aliens, but that mapping
belongs to the scenario (`SceneOptions::factions`), so a story mission can swap it. The human
recycler's rows are `1 Scavenger 20`, `2 Scout 50`, `3 Turret 40`, `4 Tug 45`, `5 Constructor 40`,
`6 Service Truck 50`, `7 Service Pod 2`; the alien recycler's are `1 Harvester 20`, `2 Scout`,
`3 Guardian 40`, `4 Hauler 30`, `5 Builder 40`, `6 Healer 50`, `7 Service Pod 2`. The Turret and
Guardian rows are names only for now: a turret is a structure, and nothing can place one yet. The
alien factory draws the human factory's eight rows until Harri supplies its own costs and names.

### The health ticks and the ship instrument

Each command-row key is drawn as a **keycap** (Harri: "I would also like the function key displayed in the
same way as here (F3)"): a slate blue-grey face, a 1 px near-black outline round the whole cap, the
white (or, selected, yellow) frame inside it, and inside that a 1 px bevel -- lighter along the top and
left, darker along the bottom and right. The plate carries **one thing**, the key's label, centred on
the cap both ways (Harri: "Centre the label. Stop drawing the kind text. Stop drawing the count on the
plate. Only the f key label, the icon under it, and the vertical health ticks under the icon."). There is
no kind name and no `x<n>` count any more, so nothing now shows a group's size as a number: the ticks are
the only reading, and they stop at ten, which he accepted. `~` is its label alone.

Under each key's plate hangs the **icon**: a pale, plain, side-view silhouette of
the kind in a 16 px square at 720p, left-justified like the ticks, the same grey whoever owns the vehicle
and whether or not the key is selected (the type is shown, not the side). Thirteen kinds have their own
(tank, turret, hover tank, mortar bike, rocket tank, scavenger, recycler, scout, assault tank, walker, APC,
bomber, MSL scout), and so do the buildings (Harri: "the recycler and the factory should have icons
too"): the deployed recycler (`1`) a block with a tower, two stacks and two bay doors, the factory (`2`)
a sawtooth-roofed shed with a chimney, and the armory (`3`) a bunker with a missile standing on each side
of it, which is Harri's own description. The game cannot build an armory yet, so `3` is never drawn today;
its icon appears the moment it holds something. A kind with none draws a hollow box with a dot. `~` shows
no icon, and a key whose vehicles are all dead is not drawn at all. Under the
icon is a row of **upright ticks, one per living vehicle** of the group, left to
right in spawn order from the key's left edge (up to ten; the ticks a long notice, the attack list or the
menu would cover are left out rather than overlapped). Every tick is the same size, 4 px wide and 14 tall,
however hurt its vehicle is: a tick's **colour** says how hurt it is, shading smoothly (per vehicle, per hit
point, never in bands) from bright green through yellow and red to near-black. A vehicle that dies takes
its tick with it and the ones after it close up to the left with no blanks. A building key has one tick.
The nav mark plate (`~`) has no ticks.

A vehicle's body follows the ground it stands on: it leans to the slope under it, by at most
`kMaxTiltSlope` (provisional, derived from the ride height and the hull's half-width). Structures
(factory, turret, deployed recycler) stay upright. Drawing only; the sim is untouched.

There is no health bar above the radar (retired 2026-10-23). The hull you are sitting in is read off the
**ship instrument**, bottom right: a C-shaped ring of segments, its upper arc the hull in green and its
lower arc the ammunition in blue, the two readings as numbers in the ring's opening, and the four
weapons beside it with the one you have up lit. A small bar also floats over every other
field unit in view (not factories or turrets), green for yours and red for a hostile. The readout that names the enemy you have targeted is **red at every health** as well, so a hostile's bar can never be read as one of yours, whose colour falls with its damage.

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
crosses the line -- your own trigger with the green mark on one of your own units fires at it, and the
sim accepts an explicit Attack order on a friendly. (Neither `Space` nor `T` is one of these: `Space` is only
offered a hostile, `T` arms the attack list, and the keyboard has no key that names a friendly.) That is
deliberate: it means you can destroy your own tank or your own factory if you mean to, without your units
ever turning on each other without being told. A unit may never name *itself*, though, so an accidental
order cannot make a tank shoot itself.

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
damage and die; a shot is a real projectile that takes time to arrive, is aimed with a lead on a moving target, is carried by
the velocity of the tank that fired it and is drawn in flight; the enemy commander (the scripted one the
game plays against) advances on the nearest of your units and engages it, and never withdraws or
reads its own hit points; the whole simulation is
deterministic (a seed plus its orders replays byte-for-byte); the window and the headless renderer
produce byte-identical frames **and the same world state** — for a scripted run, because `--autodrive`
drops real input: a stray key arriving at a window that has just taken focus must not be able to
change a verification run.

**Not there yet.**

| gap | what it means when you play |
|---|---|
| **Unreachable goals are not planned for** | a move order is planned as a route over the tile grid (deterministic A*), so units go round craters and rims and out of pockets. A goal that cannot be reached at all gets no route: the unit steers straight at it and may stop against what is in the way. `tests/test_routing.cpp` pins the plan, `tests/test_navigation.cpp` and `tests/test_steering.cpp` the local detour it falls back on |
| **The economy can be played, but not yet ordered** | wreckage (scrap) lies where units die, **one scavenger a side** hauls it to the side's factory, each delivered load is counted as `SCRAP`, and what a producer's queue is ordered to build is paid for from that count. Nothing builds on its own: the sim has no automatic production, and the enemy builds nothing until it has an AI that decides to |
| **The war can end, and the HUD says so** | the conditions are the scenario's own data (`sim::Objective{verdict, side, kind}`, asked in order by `Sim::war_outcome`), and the list the game plays by default is the recycler pair: it is lost when your recycler is destroyed and won when the enemy's is — in either form, the vehicle or the base it deploys into. A factory is still a 400 hp target that a single tank can destroy — factories are unarmed on purpose and defence is what turrets are for — but under the built-in list only the recycler decides the match |
| **Most refusals are on the console, not the screen** | text is drawn (see Display/HUD), but only a refused roster F-key or nav-mark digit puts its sentence on screen; every other refusal ("that order would move the tank you are in") goes to the console. The radar carries no text, and nothing marks the unit your gun is engaging |
| **Little feedback that YOUR shot happened** | the bolt is drawn, your gun is heard, the reticle spreads for 0.2 seconds when your shot damages an enemy and a mortar shell leaves a landing mark. There is no muzzle flash and no recoil, and a shot that misses gives no reticle response |
| **No positional audio beyond left and right** | a sound is placed on a line between the speakers: no front or back, no height, no echo or occlusion. A shot behind a hill sounds exactly like one in the open |
| **Sounds are generated, not recorded** | they are made of noise and sine waves and sound like it. A recording would sound better and bring files, licences and a loader; that trade has not been made |
| **A wreck's smoke is round across, not round up the side** | each puff of a kill's burst and column is an eight-cornered prism (84 vertices; it was a 36-vertex box), as wide across its flats and as tall as the box was, so from above or at a slant it reads as a drum and not a die. From the side a plume is still a stack of drums, because the prism is flat-topped; a rounder profile (a bevelled or sphere-like puff) is not built |
| **Silence beyond earshot is deliberate** | 60 tiles for a shot, 90 for a death; past it there is no voice at all. It is not a bug to report when a distant fight makes no noise |
| **Sounds queue, they do not overlap across frames** | the device plays what it is given in order, so a burst of fire within one sound's length waits its turn, and a buffer arriving with more than about three quarters of a second already queued is dropped rather than played late |
| **The radar is range-based, not line-of-sight** | a hostile behind a hill inside its reach (40 tiles; 80 in a scout) is still on it. Battlezone's radar worked the same way, and this is the version to play before adding occlusion |
| **Compass** | the panel turns with the tank; N, E, S, W sit on the ring for the true world directions, from the same yaw as the marks |
| **Two units on one panel pixel** | the depth test decides, so one colour wins. A hostile within about a tile of you hides under your own mark |
| **Lighting is one sun, no shadows** | the ground is painted from textures (six variants of each terrain type, each tile turned one of eight ways) and units are built from several lit prisms, all shaded by one directional sun. Nothing casts a shadow: each living unit, the one you drive included, stands on a dark translucent blob laid flat on the ground (sized from its footprint) as a stand-in, and the textures are generated, not authored art. Distance fog blends the world toward the sky's horizon colour: nothing within 5 m, solid at the edge of the meshed ground, and linear in between, so the terrain's own paint gives way to the sky's colour with distance instead of ending in a line. The blend goes into the light where the scene's vertices are built, so by the rim the ground is the horizon colour's hue modulated by each tile's own paint — a haze of the sky's colour rather than a hard edge. The HUD (bars, panels, radar, reticle) is never fogged |

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

Every flag of every binary is in [`docs/CLI.md`](docs/CLI.md).

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
