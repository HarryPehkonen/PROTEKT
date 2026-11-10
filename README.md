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
| **mouse** | turn the turret, relative to the body: the pointer is captured, and the crosshair is always the centre of the screen, where the turret points. The eye sits on the barrel's own line (2026-10-30), `kEyeAboveBarrelMetres` above it: 1 m, Harri's ask ("maybe 1m above"), which keeps the end of the barrel in the frame (27 degrees below the crosshair, inside the 30 degree half field of view) instead of 48 degrees below the bottom of the screen. `V` moves the eye (below). In a hover tank the mouse turns the hull itself and the turret stays on the centre line |
| `A` / `D` | in a plain tank (which you start in): turn the body left / right, on the spot or while driving; the turret goes round with it. In a hover tank: strafe left / right, and the mouse turns the hull |
| `E` (**held**) | boost: lift the hull, and settle back down when you let go |
| `V` | rotate the camera between three positions, round to the first: **on the barrel** (1 m above its line); a **chase** view 4 m behind the hull at the same height; a **higher chase** 8 m behind and 3 m above the barrel's line, so the hull and the ground round it read (2026-10-30; Harri: "BattleZone 2 let's the player choose the location of the camera through some key sequence. They rotate between maybe three of them."). Only the eye moves: the view still looks along the turret's aim, the crosshair and the shot are unchanged, and the driven hull is never what the crosshair names. The distances and the third height are provisional, and `V` is mine (`C` is the calibration view's)
| `C` | toggle the colour calibration view: three red/green/blue bars, a check on the display path |
| **left click** | **fire**, wherever the mark is (see below) |
| **right click** | switch weapon: gun -> rocket -> mortar -> bouncer -> shotgun -> gun. The right button is the ONLY switch (2026-10-23: `Q` and the mouse wheel stopped doing this, and the button no longer gives orders -- the orders are on `G`, `M`, `Space` and `F`). The crosshair and the label beside it change; the mortar's and the bouncer's crosshair is the centre dot alone (2026-10-31: the eight-rung range ladder is deleted -- Harri: "With the assist-targetting, let's leave the ladder out completely.  BattleZone 2 doesn't have it." -- so nothing on the screen reads a distance off for you; the landing marker shows where your shell came down, and the assist dot, `T` below, is the mortar's aiming aid). Every weapon flies the barrel (2026-10-30): the trigger sends the gun's, the shotgun's and the rocket's bolt along your own view (heading AND pitch), so point up and the shot goes up, and the tank's drawn barrel is raised to the same elevation. Nothing is aimed -- no aim-assist, no lead, no correction toward what is under the crosshair, because the bolt takes time and the target may have moved; the unit under the mark is only named for credit. (`T` draws a hint where to aim and nothing more: see its row.) The muzzle sits below the eye and the shot does not correct for it, so at range it runs parallel to the crosshair (Harri said to ignore the offset). With the mortar up it lobs a shell along the same view: point up, fire, and the shell flies the barrel. **The mortar throws at one fixed speed** (2026-10-31, Harri: "Fixed speed! That's more realistic than constant horizontal speed, right?", and on a level aim, "Let it be flat."): the shell leaves the barrel at one muzzle speed (about 21 tiles a second, calibrated so a 45 degree shot peaks at the mortar's 14 tiles) and your aim's elevation splits it between reach and climb, as a real mortar does. A level aim is a flat shot (4.4 tiles), range peaks near 45 degrees (15.0 tiles) and FALLS after it, so a steep aim is a high, short lob over an obstacle: the steepest aim (1.28 rad) climbs 7.3 tiles and lands only 8.1 tiles out ("The maximum angle should not be determined by maximum reach"). At the view pitches 0.0, 0.2, 0.4, 0.6, 0.8, 1.0, 1.15 and 1.28 rad a shell lands 4.4, 8.0, 11.8, 14.4, 15.0, 13.5, 11.0 and 8.1 tiles out on level ground. The bouncer is lobbed the same way, by hand, and fires a bouncing round instead of a contact one; its bounces carry it on past the first landing, but its own **max travel is 18 tiles** (Harri: "Give it a separate max travel. Maybe 18 tiles for now." -- provisional), so a shell that bounced and would fly further comes to rest there, and at a 0.8 rad aim (first landing 14.95 tiles) it rests at 18.0 where it used to roll on to about 30. The AI lobs the same shell: an enemy mortar bike (and the bomber, below) leaves at that one muzzle speed too (Harri: "Yes -- same fixed-speed lob as the player's"), at the STEEPEST elevation that reaches its aim point, the HIGH arc, bounded by the same 76 degree cap (2026-10-31, Harri on BattleZone II: "I wasn't looking for the reach, but for the highest lob"), so its shell clears whatever stands between the gun and the target. Measured on level ground: 7 tiles 40 ticks in the air, peaking 6.9 tiles; 10 tiles 38 ticks, 6.3 tiles; 12 tiles 36, 5.7; 14 tiles (its engagement range) 32, 4.6. A target nearer than the cap can throw (about 6.6 tiles) has no high arc inside the cap, and an elevation up to the cap would overshoot, so there it still gets the low, flat arc (4 tiles: 6 ticks, peaking 7 height units); a target past the reach of the muzzle speed gets 45 degrees, the farthest it throws. The cost is accepted, not tuned: the shell is in the air 32 to 40 ticks where the flat one took 10 to 25, so the AI connects less often against a MOVING target (a tank walking at 0.2 tiles a tick has gone 6 to 8 tiles by the time it lands); it comes down nearly straight, so a miss lands within a step of the aim point instead of carrying about 1.6 tiles past it. The old shell, which crossed the map at a constant 0.27 tiles a tick and took 26 ticks to reach 7, is gone. The shotgun is a short-range cone of seven pellets, fanned about the barrel, which `T`'s dot only marks and never steers -- close in, or waste the shells. A weapon belongs to the hull you are driving, so a vehicle that cannot carry one never offers it in the ring |
| `Space` | the selector; what it does depends on what you already hold (`Shift`+`Space` is the same key). **Nothing selected:** select the one unit of yours under the mark, if it is within 80% of the radar's reach (32 tiles; 64 in a scout); ground, enemies and the tank you drive are ignored. **Something selected that has weapons** (a tank, a turret, any armed kind) and the crosshair on an enemy asset -- or within about four degrees of one, `kAttackAimToleranceDegrees` -- the left menu's top row reads `SPACE Attack`, a red line runs from that row to the enemy, a small label of its kind (`TANK`, ...) stands over it and a health bar under the label (full width and bright green when whole; it shortens from the right, the empty part black, and goes yellow, red and very dark red as the enemy is hurt; gone at zero); `Space` then orders the selection to attack it. A friendly under the crosshair is never offered. Otherwise, if **`SPACE Deploy`** is offered (below), it deploys: **one scavenger** selected with a **biometal pool within 4 degrees of the crosshair** (`sim::kPointingConeDegrees`, the cone the AI's deploy points with and `T` takes, pinned equal to `kAssistConeDegrees`; the pool must be inside the radar's reach and not already held) drives to the pool and deploys there; **turrets** selected with the crosshair on bare ground drive to that spot and deploy there. Otherwise, with the crosshair on **bare ground** (no unit under it), the top row reads **`SPACE Go Here`** and `Space` sends **any vehicle or a whole function-key group** to that tile, one `MoveTo` each, the same order `M` gives (Harri, 2026-11-03: "select any vehicle or group of vehicles under a function key, point at the ground, and say SPACE Go Here"); it is offered only to a selection that can move (not a building), never over the attack or the deploy, and so turrets on open ground stay `SPACE Deploy`. While it shows, **the same thin white line as the Deploy's runs from the `SPACE Go Here` row to the middle of the tile `Space` would send the selection to** (Harri, round 10, 2026-11-05), drawn from the same offer as the verb so it comes and goes with the prompt. Otherwise an own tank under the mark is followed by the selection, and anything else does nothing. Any of these orders finishes the command, so it **closes the menu and clears the selection**. It never adds to a selection -- so with something still selected you cannot select a turret; a selection of several comes from an `F` key |
| `F1`-`F10` | select the group of vehicles under that key, wherever they are. The command row along the top (`~`, `1` `2` `3`, then `F1`-`F10`) keeps every key in the same spot: a new vehicle takes the first empty `F` key, a group that is wiped out leaves its key blank (nothing moves up), and a key that holds nothing is not drawn. Each drawn key shows the key label centred on the plate, the kind's icon under the label and a row of upright health ticks under that (one per living vehicle, left-justified, coloured by its hit points, each ringed in a thin dark grey outline -- Harri, round 10, 2026-11-05: in place of the black gaps between them); `2` is the factory, `~` the nav marks, and `1` is your deployed recycler once you have deployed one. With nothing selected (no left menu open), `1` brings up the recycler and `2` the factory from anywhere on the map -- no pointing -- and says so if you have none; an un-deployed recycler answers `1` too. With a selection the digits are that menu's rows, as the top menu is closed. The STARTING force takes one key per kind, so the starting tanks share `F1`. **The row is for the assets you command, not for you** (Harri, 2026-11-02): the vehicle you are sitting in is never under a key -- you already control it, and a key for it could only offer an order that fails -- and every turret always is, deployed or not (a turret stays a vehicle; it is not a building like the deployed recycler). `X` swaps the row with you: the vehicle you leave takes the first free `F` key and the one you enter loses its own; with no key free for the one you would leave, the swap is refused (`No function key is free for the vehicle you would leave.`). A key the entered vehicle held alone counts as free. A vehicle built on its own takes its own key; holding `SHIFT` on a build row queues one more per press under ONE key, up to ten, drained one at a time as the build time and the scrap allow. **The batch does not begin until you let go of `SHIFT`** (Harri, round 10, 2026-11-05): while it is held the presses only pile up, and the producer is told when it comes up. **`Tab` before letting go calls the whole batch off** -- nothing is built unless you redo it. **Mid-batch, select the builder and press `Shift`+`Tab`** to cancel the REST of the build: what was already built stays, and the unit in progress gives its scrap back. **That key appears the moment the first press is queued**, before anything is built (BATCH-PLATE, Harri, 2026-11-03): it is the first empty `F` key, held for the batch, and its row of ticks shows the batch's count as **blank (hollow) ticks**; each unit that is built lands on the key and its blank becomes a coloured health tick, and the **icon's whole square is a build gauge** on the plate -- a dark track with a blue fill rising from the bottom to the share built, one cell row at a time, the glyph drawn over it (round 10, 2026-11-05: the fill used to show only inside the glyph, so a short icon told you nothing; now it shows in the empty parts of the square too, and goes when the batch is done) -- and the glyph itself is still a dim ghost above the fill line (empty while the build waits for scrap). The key is let go when the queue holds no more of the batch. A lone (unshifted) build shows nothing until it arrives. The digit row selects no vehicle group: digits answer `G`, the attack list, the open menu -- and, with no menu open, the buildings on `1` and `2` |
| `X`, mark on an own unit | swap: drive that unit instead; the one you leave stops, and becomes an ordinary unit you can command. Every kind of yours can be driven except the factory: tanks, hover tanks, turrets, mortar bikes, rocket tanks, and also the unarmed scavenger and recycler. It refuses an enemy unit, your factory (immobile, so nothing to drive), a dead unit, the unit you are already driving, and an empty mark |
| `F`, mark on a unit | every selected unit follows that unit: it holds within 4 tiles of it, closing when it is further and standing still inside, and its gun fires at whatever is in range meanwhile. Any living unit of either side may be followed (so a wingman can shadow the hull you left after `X`), but not the follower itself. It refuses with nothing selected, with nothing living under the mark, or when you are driving the only selected unit |
| `N` | (from the top menu: no command menu, `G` prompt or attack list open down the left) arm a nav mark: the left menu shows one row, `SPACE GET NAV` and the distance to the ground under the crosshair (`17m`). **A thin white line (the Deploy and Go Here line's own white, one pixel) runs from that row to the middle of that same tile** while the row shows, and goes with it (round 12, AU). `SPACE` places the mark there and closes it; `Tab` (or `Escape`) closes it without placing. Pointed at the sky, the armed state is drawn as nothing at all and comes back unchanged when the crosshair is on the ground again; no mark is ever placed off the ground. There are ten slots, numbered `1`-`9` and then `0` for the tenth, filled in turn: the eleventh mark goes into slot `1` and the mark that was there is gone for good, with no warning. Nothing else ever removes a mark. Marks are your own annotation: the sim never sees them. Each is a small green plus on the radar and on the world map, the same on both |
| `~` | (from the top menu, as for `N`, once a mark exists; the `~` key on the command row is drawn from the first mark on) open the nav menu: `SPACE` (reading `SPACE SELECT` while the crosshair is on a mark), `TAB CLOSE MENU`, then one `<digit> NAV <metres>` row per mark, in slot order. `SPACE` on a mark and that mark's digit do the same thing: toggle its highlight, which draws it yellow inside a yellow diamond on the radar and the map. Several can be highlighted at once; a newly placed mark is never highlighted. `Tab` (or `Escape`) closes the menu. While the nav menu or an armed `N` is open the top menu is shut: `F1`-`F10`, `G`, `N`, `~` and the digits do nothing until it closes; driving, looking, `R`, `X` and `M` work as ever, and the sim never stops for them |
| `~`, after a function key | open a LIST of that key's units on the left (AV; Harri, 2026-11-08: "Function key -> '~ Select Unit' should give a LIST of units on the left side, in the same order as the health bars under the function key ... numbered from 1 to 9, and the tenth would have the label 0"). One row per living unit, in the order the key's health bars draw them (`unit_list_rows`, built from `read_slot`, so the two cannot disagree), keyed `1`-`9` then `0`, then the vehicle's name; at most ten. A digit selects that unit alone, so the next order goes to it and the others are undisturbed; `~` or `Escape` closes the list, any other key closes it and acts. The group and the vehicle stay remembered after an order, so `~` opens the list again until `F1`-`F10`, `Tab` or `Escape`. `~` does nothing, and says nothing, with no group touched, with a selection that is not inside that group, and for a key holding one unit. The left menu's `~ Select Unit` row is live whenever `~` would open a list (the selection is inside a key that holds two or more units, even when one of them is the whole selection) and dim otherwise |
| THE FOUR LISTS (AW) | `~ Select Unit`, a service truck's `3 Repair`, a vehicle's `2 Go to Nav` (it lists **every nav point you have set**, numbered by the mark's nav slot digit with its distance in whole metres at the right; the digit sends the selection there and the menu closes; with none set it says `no nav points are set.`; Escape or any other key closes it; the highlight is the `~` chain's own and `Go to Nav` no longer reads it -- AX) and an armed vehicle's `3 Attack` are one look, the left menu's own (Harri, 2026-11-08): the blue bars rounded on their outer ends with the pale ring and top highlight, the key on the left, a slight gap, then the name; **no list has a drawing of its own** (the attack list used to be a black glass plate; it is `TAB Close Menu` and menu rows now, any key closing it as ever). A row that names a unit -- a unit, a patient, a hostile -- also carries two thin lines under its name: **health** in the hit-point ramp (green from half health, amber from a fifth, red below) and **ammunition capacity** in blue. The sim has no magazine yet, so the blue line is always full (`kAmmoPlaceholderFraction`, Harri 2026-10-23); a nav mark is a place, not a unit, and has neither line. A hostile's row draws no green leader line (green says yours) |
| `3`, with a recycler selected | arm the deploy. The left menu's state row reads `SPACE DEPLOY` with the distance to the ground under the crosshair (`SPACE DEPLOY 68m`); `~` rotates the preview, which is the 2x2 tile square the building will stand on (a square rotates to itself, so rotation is only cosmetic for now), and `Tab` cancels. `SPACE` sends the recycler to that tile: it drives there, stands on the site for **ten seconds**, then becomes a deployed building in place, keeping the hull it arrived with. The spot is refused when it is off the map, on ground nothing may enter, or inside another building's footprint, and the recycler stays a working vehicle. A deployed recycler is a building on key `1`; the `F` key the vehicle was under goes blank and nothing moves up. Its 2x2 footprint is impassable and no unit can be spawned inside it. A second deploy is refused |
| `G`, then `0`-`9` | send every selected unit to that nav mark, as a move order, by the number the nav menu shows (`1`-`9`, `0` for the tenth). `G` waits for one key: a digit answers it, `Escape` cancels, and any other key cancels and then does its own job. It refuses when that number is not placed, or with nothing selected |
| `M` | order every selected unit to move to the ground under the mark, even when a unit is in front of it |
| `T` | **assist-targetting.** Takes the hostile nearest the crosshair within 4 degrees of it (measured in 3D to the asset's middle, so a target on a ridge or in a crater counts as long as it is within the angle; the cross's own ticks reach 1.8 degrees). The HUD then draws a thin red line from that asset to a small red dot, and gives it the health bar (below), even a turret: **put the crosshair on the dot and fire**. The dot is where your BARREL must point for the weapon that is up to hit the asset if it keeps doing what it is doing now. For the gun, rocket and shotgun it is the lead point: the target's velocity times the bolt's flight time, and the flight time depends on the lead, so it is iterated (3 to 8 passes in the measured cases, 16 at most). For the mortar and bouncer it is the bearing and elevation of the LOW arc that comes down on the target, solved the same way against the fixed muzzle speed. **It draws and never steers**: the shot still goes exactly where the barrel points, and a target that turns or changes speed after you fire is missed, as it is for the AI. No dot is drawn when the aim point would be past the weapon's travel (16 tiles for the gun and the mortar, 18 the bouncer, 20 the rocket, 8 the shotgun), when the mortar cannot reach it at any elevation, or when it never settles (a target as fast as the shot). The lock stays when the crosshair leaves the unit and ends when it dies or leaves the radar's reach; `T` again takes whatever is nearest the crosshair now, and with nothing within 4 degrees it lets go (with nothing held it does nothing at all, and says nothing). `T` is not the attack list: that is the command menu's `3 ATTACK` row -- select a vehicle with its `F` key, press `3`, and the nearest enemies within radar range (40 tiles; 80 in a scout) are numbered 1-9 and 0; the next digit attacks that row. `Space` attacks the unit under the mark |
| `Tab` | clear the selection (with the nav menu or an armed `N` open: close that instead) |
| `Escape` | **pause** the game and show the statistics screen; `Escape` again resumes. While paused the sim does not step (the tick stands still, the enemy thinks nothing, no order is taken) and nothing sounds, the radar's ping included. The screen is big white-on-black type, two columns, `YOU` in green and `ENEMY` in red: time played, vehicles built, vehicles destroyed (each side's own losses), biometal scrap collected, shots fired, shots hit and shots missed (fired minus hit, never counted separately; a scatter-gun's pellets and a shell's splash can strike more than was fired, and that side then shows no misses). Turrets and buildings are not vehicles and are not counted as built or destroyed; scrap counts only what a scavenger picked up or a deployed one extracted, not a recycle's refund. With a prompt open (the `G` prompt, the attack, repair or deploy arming, `~` stepping, the nav menu) `Escape` closes THAT instead and does not pause; and it no longer clears the selection (`Tab` does) |
| `R` | open or close the map: the radar's view switches to the whole world as a square, north up, in the ground's own colours, with your units and buildings, **the enemy only as the radar's own blips** (the same detection model, one function: an enemy in the driven unit's reach that is moving or firing, or stopped both within the last five seconds, always; any other only on a sweep, fading within a second; the enemy is never an asset on the map) and **every biometal pool on the map, in sight or not** (a thin white ring in a black edge, 3.5 px in radius at the reference frame -- `kMapPoolRingRadius`, its own size, 1.75 times a vehicle's dot and 7 px across against the dot's 4 -- 1 px thick (`kMapPoolRingBar`), drawn under your blips so a scavenger deployed on one still shows inside it; Harri, 2026-11-02: "the map shows the locations of all biometal pools", and round 11 AS: "Can the white squares around biometal pools be thin round circles instead? They could be just a bit bigger than the dots that represent assets like tanks") (the vehicles' dots are `kMapUnitHalf` 2.0 px a side-half and a building's square `kMapBuildingHalf` 6.5 -- round 10 halved them to 1.5 and 5.0, round 11 item AE ("slightly larger ... a bit too small now") took them a third of the way back; the radar's own sizes did not move, and a pool's ring is no longer building-sized, so a building deployed exactly on a pool hides it (a vehicle's dot does not); the line keeps its 3 mm; the one you drive a white dot with a short white line pointing along your **hull's heading** -- where you would move going forward -- which follows you as you turn, standing still or not) and your nav marks, all of them whatever the range, drawn as the radar draws one. The command menu's amber leader line follows the map as it follows the radar: down the gutter, along the top of the map and down onto each selected unit's square, one line and dot per selected unit. R again brings the radar back. It shows everything EXCEPT the enemy assets: the map is the view of what is yours, the radar is the view of what is near. It is only a picture: it gives no orders and cannot be clicked (a click still acts on the world behind it). `O` is BZ2's objectives key and does nothing yet |
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
selection (and cancels a pending `G` or an open attack list). A unit that has been told to Stop (the one you leave
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
- **green square** — one of your units, in range. **Your base** is a hollow square, 33 px across, and its outline is a thin line, 2.5 px (`kRadarBaseOutlineFraction` 0.15 of its half-size, `radar.hpp`; Harri, round 11 item AD: "a large thick square around my base ... It obscures a lot" -- it was 8 px); a biometal pool's outline keeps its 0.5.
- **red square** — a hostile, in range (an enemy factory is not a square but a red OUTLINE, the same 33 px and the same thin 2.5 px line as your base, round 11 item AK), **as the detection model shows it** (below). The only way a hostile ever appears. Both are small dots: 4.6 px across on a 1280x720 frame (half what they were, Harri, round 10, 2026-11-05: "half the width and height of the current size"; 9.2 px since 2026-11-02 and 14.5 px before that; `kDotHalfFraction` in `radar.cpp`, 0.007 of the panel's width). Scrap is not a dot and keeps its size, so a piece is now the larger of the two.
- **the detection model** (Harri, 2026-11-03: "Every few seconds, you hear a blip as the radar does a sweep. Suddenly you see the red enemy dot on the radar, but it fades gradually and within a second. This is for enemy assets that are not moving. If they are moving, they show up at all times."): a hostile that is **moving** (any velocity this tick) **or firing** (it launched a shot this step) is on the radar always -- "Yes, firing counts as moving." When it stops doing both it stays for **another five seconds** (the **tail**, `kRadarActiveTailSeconds`) and then goes invisible for as long as it neither moves nor fires again ("let's say they stay on the radar for another five seconds, and then become invisible in the radar for as long as they don't move or fire again"). Any other hostile (a turret that is not shooting, a building, a halted vehicle) appears only on a **sweep** -- the sim tick that is a multiple of `kRadarSweepIntervalTicks` (5 s, `kRadarSweepSeconds`, `radar.hpp`) -- at full strength, its red fading in a straight line to nothing over `kRadarBlipFadeTicks` (1 s) and staying gone until the next sweep. Each sweep is heard as a soft high ping (`SoundKind::RadarBlip`, dead centre). Selecting or targeting changes none of it. THREE numbers, never to be conflated, each its own `constexpr` in `radar.hpp` (Harri: "Let's make them two different numbers"): the **sweep** (5 s, how often a stationary unit blips), the **fade** (1 s, how long one blip lasts) and the **tail** (5 s, how long a moving or firing unit lingers). The sweep, the fade and the unit's velocity come from `Sim::tick()` and the unit alone; "recently moved or fired" is the one remembered fact, kept by `RadarActivity` in the app (like the hit flash; the sim never sees it and it is not hashed), fed after every sim step from the velocities and `Sim::shots()`.
- **green outline** — your base, in range: a shape as well as a colour, so it reads at a glance.
- **small white square** — litter: scrap lying on the ground (a wreck's pieces, and the pieces a skirmish opens with), in range. A little smaller than a unit's blip (7.3 px across, against its 9.2) and drawn under the blips, so it never hides one. It is the same white for anything a scavenger can collect, and a scavenger's radar is the reach it collects in, so a white mark on the panel is a mark that scavenger can take. A wreck's pieces only settle where a vehicle could drive to from the wreck (never on a tile ringed by craters), and a scavenger that finds a pile it cannot reach -- no route to it, or sixty seconds on one trip -- gives that pile up, goes idle, and does not choose it again, so it is never stuck on scrap for the rest of the battle.
- **white outline** — a biometal pool (it vents steam in the world), in range: twice the size of a piece of litter and hollow, so it reads as a place. The same white, the same reach. A map lays 3 to 10 of them, from its seed, split evenly between the two sides' halves. The world map (`R`) shows ALL of them, in sight or not, as a thin round ring (the radar's pool outline is still the square: its size is pinned by `EverySizeOnTheRadarIsPinned...`, round 11 AE).
- **green plus** — one of your nav marks, a small cross about 9 px across with no number on it, **only while it is inside the radar's range**; **yellow, inside a thin yellow diamond**, when that mark is highlighted (selected in the nav menu). A mark beyond the range is not on the radar at all, unless it is highlighted: then it is **a tiny yellow arrow head at the radar's edge**, pointing along the bearing from the radar's middle toward the mark (about 10 px across; it is a pointer, never the mark's own place). The numbers are in the nav menu (`~`). The world map (`R`) draws EVERY mark, at any range, the same way as the radar draws one in range.
- **green arrow at the rim** — your base, out of range. Your own base is the one thing you are never
  lost about, so it is always drawn or always pointed at. Enemy assets get no arrow, ever.

Distance on the panel is to scale (half the radius is half the way out); the marks themselves are
sized to be read rather than to be exact.

## Display/HUD

The panel above is most of the HUD. Beyond it the screen carries a small amount of text, in a built-in
five-by-five block font: a strip along the top with a plate for each key that holds something -- its key label
centred on the plate, the icon under it and a row of health ticks under that -- a `DEPLOYING` plate over one of your turrets
that is not yet mounted, and a notice line for a refused roster or nav-mark key. The radar is colour
and length only. The ship instrument below carries two numbers.

**Edge smoothing (2026-11-01).** The world -- sky, terrain, units, shots, smoke -- is drawn into an
offscreen image and passed through one fullscreen FXAA-style filter before anything else is drawn; the
HUD, the assist dot, the reticle and every panel are drawn AFTER it, onto the smoothed frame, and are
never softened. The filter blends only where the local brightness contrast is high, so flat colour is
untouched, but it softens EVERYTHING above its threshold -- the ground's texture patches included, which is
the known trade. Its two knobs (`edge_threshold`, `subpixel_blend`, and a dark-area floor) are one named
constant, `kFxaaSettings` in `src/render/renderer.cpp`. `--no-fxaa` turns it off: that frame is
byte-identical to what the renderer drew before the filter existed. Same seed, same frames, one capture each
-- see "Build, run, watch" for the two commands. No history buffer: a frame is a function of itself.

**Chamfered hulls (AT, then AY: every vehicle, more).** Smooth shading had nothing to blend on a box hull: AP
measured about 6,900 face pairs at 85-90 degrees (boxes) against about 3,900 at 40-50 (chamfers), so the arm's
eight-sided knuckle read round and the hull did not. `add_part` in `src/game/vehicle_shape.cpp` now cuts the box
corners of every lit Hull and Load part of EVERY authored kind (the sixteen: scavenger, tank, hover tank, scout,
missile scout, rocket tank, assault tank, mortar bike, walker, APC, bomber, defence turret, tug, constructor,
service truck, service pod): a corner that turns more than 60 degrees becomes two that each turn half as much, so a
90 degree edge is two 45s and the tank's 127 degree glacis foot two 63.7s; a second pass cuts what is still over 66
(the bomber wing's 141 degree tip), all under the 75 degree crease. AT cut 0.10 m; AY cuts `kHullChamfer` 0.25 m,
held to `kMaxCutShare` 0.45 of the shorter adjacent edge (two cuts on an edge leave a tenth flat; 0.5 would meet).
The 0.25 is MEASURED: the tank's engine deck, the scavenger's cab and the tank's compartment are the big blocks, and
0.25 m is the most they all take in full; the 0.25 m bustle, the 6 cm fender and the thin walls take the 0.45 share of
their own edge instead. Metal, Track and Glass parts (hatches, arms, wheels, aerials, racks) keep their corners, as
in AT. A tip that a cut would shorten (the scout's nose, the bomber's tail, the turret's base) is authored out by
what its cut takes, so no machine got smaller. The defence turret's base and pedestal round with it; the factory
and the recycler were left whole by AY (hand-written prisms whose faces carry painted-door and ribbed-wall texels)
and are rounded by BC, next. The silhouette changes (and with it the 72 px plate, which is the
same shape seen side on); the crease angle, the sim and every battle hash do not. Drawn triangles for the sixteen:
9,577 -> 10,500 (+9.6%).

**The factory and the recycler are rounded (BC; presentation only).** Harri: "Add roundness to everything that you are
asking about." Their faces carry painted art, so the route was chosen per piece, and it is route (b): **the painted
faces stay flat panels set INTO a rounded mass**, because the factory's door is one 64-texel tile whose opening sits 4
texels off one edge and 12 off the other, and lofting a wall into a ring crushes that door onto facets a few
centimetres wide. FACTORY (`build_factory`, `emit_rounded_tower` in `src/game/scene.cpp`): the hall is a tower with a
rounded plan (its two rear corners are quarter circles, `kCornerFacets` 3 of 30 degrees, so smooth shading blends them)
and its roof rolled in over the top 0.85 m into a crown; the flat runs of wall keep the walls tile whole and upright at
its old scale, the facets of a rounded corner wear a plain corrugated strip of the same tile (no door crushed on a
facet), the door's face, jambs and lintel are untouched and flat, so the door is a door in a rounded wall (the hall's
front is deliberately the one flat face); the annex is rounded at all four corners with its own small crown; the stack
and its team band are twelve-sided cylinders. RECYCLER (`build_recycler`): the two tracks, the two skirts, the body and
the cab are swept side profiles cut by `rounded_profile` (the SAME rule as every vehicle's hull, now public in
`vehicle_shape.hpp`) with bevelled flanks; the painted and detail parts (wheels, side panel and strip, grilles, bumper
checker, plow and slit, vision band, radiator, the crane arm) stay flat panels on it, the cab's roof hatch is a flat
panel sunk into the rounded roof, and the radiator is sunk into the skirt so its rear end does not float over the
rounded shoulder. Footprint unchanged: every cut and bevel only takes material away (the factory's vertices stay
inside 0.3 tile of the unit, the recycler's extents 2.36 x 5.2 x 2.5 m are still pinned). Triangles: factory 84 -> 376,
recycler 396 -> 696. The sim, the crease angle and every battle hash are untouched.

**Round barrels (BA).** Harri: "I think all of the barrels should be round." Every gun and every muzzle collar was
a four-cornered BOX, which read as a square rod. They are now one round barrel (`emit_barrel`, built on `emit_loft`):
a ring of `kBarrelSides` = 12 corners swept from the gun's root to its tip, both ends closed, as wide and as tall as
the box it replaced, so no gun got longer, shorter or fatter and the sim, the aim and every battle hash are untouched.
Used by the tank, the hover tank, the assault tank, the defence turret and the walker's gun arm (the collars of
the tank, assault tank and turret are rings too). **This is the ONE place where MORE sides is right**: elsewhere a
higher side count destroys detail, because smooth shading averages any panels closer than the 75 degree crease and
the facets are lost; a barrel's goal IS the average, a cylinder, so 12 flats 30 degrees apart blend into one curved
surface while each end cap stays 90 degrees from the side and keeps its edge. A barrel is 132 vertices (44
triangles) where the box was 36 (12). The HUD icon draws the barrel side-on, where a cylinder is the same rectangle
as the box, so it is unchanged. The APC's light gun and the mortar bike's tube are authored parts of their shapes,
not barrels of this kind, and are left as they were.

**Smooth shading (AP, a try-it).** Everything lit -- terrain, hulls, buildings, turrets, wrecks and the
spheres of the repair beam and the smoke -- is shaded from a per-vertex NORMAL in the fragment stage, not
per flat face, so a faceted hull reads as a curved surface (Harri's "rounded soap bar", after
Battlezone 2). A vertex's normal is the AREA-WEIGHTED average of the faces that share its position,
except across a crease: faces whose normals are more than `kCreaseAngleDegrees` (75) apart are not
averaged, so a box keeps its square edges while a chamfered panel or a sphere rounds. The sky, the HUD and
the smoke/spark prisms carry no normal and are drawn exactly as before. It is a presentation change only:
no sim hash moves. It is meant to be judged by eye; to put it back, revert the commit.

### The command menu

The menu down the left (a selection's rows, the nav menu, an armed `N`) is drawn as BZ2 draws it:
each row is two translucent **blue bars**, the key's and the verb's, lit from the top and fading
down, with white text outlined in black. The keys are right-aligned on one edge, so a digit sits
close to its verb and a wide key (`SPACE`, `TAB`) grows to the left; between the key bars and the
verb bars runs one straight **amber line** (the HUD's amber, once the `SCRAP` label's) from below the state row
past the last row. The bars are rounded on their outer ends and square where the line runs. A row
that does not apply, or that you cannot afford, keeps its place and its key on a flat dark bar in
grey. The menu's text is smaller than the rest of the HUD's and smooth-edged (antialiased) inside
bars of the same size; a lowercase letter, where a row has one (the `m` of `17m`), is drawn as a
smaller capital on the same baseline.

A row that **completes an order** -- `Follow Me`, `Hold`, `Scavenge`, `Go to Nav`, `Attack` (through the attack list), `Recycle` -- closes the menu and clears the selection as soon as its orders are posted, which is what BZ2 does and what Harri asked for. So does every **shortcut that gives an order** (2026-11-01, Harri: "When I'm done issuing a command, like 'follow me', 'deploy', etc, the command is done, and the menu should go away, as should the selection"): `Space` attacking or following, `F`, `M`, `G` and a mark's number, and the click that lands a **deploy**; a group is cleared whole. The rows that **queue a build** or **arm a click** leave the selection alone: their sequence is not finished yet. (Until this was done those shortcuts left the selection standing, and `Space` never adds to a selection, so a turret could not be selected afterwards -- Harri: "I can't seem to be able to select turrets anymore".)

**Turrets take commands** (2026-11-01, Harri: "follow me", "defend", `SPACE Deploy`, "I can un-deploy them at will, but it takes maybe three seconds. Deploying takes maybe five seconds. There are other usual commands for turrets as well."). Select a turret (`SPACE` over it, or its `F` key) and the menu is the vehicle's, key for key, with two rows of its own in the gap the vehicle's table leaves: `1 Follow Me`, `2 Go to Nav`, `3 Attack`, `4 Hold`, `5 Pick Me Up` (dim), **`6 Defend`**, **`7 Undeploy`**, `9 Bailout`, `0 Recycle`. **`Defend`** sends each selected turret at the hostile asset nearest it that your radar shows (`no enemies within radar range.` if there is none): it drives until it is **within the turret's weapon range of that unit -- 16 tiles, `kTurretRangeRaw`, the turret's own `spec_of(Turret).range`** -- deploys there and starts shooting; if the enemy dies on the way it stops and stays a plain turret. **`SPACE Deploy`** appears as the state row only when turrets are selected and the crosshair is on bare ground (an attack on a hostile in the crosshair owns `SPACE` first); `SPACE` sends them to that spot and they deploy there. A deploy takes **5 seconds** (`kTurretDeployTicks`, 150) and an un-deploy **3** (`kTurretUndeployTicks`, 90); the gun does not fire while it is coming off its mount. **`7 Undeploy`** takes a deployed turret down where it stands (dim while nothing selected is deployed); any order that goes somewhere (`Follow Me`, `Go to Nav`, a move, `Defend`, a deploy elsewhere) takes it down first and then carries on, and `Hold` keeps it up. A deployed turret is the same unit with the same `F` key (it stays a vehicle, never a building key, and `Follow Me`, `Defend` and `Undeploy` all still work on it): it is `UnitKind::DeployedTurret` (16, appended) and goes back to `Turret` when it is taken down -- the first kind that changes both ways. A plain turret still mounts itself after three seconds of standing still, as it always did (`kMountTicks`); that older rule has not been retired.

**The factory builds eight things.** Select your factory (`2`) and its rows are the keys `1`-`8`, each
drawn with its cost in scrap on the row and dimmed while you cannot pay it: `1 Mortar Bike 45`,
`2 MSL Scout 45`, `3 Tank 55`, `4 Rocket Tank 65`, `5 Assault Tank 70`, `6 Walker 100`, `7 APC 50`,
`8 Bomber 75`. Five of the kinds are new -- the **assault tank** (the tank's gun on a heavier hull,
150 hp), the **walker** (200 hp, hits twice as hard and reloads half as fast, at half a tank's
speed), the **APC** (the fastest ground vehicle; it carries no troops yet, so it has a light gun),
the **bomber** (a wide, flat hull that is an aircraft that drops the mortar's splash shell, 14 tiles, with the mortar's own muzzle speed and fixed-speed lob -- Harri: "The flying bomber just drops the mortar") and the **MSL
Scout** (the scout's hull carrying the rocket tank's rocket; a vehicle of its own, not the Scout
at a second price). Every number here is a first proposal, **provisional** until it has been
played, and the new machines are placeholder shapes. The **scavenger** has 100 hp (it was 60) -- 25 scout shots to kill instead of 15 -- because a hauler should take a
beating.

**Names follow the faction, never the side.** A Scavenger and a Harvester are the same machine:
the sim has one kind for the pair, and the menu names it by whether the producer's faction is
human or alien. By default the player plays the humans and the enemy the aliens, but that mapping
belongs to the scenario (`SceneOptions::factions`), so a story mission can swap it. The human
recycler's rows are `1 Scavenger 20`, `2 Scout 50`, `3 Turret 40`, `4 Tug 45`, `5 Constructor 40`,
`6 Service Truck 50`, `7 Service Pod 2`; the alien recycler's are `1 Harvester 20`, `2 Scout`,
`3 Guardian 40`, `4 Hauler 30`, `5 Builder 40`, `6 Healer 50`, `7 Service Pod 2`.

A **deployed scavenger is an EXTRACTOR** (Harri, 2026-11-05: "the correct BattleZone 2 name of a deployed
scavenger"): the readout names it so, the name coming from the kind's own `kind_label` row. The scavenger
on wheels is still a SCAVENGER. Harri checked the alien side in BattleZone 2 (2026-11-05: "'Harvester' ->
'Extractor' once deployed. Same as the human side."), so the deployed name is EXTRACTOR for BOTH factions.

**One name table, keyed by (kind, faction)** (round 10): `kind_label(kind, faction)` is the only place a
unit's name is written, and the readout over a pointed unit, the attack readout and the attack list all read
it with the faction of the unit's own side. An alien miner reads HARVESTER (a human one SCAVENGER), an alien
turret GUARDIAN (TURRET), an alien tug HAULER (TUG); every other kind has one name for both.

**The human recycler builds all seven of its rows** (2026-11-03; Harri: "it's the recycler that isn't
able to build everything"). `3 Turret` queues the turret the sim already has -- a vehicle until it
deploys -- and `4 Tug`, `5 Constructor`, `6 Service Truck` and `7 Service Pod` are four new kinds
(`UnitKind` 17 to 20). Each is unarmed, has a shape drawn on the map and on its key from one authored
silhouette, takes an F key like any vehicle and drives. They do nothing else yet: the constructor
raises no buildings (that needs a placement feature that does not exist), the pod repairs nothing
(the truck does, below), and the tug tows nothing. The alien recycler's rows other than Harvester and Scout are still
names only, and its Hauler is 30 against the tug's 45, which is Harri's to rule. The store opens at
40 scrap, so the 45 and 50 rows (and the Scout) cannot be paid for until a deployed scavenger adds
room. The alien factory draws the human factory's eight rows until Harri supplies its own costs and names.

**The Service Truck repairs** (2026-11-05; Harri, playtest round 10: "Service Trucks should work"; the
sim half; the beam below is its look). A truck mends, by itself, every living friendly unit
within 6 tiles that is hurt: buildings, vehicles, mounted and unmounted turrets, the deployed
scavenger -- one rule for every kind. One truck at its closest restores `max_hp / 600` a tick, so a
tank goes from nearly dead to whole in 20 s and a factory (four times the hit points) takes 80 s.
Closer is faster (the rate falls with the squared distance to half at 6 tiles) and every truck in
range adds its own share. The mending is automatic for any truck in reach; sending one is the next
paragraph. A truck does not mend itself, and the service pod does not mend at all. The rate (`kServiceRepairTicks`)
and the 6 tiles are Harri's to tune. No recorded battle contains a truck, so none moved.

**The repair beam is a shotgun cone** (2026-11-06; Harri, round 11 item AJ: "the beam is like a shotgun where
anything in that general direction will also get repaired. Dead centre would mean lots of repair there.
Off to the side means barely any"; ruled: "distance picks the axis, cone decides the share ... I don't want
anything BEHIND a service vehicle repaired ... 90 degrees to the side, I would say no effect"; and the
shape, "a shotgun microphone pattern"). The cone's axis is the way the truck POINTS, which is the way it
last moved (a truck that has never moved points the way a stopped hull is drawn, up the map). A patient's
share of the repair is the distance rule above times cos^2 of the angle between that axis and the line to
the patient: 100% dead ahead, 75% at 30 degrees, 50% at 45, 25% at 60 and nothing at 90 degrees or behind
the truck. So everything in reach and in front of the truck is mended and the centre gets the most; a
truck sent to a patient (`3 Repair`, or the crosshair) arrives pointing at it. The beam is drawn only to
patients that are actually being mended. The drop-off is one number, `kServiceRepairConePower` in
`units.hpp` (1 is cos^2, a wide shotgun; 2 is cos^4, a tighter one). A truck that is sitting still with the
wounded beside it mends nothing until it is turned to face them (drive it a little). Battles with a service
truck in them mend differently; no recorded battle contains one, so none moved.

**The enemy sends its service trucks to the wounded** (2026-11-07; slice AN; playtest round 1, D1: "The AI
does not send trucks to the wounded"). An enemy service truck with nothing to do drives, by the very order your
`3 Repair` sends, to the NEAREST friend of its own side that is hurt below 80 hp (the line the factory already
keeps a hurt unit at home for; a unit whose full health is lower, like the mortar bike, counts as mended
at its own full health), as far as its own radar reaches, and it mends with the cone on arrival. A scratch
(80 hp or more) is not worth the trip; a unit already inside its factory's mending reach is left to the
factory; a truck on its way never drops its patient for a nearer or worse-hurt one, and goes on to the next
only when the first is whole (or dead -- the order simply ends). Two idle trucks may pick the same patient:
their mending adds. Nothing else about the AI moved, and no recorded battle contains a truck, so no recorded
battle changed.

**The repair beam** (2026-11-05; Harri: "a bright, trembling beam of different colours coming from the
service truck to the item being repaired"). While a truck is actually mending something, a beam of
28 bright cubes runs from the truck to that unit, one beam per truck and patient. It is
multi-coloured on purpose (red, orange, yellow, green, cyan, violet, pouring along toward the
patient) and it trembles: every cube is thrown about a third of a metre off the straight line, afresh
each sim tick, while the two ends stay put on the truck and the patient. A truck beside a unit at full
health, out of reach, or on the other side draws nothing. Drawing only: no recorded battle moved.

**The beam is spheres, and there are sparks** (2026-11-06; Harri: "the shapes should be fairly
transparent and spherical ... What about little tiny sparks?", and on the explosion, "I never see any
sparks"). The beam's 28 beads are now translucent spheres (20-sided icosahedra, alpha 0.4), and the weld at
the patient's end throws a constant shower of 8 sparks. The explosion throws 14 sparks too. A spark is
SMALL, BRIGHT and OPAQUE -- near-white hot cooling to amber, alpha 1, never faded -- because the
view's FXAA pulls a lone bright speck toward its neighbours, so each is kept about 2.5 pixels wide at
any distance rather than turning FXAA down for everything. The blast, the smoke and the sparks are
all drawn by one extracted spawn (`game::Particle`). Presentation only; no recorded battle moved.

**Smoke is round, puffs roll, and there are motes in the air** (2026-11-06; Harri: "I would eventually
like particles floating in the air for effect", and, of the eight-sided puff, "It's not what I had in
mind"). A puff of the explosion and of a wreck's smoke is now an ICOSPHERE (20 equal triangles, 12
corners, no pinching poles), and each carries its own slow roll drawn from the frame's seed, so
overlapping puffs are not copies. A dozen pale motes drift in the air round the camera: they are
rebuilt every frame from the seed and the tick, wrap at the edge of a 32 m box so they stay where they
hang as you move, and are view-only (no battle can depend on them). Sparks no longer fade into the
distance fog. Presentation only; no recorded battle moved.

**Repair by pointing** (2026-11-05; Harri: "maybe the same logic that's used to attack an enemy can be
used to repair a friendly. You know, targetting."). Select a service truck, point the crosshair at a
damaged friendly (within the same 4 degrees the attack offer uses) and the menu's top row reads `SPACE
Repair`, with a green line to that unit and a green dot on it -- the mirror of `SPACE Attack`'s red.
SPACE sends the truck: it drives to within 3 tiles of the patient (a new order, `Repair`) and its
automatic mending does the rest, then the order ends on its own when the patient is whole or gone. A
unit at full health, an enemy, a selection with no truck, or nothing selected is offered nothing; turn
away and the row and line go. In a mixed selection only the trucks go. The automatic mending stays as
it was (whether it should is Harri's call), and the AI does not yet send trucks to the wounded. No
recorded battle issues a `Repair`, so none moved.

**The service truck's own menu** (2026-11-06; Harri, playtest round 11: "The left side menu should be
different for service truckes. '3 Repair'. ... it should come up with the nearest ten assets of mine that
need repair, in order of vicinity. If something does not need repair, it is not listed. You don't always
see ten. The max range for anything to be repaired on the list is within radar range."). A selected
service truck's menu is the vehicle's with row `3` turned from `Attack` into `Repair`. Choosing it
replaces the menu with a list: `TAB Close Menu`, then up to ten of your own assets that are hurt, nearest
first, each on its digit (the tenth on `0`) with its name and its distance in metres, and a green line
and dot from the row to the asset. Anything at full health is not listed, so there are often fewer than
ten; with none to mend, `3` says so and the menu stays. The reach is the radar of the craft you are
DRIVING (Harri: "Whichever vehicle I'm in at the time sets the radar range"), not the truck's: the same
reach the attack list uses, so a scout's 80 tiles list what a tank's 40 would not. A digit sends the
selected truck (or trucks) to that asset with the `Repair` order above; Escape leaves the list. The list
is taken when `3` is pressed so the numbers hold still, and a patient that has died or been mended by
the time its key is pressed is refused. Menu and drawing only: no recorded battle moved.

**Vehicle identities** (2026-11-03, Harri's ruling): the **bomber** is the fastest vehicle in the game
(twice the tank's speed, was 3/4), the **mortar bike** next, fast and thin (5/4 of the tank's speed
and 40 hp, was half its speed and 75 hp), the **rocket tank** fairly slow (3/4 of the tank's speed,
was the tank's) and the **walker** the slowest and toughest (half the tank's speed, 200 hp, as
before). The turret's 1/4 crawl is slower still; it is a deployable weapon. Flight for the bomber is
a separate slice and is not here.

**Range belongs to the weapon** (2026-11-03, Harri: "'Rocket tank' has a long range because it has
ROCKETS, not because it's a rocket tank. Other vehicles that use rockets also get the same range."):
a kind's row names the weapon it mounts, and its reach is read from the weapon's, so two kinds
carrying one weapon cannot disagree. The reaches: tank gun 12 (tank, hover tank, assault tank,
walker), minigun 10 (scout, APC), mortar 14 (mortar bike, bomber), turret gun 16 (turret, deployed
turret), **rocket 18** (rocket tank, missile scout, and a rocket fired from any hull), shotgun 6.
The rocket tank and the missile scout therefore reach 18 tiles, not 15, and their rockets fly 20.

**The mortar bike shoots and scoots** (2026-11-03, Harri: "...they go to a spot, fire their mortar, and
then move to a different spot immediately"; it is the bike's behaviour, not the mortar's -- "It's the
automatic behaviour of the vehicle"). A bike that is attacking (an Attack order, which is also what
the AI gives it) walks to its range, fires, and at once drives to another spot on the circle round
its target, 1 tile inside its range, one or two sixteenths of the way round (a 5 or a 10 tile hop,
which it covers inside its 45-tick reload), and fires again from there. The spot comes from the
seed, the bike's id and the tick -- no runtime randomness -- and always keeps the target in range.
Two bikes that choose one spot shove each other apart like any two vehicles. A bike told to move,
guard or patrol does exactly that, and one you drive is yours. It does not make a bike safe: it is
the thing that makes a turret's bolt miss, not a shield (measured: against one turret a standing
bike lost about 39 hp per 100 ticks inside the turret's range and died in 31 of 32 duels; a
scooting one lost about 3 and died in 12 of 32).

**An attacker that cannot hit its target goes where it can** (2026-11-04, Harri: "I sometimes get units
to attack the enemy, but they just sit there, doing nothing. I assume it's because they are behind a
hill ... They should go to a different spot where they can actually hit the target from."). A unit
under an Attack order stops at its weapon's range, and a gun does not fire into a hill, so a unit
in range with a rise between it and its target used to stand there for ever. Now it drives to the
nearest tile within 6 tiles of where it stands that it can stand on, that is inside the weapon's
range of the target and from which it has a line, and fires from there. The same state always
picks the same tile. A mortar or bomber (its shell lobs over a hill) needs no line and does not
move; a turret cannot move, so one with no line simply does not fire; a hill wider than the 6
tiles of reach leaves the unit where it is.

**Shapes follow the faction too.** Harri: "The alien side assets need a different look -- not just
colours, but shapes too; they should be more organic." A kind's silhouette is chosen by the unit's
faction (`SceneOptions::factions`), in one place in `build_scene`, never by its side. So far one
role is done: the alien **harvester**, the Scavenger kind drawn for an alien unit -- a low domed
carapace on six legs that rises to its high point behind the head, a tapered proboscis dragging
behind it, and a crest of the side's colour down the spine, all built from ten-cornered swept rings
(`emit_loft`) rather than boxes. It is 5.5 m long and 2.3 m across, against the human hauler's 2.6 by
1.2. The human Scavenger is no longer the tank's hull: it is the first kind built from ONE SHAPE
(below). The buildings and the recycler are still hand-built, one mesh recipe in `scene.cpp` and one icon
recipe in `kind_icon.cpp` that nothing keeps in step; every other vehicle follows the scavenger, below. The shape is mine to propose and Harri's to judge by
looking.

**The alien kit (AZ) -- rounder masses, and ribs a normal average cannot flatten.** Harri: "The alien side
items used to look organic and interesting, but now they look more 'normal'. ... And lots of rounding", and
"Geometry with ribs steeper than the crease angle." This is a TRY-IT: the forms are my proposal, and if they
disappoint it is one commit to revert. Read this before you look. Smooth shading averages every pair of faces
closer than `kCreaseAngleDegrees` (75 degrees), so the old Harvester's 36-degree carapace panels and 60-degree
leg facets were all blended into a dome and no segmentation survived. Raising a side count only smooths more.
So detail is built as STEPS: `plated_stations` cuts a smooth sweep into overlapping PLATES -- each swells to the
full size and the next starts tucked in just behind it, so the edge between plates is a wall almost square to
the body that meets its neighbours at 90 degrees and over. The masses stay round (ten-sided rings); the plates
are crisp. What you should see:
- the **Harvester** (more bug): twelve shingled plates down the back like an isopod, a serrated dorsal comb, a
  segmented tail, six jointed legs of stacked cones, two curved mandibles and two eyes of the side colour;
- the **Guardian** (the aliens' Turret, a gun that is also an animal): crouched on four hooked claws under a
  stack of four flaring tiers, like a bracket fungus or a pagoda, with a beetle's carapace for a head that turns
  with the aim, two eyes, mandibles closing round the barrel's root, and a thorn leaning back where the human
  turret has its aerial. The gun itself is the same round gun;
- the **Hauler** (the aliens' Tug, a beast of burden): a squat plated ox-beetle on four pillar legs with a plated
  PACK of the side colour across its back, a pair of horns, and a tow-spike where the tug has its hitch.
The sizes stay: the Harvester is the size it was; the Guardian and the Hauler fit inside the turret's and the
tug's extents. The faction is chosen once, in `build_scene`, as it was for the scavenger; every other kind is
still the human model on both sides. No sim, hitbox, label or name changed.

**One shape for a kind: the scavenger's model and its icon come from the same description.**
Harri: "If the vehicles and buildings actually looked like [the icons]". Rather than modelling a mesh to
match a drawing, the scavenger's silhouette is written ONCE, in `src/game/vehicle_shape.cpp`
(`author()`; the types are in `game/vehicle_shape.hpp`): a list of convex side-on profiles in
metres, each with a lateral extent, a bevel and a role, in paint order. The world mesh
(`build_scavenger`) extrudes each part across its width with a chamfer; the 72 px plate under the
command row (`draw_scavenger`, fitted to the box by length) is `shape_covers` sampled per cell --
the same parts seen side-on, with the window and the wheels' rings cut. Neither holds a number about
what the machine looks like, so they cannot disagree. The machine is 3.13 m long, 1.24 m wide and
1.79 m tall: two tracked pods with five road wheels, an open-topped hopper heaped with scrap, a
cabover with wrapped glass, a stack, and a twin-armed boom that kinks at an elbow into a pincer. The
harvester (the same kind drawn for an alien faction) keeps its own organic mesh, so only the human
look and the shared plate come from this shape. The mechanism is now per kind (`shape_of(kind)`): the
tank is drawn and iconed from one authored shape too -- tracks with road wheels, glacis hull, fenders,
and a turret group that turns with the turret while the plate shows it level -- with the barrel kept
apart, since it elevates. The hover tank, the scout and the missile scout are drawn and iconed from
one authored shape each too (the hover tank keeps its skirt and barrel apart, the scout carries no
gun), and so are the rocket tank (the tank's chassis under a slanted launcher rack) and the assault tank
(the same chassis under a taller, wider block, its barrel kept apart). The mortar bike (two wheels in line, the mortar angled up), the
walker (two bent legs, its gun arm kept apart since it elevates) and the APC (four wheels a side, a turret
that turns) are drawn and iconed from one authored shape each too, and so are the bomber (a fuselage and
nosecone, a tall tail fin, a wing 2.4 m across and a bomb hung under it) and the defence turret (a flared
base and pedestal, a dome with a vision slit, the gun kept apart since it elevates). One turret shape serves
the turret, the deployed turret and a turret the player drives: they used to be built two different ways (a
pedestal undriven, a tank's hull driven), and the tank's hand-built hull recipe is deleted with that. The
buildings and the recycler keep their own recipes.

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
the kind in a **72 px square** at 720p (Harri: "four to five times the current lengths ... re-draw the
icons": 4.5 times the 16 px of the first cut, as wide as the key itself, as BZ2's are), RE-DRAWN at that size
rather than scaled up (each kind is a recipe of boxes, discs and polygons in `src/game/kind_icon.cpp`: road
wheels, hatches, barrels, doors), outlined one pixel in the keycap's near-black so the pale ink reads over
bright sky and sand, left-justified like the ticks, the same grey whoever owns the vehicle
and whether or not the key is selected (the type is shown, not the side). **A key plate carries exactly three
things: the key label, the icon and the ticks.** The plate is 72 x 34 px and the keys are 78 px apart, so the
strip hangs 128 px from the top of the frame (6 margin, 34 plate, 1, the 72 icon, 1, the 14 ticks); the notice,
the attack list and the command menu start below it. A frame too narrow for fourteen keys narrows the keys and
the icons with them. Thirteen kinds have their own
(tank, turret, hover tank, mortar bike, rocket tank, scavenger, recycler, scout, assault tank, walker, APC,
bomber, MSL scout), and so do the buildings (Harri: "the recycler and the factory should have icons
too"): the deployed recycler (`1`) a block with a tower, two stacks and two bay doors, the factory (`2`)
a sawtooth-roofed shed with a chimney, and the armory (`3`) a bunker with a missile standing on each side
of it, which is Harri's own description. The game cannot build an armory yet, so `3` is never drawn today;
its icon appears the moment it holds something. A kind with none draws a hollow box with a dot. `~` shows
no icon, and a key whose vehicles are all dead is not drawn at all. Under the
icon is a row of **upright ticks, one per living vehicle** of the group, left to
right in spawn order from the key's left edge (up to ten; nothing is ever drawn over them, because the notice,
the attack list and the menu all start below the strip). Every tick is the same size, 4 px wide and 14 tall,
however hurt its vehicle is: a tick's **colour** says how hurt it is, shading smoothly (per vehicle, per hit
point, never in bands) from bright green through yellow and red to near-black. A vehicle that dies takes
its tick with it and the ones after it close up to the left with no blanks. A building key has one tick.
The nav mark plate (`~`) has no ticks.

A vehicle's body follows the ground it stands on: it leans to the slope under it, by at most
`kMaxTiltSlope` (provisional, derived from the ride height and the hull's half-width). Structures
(factory, turret, deployed recycler) stay upright. Drawing only; the sim is untouched.

**The view leans with the hull** (2026-10-31, Harri: "I would like my view to be tilted the same
amount as that of the vehicle... Normally, I would be tilted with the vehicle I'm driving"). The
camera's up is the hull's own up, taken from the same `ground_normal` the body's lean comes from, so
the horizon rolls on screen by the vehicle's lean, and the eye is turned about the hull's ground
pivot with the same rotation that turns the body — the player stays over the same part of the vehicle
as it leans, which is what makes a sideways vehicle give a sideways view. On level ground the hull's
up is exactly world up, so the basis and the eye are identical to what they were before. Two things
stay as they were: the screen furniture (bars, key plates, ticks, radar) keeps its screen position,
because it is placed by the pick ray that now uses the tilted basis. The aim cue takes that same
basis (2026-10-31, Harri: "the crosshairs don't turn with the vehicle — they stay plumb with the
world. I think they should stay with the view, not the world"), so its four ticks and its centre dot
stay square to the screen however the horizon rolls; it had been left on the world-upright basis,
which is exactly what put the one mark on that layer out of step with the ground, the units and
the HUD drawn around it. A test measures the mark's shape in SCREEN pixels through
the renderer's own projection, so it cannot agree with the drawing code by construction. Measured on a
live frame, a tank standing on a gentle slope
rolls the horizon about 3.4 degrees, and the horizon sits 70 rows lower than the level-hull frame.
Its one honest limit: facing downhill, once the nose drops more than about 3.4 degrees below the
barrel-eye's view, the muzzle tip leaves the bottom of the frame.

There is no health bar above the radar (retired 2026-10-23). The hull you are sitting in is read off the
**ship instrument**, bottom right: a C-shaped ring of segments, its upper arc the hull in green and its
lower arc the ammunition in blue, the two readings as numbers in the ring's opening, and the four
weapons beside it with the one you have up lit. A small **health bar floats over an asset only on demand** (2026-11-01, Harri: "should not be there until I either select something on my side, or point approximately at it, or have it targetted with the t key"): when that asset is **selected** (yours), or the crosshair is **on it or within 4 degrees of it** -- the same cone `T` and `Space` use, `kAssistConeDegrees`, any asset of either side -- or it is **`T`'s latched target**. Any asset: a tank, a turret or the factory alike (the viewer's own tank never has one). Green for yours, red for a hostile. The bar is **level with gravity, not with the screen**: when the view banks with the hull the bars bank with the world, parallel to the horizon, at the same size and the same place over the unit; the camera's own lean is unchanged. A unit counting down a **deploy** keeps its deploy bar whatever the above, since the wait is progress, not health; with no health bar showing it takes the health bar's place. The readout that names the enemy you have targeted is **red at every health** as well, so a hostile's bar can never be read as one of yours, whose colour falls with its damage.

**Point at a thing and read what it is** (2026-11-03, Harri: "When I 'point' at something (within a few degrees), I see the health bar, which is great, but can it also display the type of item I'm pointing at, even if it's an enemy item? ... Under the unit, it should also display the distance to it ..."): the asset the crosshair is on or within 4 degrees of (the same `pointed_asset` the health bar takes, so the bar and this readout cannot disagree about what is pointed at) shows its **name at the top, the health bar right under it, and its distance in metres under the unit** (`123 M`), the distance's plate as far below the unit's middle as the bars' outer edge is above it. The name and bar ride the unit's projected top, so **the nearer you get the higher they go**, and they are the **same size in pixels at any range**: the lettering is the radar's N S E W size (2 px cells, 10 px tall). Vehicles and buildings, **yours and the enemy's**. The ink says whose: **green yours, red theirs** (nobody's -- the ground, a biometal pool -- would be white, but only assets are read). It shows **only while pointed at**; **`T` on the pointed asset locks it** (his own too, not only an enemy), so it stays while you look elsewhere until you press `T` on it again or it dies; `T` on another asset moves the lock and `T` on nothing lets it go. `T` still latches the assist's dot and line on a hostile exactly as before -- the lock is a separate, additional thing `T` does, and the assist itself is unchanged. The unit `Space` would attack keeps its own red label from the attack offer (no second name over it).
**The target diamond** (2026-11-08, round 1 slice AQ; Harri: "When I hit t to target an enemy asset, the radar and the map do not get a diamond around the target.  They should, and it should not go away with the radar blip -- it should stay until I hit t again, or the target is destroyed."): `T` on a hostile already locked the readout (`Interaction::readout_lock`; `T` again lets it go, `T` on another moves it, and the lock goes when the unit dies or leaves the radar's reach); that lock now also reaches the radar and the map, which each draw a red diamond ring round the target (`radar_target`, `WorldMap::target`, one shape: four bars, `kTargetDiamondFit` x the dot it rings, `kTargetDiamondBar`). It is made from the unit's POSITION, not from its blip, so it does not fade with the sweep or its tail (a still enemy has no blip between sweeps and keeps its diamond) and the sweep, fade and tail constants are untouched; the dot sizes are untouched too. It is player intent held in the HUD, never sim state. Only a hostile gets a diamond; locking one of his own units still holds its readout but draws none.

**The crosshair is the weapon** (2026-11-03, Harri: "the crosshairs should actually change to indicate what weapon I have any given moment. The lines in the crosshairs should be finer. Right now the lines are very bold, and it looks like a kids game"). Five thin shapes, one per carried weapon, each round an open centre with a small dot in it: the **gun** four ticks (up, down, left, right); the **rocket** four corner brackets, a lock-on frame; the **mortar** a ring, its splash; the **bouncer** four ticks on the diagonals, an X, for the ricochet; the **shotgun** four dots on the diagonals, its spread. The lines are 1.4 px at 720 high (they were 4.4 px, the rocket's 7.9) and the weapon's NAME is no longer drawn beside the crosshair: the ship instrument says it. The reticle keeps everything else it had: red, green or white by what a click would do, dimmed while the weapon reloads, pushed outward by a hit.

**The map, the scrap gauge and the deployed buildings** (2026-11-03, Harri's nine rulings after playing). The world map (`R`) opens **half the size**, 165.12 px square at 1280x720 where it was 330.24 (half the radar's long axis, `kWorldMapRadarFraction`), and carries no `WORLD MAP - R CLOSES` caption. The amber `SCRAP` plate above the radar is gone: the **store gauge** beside the radar (or the map) writes the same number under its column. The gauge's column is now **separable chunks with a small gap between them** (1.5 text cells, 3 px at 720): one green chunk for the 40 base and one gold chunk for each deployed scavenger's 20, so counting the gold chunks counts your scavengers and a lost one is a missing chunk at a glance; the white bar is cut by the same gaps. The gauge was **broken in map mode**: its pill was one quad the panel's height and its middle sagged nearer to the eye than the sections in front of it, so beside the 330 px map only 62 of the green's 203 rows showed and none of the gold's (just the white bar over it); the pill is bands no taller than a chunk now, and a pixel test counts the chunks down the real rendered column. A **deployed scavenger** stands plumb on a slope like the factory, the turret and the deployed recycler (`stands_level`); it was the one building still leaning with the ground. The ground itself is untouched.

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
sim accepts an explicit Attack order on a friendly. (Neither `Space` nor the attack list is one of these: `Space` is only
offered a hostile, the list names only hostiles, and the keyboard has no key that names a friendly.) That is
deliberate: it means you can destroy your own tank or your own factory if you mean to, without your units
ever turning on each other without being told. A unit may never name *itself*, though, so an accidental
order cannot make a tank shoot itself.

**Everybody tries not to shoot through a friend** (Harri: "not shooting at our own ships should be
attempted"). Before a gun takes an automatic shot it flies that shot on a copy of the nearby units, by the
real flight; if it would hurt a unit on its own side it does not take it and picks the next target it can
shoot cleanly, or holds fire. A straight bolt is stopped by the first body in its lane (a hostile in front
shields the friend behind it); a lobbed mortar shell goes *over* the friends under its arc and is judged by
where it comes down: the body it lands on and the 1.5-tile splash. This is only the *decision*: **friendly
fire damage is unchanged** -- a round that does reach a friend still hurts it, and your own trigger and an
explicit order on a friendly are never second-guessed. It is a try, not a guarantee: it judges positions as
they are now, so a friend who drives into the lane afterwards is still hit. Tightly packed units can go
quiet while they are packed.

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
| **The economy can be played, but not yet ordered** | wreckage (scrap) lies where units die, **one scavenger a side** collects it **by a trip** -- no load, no factory needed: an idle scavenger looks for the nearest piece inside its radar (40 tiles) once a second, **drives to it, and takes it only when it is at the pile** (within one tile, `kScavengeCollectRadiusRaw`); the piece never vanishes from a distance, and the `SCRAP` count rises on the beat after it arrives (a pile it is already standing at is taken at once). With nothing in its radar it simply stops. To work new ground, select it, `1 Follow Me` and lead it there, then select it and `3 Scavenge` (a `Stop`: it goes back to collecting). A **deployed** scavenger is unchanged: its pool is drawn in, not fetched. A scavenger under any order (following, holding, moving) takes nothing. The store is capped (see the next row). What a producer's queue is ordered to build is paid for from that count. Nothing builds on its own: the sim has no automatic production, and the enemy builds nothing until it has an AI that decides to |
| **Biometal pools, and the scrap store's capacity** | a map lays **3 to 10 biometal pools** from its terrain seed (`sim::pool_count_for`, `lay_pools`; a designed map sets its own count), **balanced by side** (Harri, 2026-11-02: "no seed can hand one side the economy"): a pool belongs to whichever base is nearer (the perpendicular bisector between the two), each half gets `count / 2` and the odd one goes to a side the seed picks, with the positions still random inside each half and every rejection (open ground, 14 tiles from a base, the 24-12-6-3-1 spacing) unchanged. Seed 42 lays 9: 5 and 4 (it was 1 and 8). A pool is an **infinite** source at a fixed tile: it stores no amount. A scavenger told to deploy **on a pool** drives there, stands `scavenger_deploy_ticks` (**150 ticks, 5 s**, Harri's number, 2026-11-01; the same for both sides; the recycler keeps its own 300) and becomes a `DeployedScavenger`, which extracts **one unit per beat per side**, at **two rates**: **fast** (1 a second, `kPoolFastIntervalTicks` 30) while the bank's level sits inside the **gold band** and **slow** (1 every 5 s, `kPoolSlowIntervalTicks` 150) once it climbs into the green. Neither rate depends on how many scavengers or pools you hold; each deployed scavenger makes the **fast region 20 units taller**, so three give a 60-unit band at the fast rate, never a faster rate -- spending keeps you earning fast. Litter keeps its own rate (1 a second per scavenger), independent of the band, and is the fastest income in the game. **One scavenger per pool**; to put another there you must destroy the first. The player gets there with **`SPACE Deploy`**: select a **single** scavenger, point at the pool within the narrow arc, and the menu's top row reads `SPACE Deploy`; `SPACE` sends it. The prompt exists only in that context -- a group never gets it, nor a scavenger with no pool under the cone -- and it is not one of the menu's permanent rows. While it shows, **a thin white line runs from the `SPACE Deploy` row to the centre of the pool** (Harri, 2026-11-03), one pixel wide at 720 and drawn from the same offer as the verb, so it comes and goes with the prompt; turrets' `SPACE Deploy` is onto bare ground and has no pool, so no line. A deployed scavenger takes no order and is never un-deployed (its capacity is committed while it lives; a deployed TURRET, below, grants none and goes back at will). **A side's store holds 40 at the start** (`Scenario::store_capacity`; Instant Action is always 40) and **each completed deployed scavenger adds 20** -- space only, never scrap. **A full store stops collection**, at a pool and from litter alike (litter stays on the ground); **a recycle the store cannot absorb scatters the remainder as collectable litter where the unit stood** (nothing is destroyed or blocked); losing a deployed scavenger drops the capacity by 20 and **destroys any scrap above the new capacity**. Both sides play by these rules. On the **enemy** side any vehicle pointing at a free pool (within 4 degrees of its facing, and inside its own radar) sends a scavenger to deploy there, provided its side has no litter in reach. **Either side's scavenger with NO ORDER deploys itself** (Harri, 2026-11-01): idle, no litter in its own radar, and a free pool in that same radar, and it goes to the pool and deploys. Any order -- Follow Me, Go To, anything -- suspends this for as long as it stands, and going idle resumes it, so you can lead a scavenger past a pool; `SPACE Deploy` stays for intent. **The store gauge** sits right beside the radar: a vertical pill whose whole extent is the capacity, a **green** section on top for the base 40 and a **gold** section added at the bottom for each deployed scavenger's 20, a white bar up to what you hold, and that number under it. **A pool vents**: a plume of steam rises from it over **altered ground** (a 5x5 patch of teal biometal ground; there is no disc or rim any more), so it reads from the ground as something coming out of the ground. **A deployed thing alters the ground around it** (Harri, on the recycler and again on the scavenger: "it should look like the ground was altered"): ONE mechanism (`game/site_ground.hpp`) in which each kind says how far and which atlas tile -- the deployed recycler a 4x4 pad of steel plating (`site_pad`: its 2x2 footprint and one tile beyond), the deployed scavenger a 3x3 rig of grating with hazard corners (`site_rig`), a pool a 5x5 vent (`site_vent`) -- and the scene paints those tiles in place of the ordinary ground. Where two reach one tile the stronger kind wins (recycler over scavenger over pool). It is paint only: the tiles stay drivable (outside the building's own footprint, which is the sim's rule as before) and no hash moves. **The recycler is twice as big** (Harri, 2026-11-02: "Maybe twice as big. It's pretty big in BattleZone 2."; one constant, `kRecyclerScale`, 2.0) and a **deployed one stands square on its tiles**, centred on its footprint like the pad under it; a recycler still driving keeps its own heading. Drawing only: the solid footprint stays 4 m square and no hash moves. A 55-scrap tank cannot be afforded at 40 until a scavenger has deployed. `tests/test_pools.cpp` pins each rule |
| **The war can end, and the HUD says so** | the conditions are the scenario's own data (`sim::Objective{verdict, side, kind}`, asked in order by `Sim::war_outcome`), and the list the game plays by default is the recycler pair: it is lost when your recycler is destroyed and won when the enemy's is — in either form, the vehicle or the base it deploys into. A factory is still a 400 hp target that a single tank can destroy — factories are unarmed on purpose and defence is what turrets are for — but under the built-in list only the recycler decides the match |
| **Most refusals are on the console, not the screen** | text is drawn (see Display/HUD), but only a refused roster F-key or nav-mark digit puts its sentence on screen; every other refusal ("that order would move the tank you are in") goes to the console. The radar carries no text, and nothing marks the unit your gun is engaging |
| **Little feedback that YOUR shot happened** | the bolt is drawn, your gun is heard, the reticle spreads for 0.2 seconds when your shot damages an enemy and a mortar shell leaves a landing mark. There is no muzzle flash and no recoil, and a shot that misses gives no reticle response |
| **No positional audio beyond left and right** | a sound is placed on a line between the speakers: no front or back, no height, no echo or occlusion. A shot behind a hill sounds exactly like one in the open |
| **Sounds are generated, not recorded** | they are made of noise and sine waves and sound like it. A recording would sound better and bring files, licences and a loader; that trade has not been made |
| **A wreck's smoke is round across, not round up the side** | each puff of a kill's burst and column WAS an eight-cornered prism (an icosphere now, below) (84 vertices; it was a 36-vertex box), as wide across its flats and as tall as the box was, so from above or at a slant it reads as a drum and not a die. From the side a plume is still a stack of drums, because the prism is flat-topped; a rounder profile (a bevelled or sphere-like puff) is not built (SUPERSEDED, AH and AK: a puff is now an icosphere everywhere -- the blast, the smoke, the motes and the pool's steam, which goes through `emit_particle` like the rest; only a spark is a prism, a four-sided cube) |
| **Silence beyond earshot is deliberate** | 60 tiles for a shot, 90 for a death; past it there is no voice at all. It is not a bug to report when a distant fight makes no noise |
| **Sounds queue, they do not overlap across frames** | the device plays what it is given in order, so a burst of fire within one sound's length waits its turn, and a buffer arriving with more than about three quarters of a second already queued is dropped rather than played late |
| **A quit never waits on the sound device** | the device is closed with a two-second budget. A driver can deadlock SDL's own audio threads (seen on SDL 3.4.18 with PipeWire, where a device-change event wedges SDL's close for ever), so a close that has not returned makes the game leave without it and print `protekt-game: the audio device did not close within 2000 ms; exiting without it`. Nothing is lost: the process is quitting anyway, and a healthy close takes a couple of milliseconds. It leaves with status 1, because its shutdown did not finish |
| **The radar is range-based, not line-of-sight** | a hostile behind a hill inside its reach (40 tiles; 80 in a scout) is still on it. Battlezone's radar worked the same way, and this is the version to play before adding occlusion |
| **Compass** | the panel turns with the tank; N, E, S, W sit on the ring for the true world directions, from the same yaw as the marks |
| **Vehicles shove, they do not pass through each other** | (2026-11-03; Harri: "Maybe I should be able to push other vehicles out of the way, but with difficulty.  That way I won't get trapped.  That's how it is in BattleZone 2.") a mobile unit can no longer be driven through another: it pushes the vehicle ahead aside, **at two thirds of its own speed** (`kShoveSpeedNumerator/kShoveSpeedDenominator` = 2/3; it was 1/2 until Harri found the shove "very easy"), and the shoved vehicle is carried that step with half as much again to the side it already lies on, so a head-on pair slides past instead of cancelling. Either side's vehicle, friend or foe; buildings behave as before. It is never a wall: a vehicle that cannot be carried (the map's edge, a crater, a building) is driven through at the same two-thirds speed, and a unit that has shoved for 45 ticks (`kShoveYieldTicks`) in a row stops colliding until it has been clear for a tick, which is what breaks a head-on stalemate the sim's lack of pathfinding could not. A unit mid-deploy is not moved. The numbers are provisional and Harri's to set. This is a SIM change: two recorded values moved (`Observer.RenamingTheSupportVehiclesMovedNoRecordedBattle`, the scripted skirmish's final hash, and `Observer.TheFiredStreamForAScriptedBattleIsUnchanged`, 70 shots to 71, first five unchanged) and were re-recorded |
| **Two units on one panel pixel** | the depth test decides, so one colour wins. A hostile within about a tile of you hides under your own mark |
| **Lighting is one sun, no shadows** | the ground is painted from textures (six variants of each terrain type, each tile turned one of eight ways) and units are built from several lit prisms, all shaded by one directional sun. Nothing casts a shadow: each living unit, the one you drive included, stands on a dark translucent blob laid flat on the ground (sized from its footprint) as a stand-in, and the textures are generated, not authored art. Distance fog blends the world toward the sky's horizon colour: nothing within 5 m, solid at the edge of the meshed ground, and linear in between, so the terrain's own paint gives way to the sky's colour with distance instead of ending in a line. The blend is the last thing done to a pixel, in the fragment stage after each tile's texture has been multiplied in, so the haze keeps its full strength all the way to the rim; the vertex carries the distance as an amount and never a fogged colour. Measured on a live frame, the ground shifts 23 levels of 255 at the far band, 8 at the middle distance and under 1 underfoot. It was blended into the light where the scene's vertices are built until 2026-10-31, which let the texture multiply the haze away: the ground then moved by less than 8 levels at any distance, and the player correctly reported that he could see no fog at all. The HUD (bars, panels, radar, reticle) is never fogged |

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

To build only the game for a play test, **`cmake --build build -j8 --target protekt-game`**. It skips every
test object -- 65 source objects against 100 test objects, so the tests are about 60% of the compile and none
of the playing. It will NOT catch a test file that fails to compile; the gate (`tools/ci.sh`) does that.

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

# the same shell at the steepest aim (1.28 rad, lands 8.1 tiles out), the view levelled on frame 25 to watch it land
./build/protekt-game --headless --no-sound --seed 42 --switch 2 --look-up 128 --fire 18 --look-level 25 --frames 90 --capture /tmp/lob-high.ppm

# the radar off, for comparing two frames pixel by pixel (the game always has one)
./build/protekt-game --headless --seed 42 --frames 60 --autodrive --no-radar --capture without.ppm

# the edge smoothing off, to compare a frame with and without it (the default is on); same seed and frames,
# so only the filter differs and the state hash printed is the same
./build/protekt-game --headless --seed 42 --frames 3 --capture /tmp/with-fxaa.ppm
./build/protekt-game --headless --seed 42 --frames 3 --no-fxaa --capture /tmp/without-fxaa.ppm

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
metal and building tiles with a few percent of tone variation, the hover tank's bright skirt
band (`hover_skirt`), and the three **site tiles** (45-47: `site_pad` steel plating, `site_rig` grating
with hazard corners, `site_vent` teal biometal ground) that a deployed recycler, a deployed scavenger and a
pool paint into the ground round them. `--seed` repaints the patterns; the
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
