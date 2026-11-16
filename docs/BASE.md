# The base: producers, production, buildings, power, upgrades

What the base is made of, what each producer can build and for how much, how power works on each side, what can
be upgraded, and how a building is placed. This is the REFERENCE for the base: the decisions themselves are in
`docs/REQUIREMENTS.md`, the order things happened in is in `docs/PLAN.md`, and this file says what is known and
where it came from.

**Markers.** `[CAPTURED]` read off one of Harri's own Battlezone Combat Commander 2.0.185 captures; `[RULED]` his
decision; `[REFERENCE]` from a published guide or the level editor; `[PROPOSED]` the assistant's, for him to
correct; `[NOT BUILT]` decided and absent from the tree; `[SHIPPED]` in the tree. A slice that builds one of these
items moves its marker.

**Names.** Where the game's own screens and the documents disagree, the SCREENS win, because a panel is what the
player reads: the panels carry short forms (`TRAINING FAC.`, `ANTENNA`) and the guides carry long ones ("Training
Center", "Antenna Mound"). One deliberate exception, his ruling: PROTEKT prints **CENTRE**, so `TECH CENTRE` where
the game prints `Tech Center`.

**A second deliberate divergence, his ruling (2026-11-13, asked as a choice of two): the Scion shield is spelled
`Absorption` in PROTEKT**, where the game's own screens and the guides print `Absorbtion` (a misspelling). It sits
beside `TECH CENTRE` and `TRAINING CTR.` as a DECISION: a later session sees a choice that was made, not a typo
that was missed, and must not "fix" it back (11.6).

## 1. Factions, and which side owns what

`Faction { Human, Alien }` (`include/protekt/game/faction.hpp`). A side's faction decides which names, which
build lists and which silhouettes are used; the sim itself knows no faction and no recorded battle moves when the
pair is swapped (`--alien`).

**The name table** (`kind_label`, `src/game/row.cpp`) is keyed by `(kind, faction)`, never by side:

| kind | human | alien |
|---|---|---|
| Scavenger (the mobile one) | `SCAVENGER` | `HARVESTER` |
| Turret, and the DeployedTurret it becomes | `TURRET` | `GUARDIAN` |
| Tug | `TUG` | `HAULER` |
| DeployedScavenger (the deployed one) | `EXTRACTOR` | `EXTRACTOR` |
| everything else | one name for both | |

The Constructor, the Service Truck and the Service Pod have ONE name in the table today; the alien side's own
words for them -- **BUILDER**, **HEALER** -- exist as labels on the Matriarch's rows, not in the table. Which
casing the menu's strings take is not ruled.

**Side ownership, where the sides are NOT symmetric** `[CAPTURED]` `[RULED]`:

- **The Factory is HUMAN ONLY.**  The alien equivalent is the **Kiln**.
- **The Bomber Bay is HUMAN ONLY.**  The alien side has no equivalent.
- **The Jammer and the Stronghold are ALIEN ONLY.**
- The deployed recycler is the alien **MATRIARCH** -- a name for the same kind, not a kind of its own.

## 2. The producers and their rosters

Six rosters exist. Three are in the tree (`kHumanBuildList`, `kAlienBuildList`, `kFactoryBuildList`, all in
`src/game/menu.cpp`); the rest are captured (or, for the Kiln's three, ruled and unnamed) and not yet in it.

### 2.1 The deployed recycler -- human, seven rows `[SHIPPED]`

| key | row | cost | builds |
|---|---|---|---|
| 1 | `Scavenger` | 20 | the Scavenger kind |
| 2 | `Scout` | 50 | the Scout kind |
| 3 | `Turret` | 40 | the Turret kind (a vehicle until it deploys) |
| 4 | `Tug` | 45 | the Tug kind |
| 5 | `Constructor` | 40 | the Constructor kind |
| 6 | `Service Truck` | 50 | the ServiceTruck kind |
| 7 | `Service Pod` | 2 | the ServicePod kind |

Every row is a real kind: the price drawn is read from the sim's spec table, so the price shown and the price
charged are one number.

### 2.2 The Matriarch -- alien, seven rows `[SHIPPED]`, four of them inert

| key | row | cost | builds |
|---|---|---|---|
| 1 | `Harvester` | 20 | the Scavenger kind |
| 2 | `Scout` | 50 | the Scout kind |
| 3 | `Guardian` | 40 | **nothing** -- a name row awaiting structure placement |
| 4 | `Hauler` | 30 | **nothing** today; its kind (Tug) exists |
| 5 | `Builder` | 40 | **nothing** today; its kind (Constructor) exists |
| 6 | `Healer` | 50 | **nothing** today; its kind (ServiceTruck) exists |
| 7 | `Service Pod` | 2 | the ServicePod kind |

So three of the four dead rows need only their kind wired up, and the fourth -- the Guardian -- needs the
structure-placement feature that the construct system brings.

### 2.3 The Factory -- human only, eight rows `[SHIPPED]`

| key | row | cost | build time |
|---|---|---|---|
| 1 | `Mortar Bike` | 45 | 900 |
| 2 | `MSL Scout` | 45 | 900 |
| 3 | `Tank` | 55 | 900 |
| 4 | `Rocket Tank` | 65 | 1300 |
| 5 | `Assault Tank` | 70 | 1400 |
| 6 | `Walker` | 100 | 2000 |
| 7 | `APC` | 50 | 1000 |
| 8 | `Bomber` | 75 | 1500 |

The Factory answers for both factions in the tree today, in its own comment's words *"both factions, until the
alien factory's own list is supplied"* -- and the Forge's list IS supplied now (2.4; the Kiln's three are not
named), so that comment is out of date.

### 2.4 The Forge's six, and the Kiln's light three -- alien

**`[RULED]`, a SELF-CORRECTION by him (2026-11-13): the Kiln offers ONLY THE LIGHT THREE, and his capture of "the two
panels" was the FORGE TWICE.** So the earlier reading of this section -- one identical six-row list on both panels
-- is WITHDRAWN, and the guide was right that the Forge adds the **Lancer, Mauler, Archer and Titan** (11.1). The
reference's "early production" (Kiln) against "most of your forces" (Forge) IS a difference in his game.

**The Forge's list `[CAPTURED]`** (what he photographed; the capture was the Forge's):

| key | row | cost |
|---|---|---|
| 1 | `Sentry` | 50 |
| 2 | `Lancer` | 55 |
| 3 | `Warrior` | 55 |
| 4 | `Archer` | 65 |
| 5 | `Mauler` | 70 |
| 6 | `Titan` | 90 |

One page, six rows. **The Kiln's list is NOT captured**: he says three light rows, and the sources do not NAME
them. What the sources do say: the guide's four additions (Lancer, Mauler, Archer, Titan) leave **Sentry and
Warrior** of the six, which is two, not three -- so the Kiln's third row, and which rows are the three, are
**`[NOT BUILT]` and unrecorded**, not guessed here. (The Scout Drone is on the Matriarch's list, 2.2.) Costs of the
Kiln's rows are presumably the Forge's rows' own, which is `[PROPOSED]`.

### 2.5 The Constructor's CONSTRUCT list -- human, nine buildings

| key | row | cost | |
|---|---|---|---|
| 1 | `POWER` | 30 | `[CAPTURED]` |
| 2 | `FACTORY` | 55 | `[CAPTURED]` |
| 3 | `RELAY BUNKER` | 50 | `[CAPTURED]` |
| 4 | `ARMORY` | 60 | his recollection; the cost column is under the selection overlay in both captures |
| 5 | `SERVICE BAY` | 50 | his recollection, same reason |
| 6 | `GUN TOWER` | 50 | his recollection, same reason |
| 7 | `TRAINING CTR.` | 70 | his recollection; the game's row prints `TRAINING FAC.` |
| 8 | `TECH CENTRE` | 80 | 80 in one read, covered in the other |
| 9 | `BOMBER BAY` | 100 | `[CAPTURED]` |

The costs may change, by his word.

### 2.6 The Builder's BUILD list -- alien, six buildings `[CAPTURED]`

| key | row | cost |
|---|---|---|
| 1 | `KILN` | 60 |
| 2 | `GUN SPIRE` | 75 |
| 3 | `ANTENNA` | 60 |
| 4 | `DOWER` | 50 |
| 5 | `STRONGHOLD` | 70 |
| 6 | `JAMMER` | 50 |

One page. (`ANTENNA` is the panel's short form of the Antenna Mound.)

**`[SHIPPED]` (round 18, K6): all six rows raise a building** through the same job as the human nine (the press
arms it, SPACE places it, the Builder drives within one tile, the build rises on its tick timer). Kinds 29-34
(`Kiln` .. `Jammer`), costs as above. **The gate is a BUILDING, never power** `[RULED]`: `ANTENNA` and `DOWER` need
a standing Kiln, `STRONGHOLD` a Forge, `JAMMER` an Overseer; no row reads the human power or connection rule. The
Forge and the Overseer are upgrades in place and **no upgrade is built**, so in play the Stronghold and the Jammer
cannot be raised yet (their rows stay dim, naming the missing building).

**The `JAMMER` gate as shipped is now WRONG** `[RULED]`: he ruled (2026-11-13, by choice) that the Jammer needs the
**ANTENNA MOUND**, not the Overseer Array (3.3). Changing the tree's gate is a slice, `[NOT BUILT]`, not a doc
edit; and since an Overseer Array IS an upgraded Antenna Mound, the same "an upgrade must not take a prerequisite
away" rule as 3.3 applies, `[PROPOSED]`.

**Footprints were PROVISIONAL when this paragraph was written** (Kiln 2x2, Gun Spire 1x1, Antenna 1x1, Dower
2x2, Stronghold 2x1 -- the one rectangle, so `~` turned it -- Jammer 1x1, all in the pre-scale unit). **He has
since given every alien footprint, all square, and they are `[RULED]` in 3.4**; the tree's constants
(`include/protekt/sim/units.hpp`, `scaled_footprint`) already read Kiln 8x8, Gun Spire 4x4, Antenna 8x8, Dower
8x8, Stronghold 4x4, Jammer 4x4. **Looks** are authored in the Harvester's
idiom (plated organic lofts and blades, no boxes). **`[NOT BUILT]`: every function** -- the Kiln produces nothing,
the Gun Spire does not shoot, the Antenna detects nothing, the Dower services and repairs nothing, the Jammer jams
nothing, the Stronghold constructs no weapons and no shields -- **and the Power Lungs** (built in, destructible,
disabling, easy to repair, hidden from the `3 Attack` list: the five rules of 4.1) do not exist. **This tree's alien
side has NO POWER AT ALL `[SHIPPED]`, and that is now WRONG** (4.1): the Lungs and the DISABLED state are slices,
not doc edits.

### 2.7 The Builder's UPGRADE list `[CAPTURED]`

One list, and its targets are **mixed**: a building (the `KILN`) and units (two `EXTRACTOR`s) appear together,
keyed `1`, `2`, `3`. Each row reads **`cost | distance`** -- `60|83m` -- and the verb row echoes the selected
target's pair (`SPACE UPGRADE 60|83m`). The list is ordered **nearest first**, and the world floats the same
distance (`83m`) beside the highlighted thing.

So Build and Upgrade are separate verbs, and an upgrade is not a row you place: it is a thing done to something
that already exists.

### 2.8 The build-time rule `[RULED]`

**20 ticks a scrap** -- a building's or a unit's build time is its cost times twenty, so the 30-scrap Power
Generator takes 600 ticks. The tree already uses this for its units, with ONE documented exception: the human Tank
costs 55 and takes 900 rather than 1100. Whether the buildings inherit the exception is not ruled.

## 3. The buildings

Footprints are in TILES, width x height, and **the columns below show the CURRENT values first**: the `[RULED]`
x4 scale of 2026-11-13 (3.8) and the alien footprints he then gave (3.4, the full table with its provenance). The
tree has a per-kind table, `footprint_of(kind)` (units.hpp). The OLD pre-scale values, which were `[PROPOSED]`
starting points he had accepted, are the footnote under each table: they are history, kept so the record loses
nothing.

### 3.1 Human

| building | row | cost | footprint (current) | prerequisite | what it does | upgrade |
|---|---|---|---|---|---|---|
| Power Generator | `POWER` | 30 | 4x4 | -- | provides **3 units of power** | -- |
| Factory | `FACTORY` | 55 | 8x8 | -- | builds the human eight (2.3) | **none** -- his recollection |
| Relay Bunker | `RELAY BUNKER` | 50 | 4x4 | -- | **extends the human build area** (wireless power) | -- |
| Armory | `ARMORY` | 60 | 4x4 | -- | arms vehicles | -- |
| Service Bay | `SERVICE BAY` | 50 | 8x8 | -- | repairs vehicles | -- |
| Gun Tower | `GUN TOWER` | 50 | 4x4 | -- | base defence; a power CONSUMER | -- |
| Training Centre | `TRAINING CTR.` | 70 | 8x4 | -- | **UNKNOWN -- parked by him**; the ISDF guide answers it `[REFERENCE]`: unlocks the **MAG** (12.3) | -- |
| Tech Centre | `TECH CENTRE` | 80 | 8x4 | -- | **UNKNOWN -- parked by him** ("a requirement for something else"); the ISDF guide answers it `[REFERENCE]`: the **advanced-weapons** building (12.3), and it **unlocks the Shadower** `[RULED]` (12.4) | -- |
| Bomber Bay | `BOMBER BAY` | 100 | 8x8 | -- | houses the bomber, which never leaves it on its own | -- |

*Footnote -- the OLD pre-scale footprints, superseded:* Power Generator 2x2, Factory 4x4, Relay Bunker 2x2, Armory
3x3, Service Bay 3x3, Gun Tower 1x1, Training Centre 3x3, Tech Centre 3x3, Bomber Bay 4x4. (These old figures do not
all match 3.4's "x4 of" provenance -- 3.4 derives Power from 1x1, Armory from 1x1, Training and Tech Centre from
2x1, Service Bay from 2x2 -- so the old column was not the exact input to the scale. Not resolved; 3.4's values
are the ones in force.)

### 3.2 Alien

Lung counts are `[RULED]` where they are given: a building carries **one** lung and its upgrade carries **two**.
Where a count is not given it is `?` and is his to set.

| building | row | cost | footprint (current) | prerequisite | what it does | upgrade | lungs |
|---|---|---|---|---|---|---|---|
| Power Lung | -- | 0 | a COMPONENT, about a quarter of its structure each way (4.2); no grid footprint of its own is given | -- | a building's own power, built into it (4.1) | -- | the lung itself |
| Kiln | `KILN` | 60 | 8x8 | a built Power Lung `[REFERENCE]` | early production; **the light three only** (2.4) | `-> FORGE`, 60 scrap | 1 |
| Forge | -- | 120 as a structure; 60 to upgrade a Kiln | 8x8 | the Kiln upgraded | production; **the six** (2.4) | -- | 2 |
| Antenna Mound | `ANTENNA` | 60 | 8x8 | a built Kiln `[RULED]` | the relay/comms role | `-> OVERSEER ARRAY`, 80 scrap, **needs a Forge** | 1 |
| Overseer Array | -- | -- | 8x8 | the Antenna upgraded | -- | -- | 2 |
| Dower | `DOWER` | 50 | 8x8 | a built Kiln `[RULED]` | services and repairs vehicles; **needed for Healers** | -- | ? |
| Stronghold | `STRONGHOLD` | 70 | 4x4 | a Forge `[RULED]` | the alien **Armory**, and it also grants shields | -- | ? |
| Jammer | `JAMMER` | 50 | 4x4 | **an Antenna Mound** `[RULED]` (his earlier "Overseer" reading is withdrawn, 3.3) | blinds the enemy's radar and degrades their targeting nearby | -- | ? |
| Gun Spire | `GUN SPIRE` | 75 | 4x4 | a Power Lung `[REFERENCE]`; the Lung is also **built in** (4.1) | the alien gun tower; silhouette `[CAPTURED]` in 3.9 | -- | ? |

*Footnote -- the OLD pre-scale footprints, superseded:* Power Lung 2x2, Kiln 3x3, Forge 3x3, Antenna Mound 3x3,
Overseer Array 3x3, Dower 3x3, Stronghold 3x3, Jammer 2x2, Gun Spire 1x1. (2.6 also records the earlier
provisional set Kiln 2x2, Gun Spire 1x1, Antenna 1x1, Dower 2x2, Stronghold 2x1, Jammer 1x1.) The Power Lung's
2x2 was a placeholder from before he ruled it a component.

### 3.3 The prerequisite tree, as it stands

Confirmed in his own play: the **ANTENNA** and the **DOWER** each need a built **KILN**; the **STRONGHOLD** needs a
**FORGE**; the **ANTENNA'S UPGRADE** needs a Forge. **The JAMMER needs the ANTENNA MOUND** `[RULED]` -- his ruling
by choice on 2026-11-13, NOT the assistant's: his earlier play-reading, "Jammer <- Overseer", is WITHDRAWN and the
GUIDE wins (11.1 row 1). The disagreement is closed. From the reference,
unconfirmed: the Kiln and the Gun Spire need a Power Lung; the Forge needs the Kiln and two Power Lungs; the
Overseer Array needs a Forge, an Antenna and two Power Lungs.

"Needs a Kiln" should read **"a Kiln or a Forge"**, since a Forge IS the upgraded Kiln: an upgrade must not take
a prerequisite away.

### 3.4 Footprints, in TILES, after the x4 scale -- human and alien `[RULED]`

A TILE is about **four tank lengths** (he worked it out from the satellite view). All values below are tiles,
width x height; a turned rectangle swaps them (`~`). The tree's own constants are `scaled_footprint(w, h)` in
`include/protekt/sim/units.hpp`.

| building | tiles | provenance |
|---|---|---|
| Power Generator | 4x4 | x4 of 1x1 |
| Armory | 4x4 | x4 of 1x1 |
| Relay Bunker | 4x4 | x4 of 1x1; **he confirmed** the 1x1 ("The dimensions for Relay Bunker and Gun Tower are correct") |
| Gun Tower | 4x4 | x4 of 1x1; **confirmed** the same way |
| Training Centre | 8x4 | x4 of 2x1 |
| Tech Centre | 8x4 | x4 of 2x1 |
| Factory | 8x8 | x4 of 2x2; **8x8 in BOTH states** -- the one a Constructor places and the one a scenario spawns as a point unit |
| Service Bay | 8x8 | x4 of 2x2 |
| Bomber Bay | 8x8 | x4 of 2x2 |
| Deployed Recycler | 8x8 | 2x2 -> 8x8, **ruled** ("we also need to scale the factory and recycler, including a deployed recycler") |
| **Stronghold** (alien) | **4x4** | his; **CHANGED** from the provisional 8x4; given twice, agreeing |
| **Gun Spire** (alien) | **4x4** | his; confirmed (given twice, agreeing) |
| **Jammer** (alien) | **4x4** | his; confirmed (given twice, agreeing) |
| **Dower** (alien) | **8x8** | his; confirmed |
| **Kiln** (alien) | **8x8** | his: "Kiln is the same size as a Forge" |
| **Forge** (alien; the Kiln upgraded) | **8x8** | his; an UPGRADE, not a build row |
| **Antenna** (alien) | **8x8** | his; he **corrected** my provisional 4x4 |
| **Overseer Array** (alien; the Antenna upgraded) | **8x8** | his; equal to the Antenna's |
| **Matriarch** (alien recycler) | **8x8** | his; matches the human deployed recycler's 8x8, so the two sides' producers agree |
| **Extractor** (a deployed scavenger, both factions) | **4x4** | his; see below |

The six alien BUILD rows, in the order of 2.6: KILN 8x8, GUN SPIRE 4x4, ANTENNA 8x8, DOWER 8x8, STRONGHOLD 4x4,
JAMMER 4x4. His wording on the final list: *"Stronghold, Extractor, Gun Spire, and Jammer are all 4 x 4.
Matriarch, Dower, Forge, and Overseer Array are all 8 x 8."*

**THE RULE HE CORRECTED: an upgrade does NOT change the footprint.** The assistant had written that "an upgrade
may change the footprint", because the Overseer Array's 8x8 looked bigger than the Antenna's provisional 4x4. That
was wrong and the wrong value was the assistant's, not his: *"the Antenna to Overseer Array 'staying' at 8 x 8 is
the correct specification."* The Forge is the Kiln at 8x8; the Overseer Array is the Antenna at 8x8; an upgrade
replaces the building with one of the same size. Every footprint an upgrade has equals its base's.

**EVERY ALIEN BUILDING IS SQUARE** -- his BELIEF, hedged ("I THINK all the buildings are square on the alien side
... I may have made a mistake"), not a measurement: at the time the alien side had no overhead view to measure
from. If the alien side gains one (it has, at the Overseer Array, 3.7), re-measure. The four the assistant asked about and the four he had
already given are all square, so it is at least consistent.

**THE EXTRACTOR is not a construct building.** In the name table (section 1) "Extractor" is what a DEPLOYED
SCAVENGER is called on BOTH factions, the alien deployed Harvester included. So his 4x4 is a deployed scavenger's
footprint, **which it does not have yet** `[NOT BUILT]`. Giving it one makes it sixteen tiles of ground no unit
may cross, like any standing building. That is a SIM change: if a recorded battle walks a unit across an
extractor, the recording moves and must be named, proven and re-recorded.

### 3.5 The alien buildings' JOBS `[RULED]`

His words (2026-11-13): *"And I can see the human base from it! I suspect the Jammer made the alien side invisible
via satellite in the other scenario. Dower is the Service Bay equivalent. Stronghold is the Armory. There is no
Jammer on the human side."*

| alien building | job | human twin |
|---|---|---|
| Matriarch | producer (its own roster, 2.2) + the loadout editor | the deployed Recycler |
| Kiln / Forge | producer (the Kiln's light three, the Forge's six, 2.4) + the loadout editor | the Factory |
| Dower | **the Service Bay's equivalent** -- repairs vehicles | the Service Bay |
| Stronghold | **the Armory** -- unlocks the good weapons | the Armory |
| Jammer | **counter-detection**: it hides its side from the satellite view | **NONE -- he says so outright** |
| Antenna / Overseer Array | the satellite view (3.6: at the Overseer Array for certain) | the Relay Bunker |
| Gun Spire | a gun station: he mans it and shoots | the Gun Tower |

**Two pairings that change the slice plans** (the assistant's, for him to correct):
1. **Dower = Service Bay.** The Service Bay slice (the drive-on platform, 3.8) should be built as a PAIR, the
   human drive-on repair platform and its alien equivalent, using ONE mechanism.
2. **Stronghold = Armory.** The loadout-editor work therefore has two homes on the alien side (the Matriarch and
   the Kiln/Forge) and one on the human side (the Armory *unlocks*; the editor is entered at the producers, 3.6),
   and the two sides' unlock rules have to be designed together.

The Stronghold's shields, and its "tier-2 weapon upgrades", are the guides' (11.1, `[REFERENCE]`); that it
grants shields where the human Armory does not is a second asymmetry (13), **his to confirm**.

### 3.6 ENTERABILITY -- both sides `[RULED]`

How a building is entered: **on foot**, **by vehicle**, or **a station you man**. His human list (2026-11-13):
*"I can NOT enter a training facility or a technical centre. I can walk into a Bomber Bay, go up the stairs, and
look at the bomber, but there is nothing to do. Same with the service bay -- there is nothing there to do. Power
units have no entrances, nor does the Armory. I can go into the deployed recycler, factory, relay bunker, and gun
tower. When I'm in the gun tower, I can aim and shoot, and all the usual point-at-press-space, function keys, etc."*

**Human**

| building | enterable | entered by | what is inside |
|---|---|---|---|
| Power | NO | -- | -- |
| Armory | NO | -- | -- (its job is to UNLOCK options, not to be walked into) |
| Training Ctr. | NO | -- | -- |
| Tech Centre | NO | -- | -- |
| Factory | YES | on foot | the loadout editor |
| Deployed Recycler | YES | on foot | the loadout editor |
| Relay Bunker | YES | on foot | a SATELLITE VIEW and remote orders |
| Gun Tower | YES | on foot | **A STATION**: the player mans its gun -- aim, shoot, and the usual point-and-SPACE, function keys, all of it |
| Service Bay | YES | on foot, and **by VEHICLE** (driving onto the platform) | nothing to do inside; the repair is on the platform (3.8) |
| Bomber Bay | YES | on foot | nothing to do; the bomber is visible up a flight of interior STAIRS |

**Alien** (*"On foot, I can also go into an alien gun spire, matriarch, and forge. That's before and after any
upgrade. I can shoot from a gun spire, and change the loadout from the matriarch and forge."* and *"Overseer Array
can be entered on-foot! It gives the satellite view."*)

| alien building | enterable | entered by | what is inside |
|---|---|---|---|
| Gun Spire | YES | on foot | a STATION: he can shoot from it |
| Matriarch | YES | on foot | the loadout editor, like the human producer |
| Kiln and Forge (both, before and after the upgrade) | YES | on foot | the loadout editor |
| **Overseer Array** | **YES** | on foot | **the satellite view** |
| Antenna (before the upgrade) | `[PROPOSED]` YES -- **his ASSUMPTION** ("I ASSUME the Antenna can also be entered, and will give satellite view") | on foot | the view is NOT confirmed before the upgrade (see 3.7) |
| Dower, Stronghold, Jammer | **not named by him** | -- | **assumed NONE until he says otherwise** (assumed, not invented) |

**Not mentioned by him, and the assistant's reading** `[PROPOSED]`: the **deployed turret** -- in Battlezone 2 a
turret can be hopped into and manned, and this tree already lets a player swap into any armed kind (the same seam
as the gun tower); the **wheeled recycler** is a VEHICLE, one drives it rather than enters it; the **Service Pod**
is used by driving OVER it (his words).

**Enterability is a PER-BUILDING PROPERTY, not a special case** (it came from a defect, 3.8): every building's
footprint blocks units by default, which is right for a Power plant and an Antenna and wrong for a Service Bay. An
enterable building's footprint still refuses to have ANOTHER building placed inside it, and still blocks nothing
else.

**The correction this list forced**: the assistant had recorded the ARMORY as where the loadout editor lives. He
says it has NO entrance and the recycler and the factory do, which matches his original sentence -- "make
adjustments to the weapon load-out for what the recycler and factory can build" -- the editor is at the PRODUCER.

**He simplified it: NO INTERIOR IS REQUIRED -- an EXTERIOR CONTROL PANEL** (*"To make it easy, I don't think I HAVE
to go inside a building. There could be some control panel outside that I access by pointing at it as a soldier,
and hit space to select it."*). Three features collapse into one mechanism, a panel opened from an EXTERIOR CONTROL
POINT, selected by pointing and pressing SPACE (the tree's aimed-action grammar):

| building | exterior control panel | opens |
|---|---|---|
| Factory / deployed Recycler (and, alien, Matriarch / Kiln / Forge) | yes | the loadout editor (the producer's blueprints) |
| Relay Bunker (and, alien, Overseer Array) | yes | the satellite view + remote orders |
| Gun Tower (and, alien, Gun Spire) | yes | the gun station: aim and shoot, orders live |
| Service Bay | a drive-on PLATFORM rather than a panel | repairs what parks on it (3.8) |
| Power, Armory, Training Ctr., Tech Centre, Bomber Bay | none | -- |

The **interiors are OPTIONAL FLAVOUR, not a requirement**, the Bomber Bay's stairs and the view of the bomber
included (an interior with levels is a feature of its own, named and not scheduled). The panels are reached **ON
FOOT** (his words open with "on foot", twice), which keeps INFANTRY in the critical path for all of them, human and
alien, and for the APC's dismount: the cheaper driver's-seat access is NOT what he wants (14).

### 3.7 The satellite view: it shows the MAP and what is BUILT, not what is MOVING `[PROPOSED]`

Two of his findings, side by side: over the ENEMY base he sees **no enemy ships** ("I don't actually see any enemy
ships"); from an Overseer Array he **can see the human base**. Together they give the rule: the overhead view shows
the map and what is BUILT on it, **not what is moving about it**. Coherent, and worth confirming with him rather
than assumed.

**The Jammer is what bends it** (his suspicion: *"the Jammer made the alien side invisible via satellite in the
other scenario"*): the Jammer's side stays hidden from the view. The assistant's reading: the Jammer's side's own
units stay hidden even where its structures show.

**Where the view comes from.** Human: the **Relay Bunker**, from the start. Alien: it was first thought there was
none (*"There is no satellite view"*, twice -- the Antenna he had been looking at gave none); then **the Overseer
Array gives it**. So on the alien side the view is the UPGRADE's GIFT, not a building's own purpose: the upgrade
buys reach, not just a second lung. **Unresolved, and testable in play**: the research guide (11.1) and his own
assumption both say the un-upgraded Antenna gives the view; walk into an un-upgraded Antenna and see. If the view
comes up it is the building's own, if not it is the upgrade's. The table has the same shape either way. This also
explains why his alien footprint measurements were beliefs before the Overseer and are measurements after.

### 3.8 Scale, the ghost, the connection rule, the Service Bay and the bolt `[RULED]`

**The scale** (*"I realized a 'tile' is actually about four tank lengths. Can we make all buildings four times as
wide, long, and tall? ... The buildings seem... puny. So, 1 x 1 becomes 4 x 4. 2 x 1 becomes 8 x 4."*): footprints
x4 in tiles (3.4) and MODELS x4 in width, length AND height -- the part he sees. The clean way is ONE scale applied
to a building's shape rather than re-authoring every number. Everything that reads a footprint follows: the preview
grid, the pick box, the reserved tiles no unit may enter, the site rules, the build job's site. The 15 placeable
buildings (human nine + alien six) scaled first; **he then ruled the deployed recycler and the factory too**
(*"I'm afraid we also need to scale the factory and recycler, including a deployed recycler"*): the deployed
recycler 2x2 -> 8x8 with its model x4; the wheeled recycler's model x4 (it has no footprint, being a vehicle); the
factory's model x4 and 8x8 in both states. The scenario-spawned factory is what moves recorded battles: the sim
now reserves **64 tiles** around a factory that used to be a point, so a unit that walked through there walks
around it. **The proof required of the slice**: not "the hash moved" but that THIS change moved it -- revert the
footprint, show the old hash return, restore, and report both numbers.

**The ghost** (*"a 'ghost' of the finished building so you can see which way it's facing. Just a very transparent
green or red glowing wireframe, depending on whether it can be built there or not."*): the armed preview draws,
besides the footprint grid, the building's OWN SHAPE as a very transparent glowing wireframe at the site, GREEN
where the sim would allow it and RED where it would refuse. It shows the FACING, which matters now rectangles turn.
Drawing only: it cannot move a battle.

**The connection rule** (his, restated):
- **EVERY building must be CONNECTED BY TOUCHING.** `POWER` may stand next to a RECYCLER or a RELAY BUNKER, **or
  next to a building that is itself touching one of those** -- a plant may chain ONE step out from the hub.
- **Only a RELAY BUNKER may be built touching nothing** -- it extends the reach, so it may stand alone.
- **A RECYCLER may be placed almost anywhere too, except on bad ground.**

This SUPERSEDES the narrower rule built the day before (K4: only the rows that need power -- Factory, Relay Bunker,
Gun Tower -- required a connection). One rule for all rows, the exceptions named in it. It sits beside, and does
not replace, the human build-area rule of section 4 (adjacent to the recycler or a Relay Bunker -- human only).
The alien side's gate is still a building, never power (2.6).

**The Service Bay's platform -- a DEFECT he hit** (*"I can't seem to be able to drive onto the platform on the
Service Bay. If I'm in its vicinity, nothing gets repaired. The health bar stays where it is. I should be able to
drive onto the platform."*): (1) a footprint BLOCKED units, wrong for a Service Bay, so enterability is per
building (3.6); (2) nothing repairs, correctly unbuilt, but a platform you can drive onto that does nothing is
worse than one you cannot enter, so the repair is built WITH the drivability, **reusing the Service Truck's own
repair arithmetic (the cone's `max_hp/600` a tick)**, not a second repair. The AMMO half stays blocked and
declared: PROTEKT's weapons have cooldowns, not rounds. A unit inside is repaired at the truck's rate; one outside
is not; no other building may be placed inside it.

**THE BOLT / LOB RULE -- a building's FOOTPRINT is its hit shape.** His bug: *"I was hiding behind the enemy
recycler. The alien guardian was still able to shoot at me, right through the recycler. The bolts should have been
interrupted by the recycler."* The diagnosis: a footprint blocked units (8x8 tiles for the recycler) and was drawn
(**9.44 x 20.8 m**), but its HIT BODY was a point-sized circle -- the projectile hit radius, half a tile -- so a bolt
crossed the drawn building and only the invisible point could stop it. The same hit test is the friendly-fire
probe's, which is why the Guardian fired at all (it believed nothing was in the way). The bug predates the scale (a
2x2 recycler with a point body); the x4 made it unmissable. **The rule**: the footprint is the hit shape -- the
same square that blocks units and refuses placements, one rule with three uses that cannot drift from what is
drawn. **Both sides: cover is cover**, and the probe reads the same test, so a gun also stops firing through its
own buildings. A SIM change: bolts stop earlier, so recorded battles may move -- named, proven and re-recorded.
(The commit history names this as `fix(sim): a building stops a bolt -- its footprint is its hit shape`.) The task brief calls this "the bolt/lob rule"; the sources spoke of BOLTS only, and the lob half was **his to
rule -- and he has** (below).

**THE LOB RULE `[RULED]` -- THE ARC DECIDES.** Asked as a choice (2026-11-13), his answer: *"The arc decides: a
shell that clears the roof lands behind it."* So **a lobbed shell (mortar, bomb) is NOT stopped by a footprint the
way a bolt is**: a bolt is stopped at the edge it enters, a shell is stopped only if its arc does not clear the
building -- and a shell that clears the roof lands BEHIND the building. This closes the gap M1 honestly reported (the
bolt rule recorded, the lob unrecorded), and gives his own earlier sentence its mechanism: *"I should still be able
to lob a mortar over a recycler to hit the target behind it, depending on the location."* The bolt rule and the lob
rule are ONE subject -- what a projectile does when it meets a footprint -- and are written here side by side.
Status: the tree's `meets_building` (`src/sim/projectile.cpp`) already treats a shell this way -- a shell above the
roof as it crosses the footprint goes over, and one that comes down inside the rectangle is stopped on the roof --
so the rule reads `[SHIPPED]` by that code; I did not run a battle to prove it for this slice, and what the tree
takes as a building's "top" height is not restated here.

**What the scale slices were** (history, for provenance): L1 the scale, L2 the ghost, L3 the connection rule for
every row (re-pinning K4's tests deliberately), M1 the Service Bay drive-on platform (after L1 and L2, because
those change every footprint).

### 3.9 The Gun Spire's silhouette `[CAPTURED]`

From the cropped capture of a Gun Spire with its Lung (the same crop as 4.2), 2026-11-13:

- **three tall vertical prongs** on a wide sloped base; each prong about **4-5 times taller than it is wide**; the
  **two outer prongs lean outward 5-10 degrees** and the centre one is vertical; a **circular ring near the top of
  each outer prong** and a **vertical slot in the centre one**; the gaps between prongs about one prong wide;
- a **wide trapezoidal pedestal**, wider at the bottom, sloping inward **15-20 degrees** from vertical, with **5-6
  horizontal rib bands** across the front and a **central vertical seam** suggesting two halves; the base about a
  third of the total height;
- **dark metal with teal/cyan accent lines** and **6-8 small emissive YELLOW lights** (a row of about four low on
  the base, one near the top of each prong);
- overall **slightly taller than its base is wide** (about 1.1-1.2 to 1), a crown-like profile.

So the alien six's provisional looks (authored in the Harvester's idiom, 2.6) get a REAL reference for the Gun
Spire, worth its own slice. The authored shape is `[SHIPPED]`; matching it to this capture is `[NOT BUILT]`.

## 4. Power

**The sides are asymmetric, by his ruling.**

**Human: shared, and positional.**  A Power Generator grants **3 units** of power; consumers draw **different**
amounts; **the number on the screen is the SURPLUS**, not a currency. Scrap buys a generator and nothing else.
And the human build area is **ADJACENT to the deployed recycler or to a Relay Bunker** -- wireless power -- so the
Relay Bunker's job is to EXTEND where you may build. This rule is human-only, in his words: *"That's only for the
human side."*

**Alien: one lung per building, and a lung is a target.**  Each alien building carries its own Power Lung, so no
adjacency rule applies and losing a lung disables ONE building rather than the grid. Lung counts: Kiln 1, Forge 2,
Antenna Mound 1, Overseer Array 2 `[RULED]`; the rest unset. **Destroying a lung DISABLES the building**; the
**Builder** rebuilds it with **`6 Add Lung`**, which is **free** `[RULED]` (`[NOT BUILT]`, 6). What a lung looks
like is `[CAPTURED]` (4.2); the earlier `[RULED]` description -- "a rotating, crystalline, translucent bulb about a
metre tall" -- was the first word, and its size was provisional and his to change.

### 4.1 The Power Lung: five rules `[RULED]`, built in slice N3 (sim); the standalone Lung and `6 Add Lung` stay `[NOT BUILT]`

His own words (2026-11-13): *"When you build a Gun Spire, or ANYTHING on the alien side that needs a Lung, the Lungs
are built into the structure. When you build the structure, it automatically comes with the Lung. As a matter of
fact, if I am on the human side, and I want to get past a Gun Spire, I direct my assets to attack the Lung. That
disables the Gun Spire, no matter what is connected to it. They are easy to repair. They do not show up in the "3
Attack" list, so you have to actually POINT at a lung to direct anything to attack it specifically."* And, asked
whether the aliens get Lungs, his answer: *"Give the aliens Lungs -- the guide says their buildings spawn with one
attached."*

1. **A Lung is BUILT IN.** A structure that needs one comes with it: building the structure builds its Lung, with no
   separate placement step for the ones that belong to a structure. (The guide's standalone Lungs are for powering
   a Gun Spire or replacing a destroyed one; the Builder's `6 Add Lung`, section 6.)
2. **A Lung is a DESTRUCTIBLE COMPONENT of the structure.** A gun can target it and kill it, and killing it
   **DISABLES the structure** -- it does not destroy it. Disabled, not gone.
3. **"No matter what is connected to it":** the structure's OWN Lung is its own power, so a structure that is also
   tethered to something else is still disabled by losing its own Lung.
4. **They are EASY TO REPAIR** -- cheap, deliberately, so the counter-play is a raid, not a demolition.
5. **They are DELIBERATELY HIDDEN from the `3 Attack` list:** a Lung is attacked only by POINTING at it -- the same
   instinct as the exterior control panel (point at a thing, press SPACE; 3.6).

**BUILT (N3, sim):** rules 1-5 for the six alien buildings. A Lung is a `PowerLung` unit (`Unit::host` links it to its
structure), appended when the structure stands; killing it DISABLES the structure (`structure_disabled`: the Gun Spire
stops firing, the Dower's platform stops mending) and never destroys it; a friendly Constructor beside the structure
restores it for 5 scrap and 90 ticks (PROVISIONAL; he has given no figure); it is hit only by a bolt AIMED at it, which
passes through its own structure's walls; it is not in the `3 Attack` list, and the assist cone never finds it -- only
an exact pick does. The Gun Spire shoots now, with the Gun Tower's gun (PROVISIONAL). **Not built:** a standalone Lung,
the Builder's `6 Add Lung` row (so there is no order to SEND a Builder to restore one), a producer's gate (no alien
producer exists), the Forge's second Lung, and the Lung's slow spin and translucency.

**WHAT THIS MAKES WRONG: the alien side's current "no power at all" (`[SHIPPED]`, the six built with "the gate is a
building", 2.6).** Say it plainly: the alien side is to have Lungs, built into the structures that need them, plus
standalone ones to replace a destroyed one. **These are slices, not doc edits** -- the Lungs and the new
**DISABLED state** are a sim change (a destructible component and a state the structure can be in), `[NOT BUILT]`.

**Not resolved by it:** the prerequisites the guide gives (the Kiln and the Gun Spire need a Power Lung, the Forge two
-- 3.3) sit oddly with Lungs that come WITH the structure. His words confirm the Gun Spire carries a Lung; whether
any standalone Lung must be placed FIRST as a prerequisite is not ruled. Per-building Lung counts for the Dower,
Stronghold, Jammer and Gun Spire stay unset. The Lung's repair cost and rate are not given.

### 4.2 The Lung's look `[CAPTURED]`

He said (2026-11-13): *"Basically an upside down lightbulb."* **The impression was his; the picture corrected the
FORM.** The capture (a cropped Gun Spire with its Lung) shows a **low-poly FACETED CRYSTAL**, not a smooth bulb on a
neck:

- **shape**: roughly ovoid/teardrop, **widening low and tapering to a rounded point at the top**; about **8 to 12
  irregular flat facets** meeting at sharp edges, like a chunky gemstone;
- **colour and light**: **pale green/cyan**, brighter at the centre, **EMISSIVE** -- it glows from within rather
  than being lit -- and **semi-transparent**;
- **motion**: it **rotates slowly**, a separate object with its own spin, not part of the structure's mesh; the spin
  is about a near-vertical axis, the pivot at the neck;
- **attachment**: a **short dark neck or bracket on the OUTER side of the right prong, about halfway up it**,
  protruding outward -- not on the base;
- **size**: height about **25-30% of the whole structure's height** (about 40% of a prong's height); width about
  **20-25% of the base's width** and slightly wider than one prong -- **about a QUARTER of the structure's height
  and width**. The x4 scale rule (3.8) does not answer this: a Lung is a component, not a building on the grid.

That is the Gun Spire's right-hand prong (3.9); where a Lung sits on the other five alien structures is not captured.
The metre-tall figure of the earlier description (above) and this proportion are not reconciled: with the x4 scale
the Gun Spire's height is not stated in metres here. Not built: `[NOT BUILT]`.

**Either side: a destroyed building scatters scrap** on the ground for a scavenger to collect -- power and scrap
touch at construction time and nowhere else `[RULED 2026-10-24]`.

## 5. Upgrades

**Alien: buildings.**  Two paths, both priced and both chained:

| from | to | cost | needs | gain |
|---|---|---|---|---|
| Kiln | Forge | 60 | the Kiln | a second lung, and the Forge |
| Antenna Mound | Overseer Array | 80 | **a Forge standing** | a second lung, and the relay role |

**Human: exactly one target.**  Only a **DEPLOYED EXTRACTOR** can be upgraded `[RULED]`. The **Constructor** does
it, and it must **travel to the deployed scavenger** to do so -- an action performed ON the machine, not an order
given to it -- with an **animation**, after which the machine **looks more elaborate**. Its cost is **60**, the
same as a Kiln's. Its effect is on the store gauge: **the extractor's section turns RED**, **red sections sit at
the BOTTOM of the gauge**, and a **red one accumulates scrap FASTER than a yellow one**. (The gauge already works
in sections with different rates: green is slowest, yellow is what a deployed scavenger gives.)

**What cannot be upgraded**: the Factory `[his recollection]`; and mobile scavengers or harvesters -- only
DEPLOYED ones `[RULED]`. Whether the alien Harvester can be upgraded he does not recall, and the evidence points
to no: the Builder's upgrade list held extractors.

## 6. Placement, and the panels

**The preview.**  A building in hand paints a translucent green footprint on the ground, `[CAPTURED]`:

- the machine's name above it (`BUILDER`), the build's name beside it (`BUILD KILN`), and a shortfall line under
  that: **`ADD 52 SCRAP`**; a size or distance readout at its base (`1.6M`, `19M`).
- green where the placement is good and red where it is not. PROTEKT already owns this: `append_deploy_preview`
  draws "the sim's answer to *would SPACE deploy here?*", green when it is.

**The panel grammar** `[CAPTURED]`: instruction rows first -- `SPACE / PREVIOUS PAGE`, `TAB / CLOSE MENU`,
`~ / ROTATE BUILDINGS` -- then a SECTION HEADING naming the list (`UPGRADE`), then keyed rows carrying
`cost | distance` where a distance applies. The instruction rows are drawn even when there is only one page, so
their presence is not evidence of paging.

The `~` row's LABEL FOLLOWS THE SUBJECT: `ROTATE BUILDINGS` on a construction vehicle, `DROP OFF` on a producer.
**The Drop Off items are ignored entirely, by his ruling.**

**The verbs and their keys** `[CAPTURED]`, on the Constructor and the Builder:

| key | verb | state |
|---|---|---|
| `~` | `Select Unit` | shipped |
| `1` | `Follow Me` | shipped |
| `2` | `Go To Nav` | shipped |
| `3` | `Build` (PROTEKT's `3 Construct`) | the PANEL is shipped (round 17 J1: nine rows, every press refused -- nothing can be placed yet); placement is `[NOT BUILT]` |
| `4` | `Upgrade` | `[NOT BUILT]` |
| `5` | `Demolish` | a row with NO functionality, by his ruling |
| `6` | `Add Lung` | alien only; free (4); `[NOT BUILT]` -- and see 4.1: a Lung also comes built in |
| `9` | `Bailout` | a row with NO functionality, by his ruling |
| `0` | `Recycle` | the sim already has `orders::Recycle`; needs a row |

`SPACE` executes and `TAB` closes.

**The heading is NOT adopted, and his name is NEVER displayed** `[RULED]` -- the reference heads its panels
`HARRY PEHKONEN CONSTRUCT`, and PROTEKT will not.

## 7. Consumables and odd items

**The Service Pod** `[RULED]`: a **spinning blue crystal-looking** object that **repairs any vehicle that drives
through it**, restoring **health and ammunition**, and is **used up** when it is. In the tree, `ServicePod` is
kind 20, sits on both recycler lists at a cost of 2 with an authored shape, and has **no behaviour at all**.

Its ammunition half has nothing to act on: PROTEKT's weapons have a **cooldown** (`WeaponSpec::cooldown`, ticks
between shots) and no rounds. The HUD's ship gauge does draw an AMMO arc, and where that number comes from is not
established.

## 8. The art directive

**Everything alien looks unusual** `[RULED]`, in his words: *"please note the unusual looks for everything
alien."* Nothing alien is to read as a human machine in another colour. It covers:

- the three kinds that already have alien geometry -- the Harvester, the Guardian and the Hauler;
- the alien buildings: Kiln, Forge, Antenna Mound, Overseer Array, Dower, Stronghold, Jammer, Power Lung -- whose
  captured looks are the lung (4.2) and the Gun Spire (3.9);
- the alien factory's six vehicles, Sentry through Titan, which have **no geometry of their own at all** today.

The lung's own marks: rotating, crystalline, translucent, one per building and two after an upgrade -- and now
`[CAPTURED]` in full (4.2: a faceted crystal, pale green/cyan, emissive, about a quarter of its structure). The Gun
Spire has a captured silhouette too (3.9).

## 9. What is still open

1. **What do the Training Centre and the Tech Centre unlock?**  Parked by him, so those two rows would ship inert
   or absent. **A published guide now answers both (12.3, `[REFERENCE]`)**: the Tech Centre is the
   advanced-weapons building and the Training Facility unlocks the MAG. It is a guide's answer, not his; he has
   not confirmed it.
2. **Where is an unmet requirement reported?**  PROTEKT dims a row that does not apply; the reference does not dim
   at all -- in the capture where the Kiln an Antenna needs had not been built, the ANTENNA row looked like every
   other row -- and reports at the point of action instead (`ADD N SCRAP`). Does that line NAME a missing building
   too?
3. Defaults he has not contradicted: a claimed scrap pile frees itself when its claimer gives up; more scavengers
   than piles share the nearest; the Walker's combat numbers are the assistant's proposal; the alien kit's masses
   are re-authored only if a smaller hull cut does not settle a Guardian.
4. **Guide-versus-play disagreements.** TWO CLOSED on 2026-11-13, by his choice, the guide winning and his
   earlier reading withdrawn: the **Shadower** is a **Tech Centre** unlock, not the Armory (12.4); the **Jammer**
   needs the **Antenna Mound**, not the Overseer Array (3.3). The **Kiln's roster** is closed too, by his
   self-correction: the Kiln offers only the light three and his captures were the Forge twice (2.4). **Still
   open:** the Gun Spire's Power Lung as a prerequisite (guide only; his words that a Lung is built in bear on it,
   4.1), and which three rows the Kiln offers (unnamed, 2.4).
5. **Owed by him**: whether the un-upgraded Antenna gives the satellite view (3.7, testable in play); the
   enterability of the Dower, Stronghold and Jammer (assumed none, 3.6); whether the Stronghold grants shields
   (13); whether on-foot entry is automatic or a key (14); whether a preset says deployed versus wheeled (15); the
   human and alien Power Lung counts still unset (3.2); the Lung's repair cost and whether a standalone Lung is a
   prerequisite (4.1); the Kiln's three rows (2.4). **No longer owed (answered 2026-11-13):** what a lobbed shell does
   against a footprint (3.8), the force-class counts (15), the `Absorption` spelling (11.6).
6. **Unbuilt, named so nothing is implied to exist** (all `[NOT BUILT]`): the energy pool and its three shield
   sinks, the two Scion forms, the five weapon mechanics (11.4), the four Scion and six ISDF specials, mines, the Power Lungs and the DISABLED
   state (4.1), the Scion `C`-key transition and its two shapes (11.7), the
   refit crates, the Extractor's footprint (3.4), infantry and `Bailout` (14), initial-condition presets (15), and
   every alien building's function (2.6). A MORPH pausing production and the Matriarch's scrap trickle are
   guide-only (11.1). The heat signature and armour type are PARKED (15).

## 10. Provenance

- **His captures** (Battlezone Combat Commander 2.0.185), 2026-11-10: the CONSTRUCT panel; the Builder's build
  menu; the Builder's top menu; the Builder's UPGRADE list with a Kiln and two Extractors; the production
  panel he took for the Kiln's and the Forge's (**both were the Forge's**, 2.4); the Forge and its lungs; the Overseer Array. These fix every roster, cost, key and panel
  idiom marked `[CAPTURED]`.
- **Published guides**: the IGN unofficial Battlezone II guide (structures, requirements and prices), the level
  editor FAQ (the path-point names, which is how the Scion counterparts are known), the Steam buildings guide
  (upgrades, power lungs) and reviews. Used for the roles and the requirement tree marked `[REFERENCE]`.
- **His earlier rulings** already recorded in `docs/REQUIREMENTS.md` §B.5 Power (2026-10-24) and in the faction
  sections.
- **2026-11-13 working notes** (round 19-20 and the Gemini pastes), distilled into 3.4-3.9 and sections 11-16:
  his verbatim rulings on scale, the ghost, the connection rule, enterability, the alien footprints, the alien
  jobs, infantry and the starting conditions are `[RULED]`; four Gemini write-ups he pasted that night (the Scion
  research, the Scion loadouts with the specials and shields, and the ISDF research) are `[REFERENCE]`, a GUIDE
  that his play outranks. His ask: *"Please make sure all this information is captured somewhere permanent."*
- **2026-11-13, later: the "ruled by choice" cards and the Lung capture** (`protekt-plan`: `ruled-by-choice`,
  `the-lung-and-the-two-forms`, with the two Gemini reconciliations): the lob rule, the Kiln's roster, the Tank's
  Special slot, the Shadower and the Jammer closing for the guide, the `Absorption` spelling, the five Lung rules,
  the Lung's and the Gun Spire's captured looks, the `C` key and the force classes. `[RULED]` or `[CAPTURED]`
  as marked.
- **The tree**: `src/game/menu.cpp` (the three shipped lists), `include/protekt/sim/spec.hpp` (costs and build
  times), `include/protekt/game/faction.hpp`, `src/game/row.cpp` (`kind_label`), `src/game/scene.cpp`
  (`append_deploy_preview`, the ship gauge).

## 11. The Scion loadouts and research `[REFERENCE]`

**Source: a Gemini write-up on the Scions that Harri pasted on 2026-11-13, in four pastes (research, loadouts,
specials and shields; the ISDF paste is section 12).** It is a GUIDE, not his play: **where it disagrees with
his play, his play wins and BOTH are recorded** -- except where he has since ruled FOR the guide (11.1's table). Everything in this section is `[REFERENCE]` unless it says
otherwise, and every mechanic named here that the tree does not have is **`[NOT BUILT]`**.

### 11.1 What the guide says, against his own play

**Corroborates him** (independent agreement): the Matriarch's roster (Scavenger, Scout Drone, Guardian, Builder,
Hauler, Healer -- his captured list, name for name); the Scavenger deploys on a pool to become an EXTRACTOR; the
Builder upgrades deployed Extractors (his own report: "the constrictor has to go to the deployed scavenger to do
the upgrade"); **the Stronghold is the Armory**, unlocking shields AND tier-2 weapon upgrades; **the Antenna gives
the satellite view** (a second source for his assumption); **the Jammer is a cloaking tower** hiding nearby
friendlies from enemy radar and breaking missile locks; **the Dower is the repair pad** that heals nearby
vehicles and unlocks the Healer at the Matriarch; Kiln -> Forge and Antenna -> Overseer Array are MORPHS of the
existing building, not new builds; Power Lungs come attached to Scion buildings (his "one lung per building, two
after an upgrade"); **"Entering the Matriarch or Kiln/Forge lets you customize the default loadout for newly built
units"** -- the second independent confirmation of what he told the assistant himself (he changes the loadout FROM
the Matriarch and the Forge).

**DISAGREED with him -- THREE rows, two now CLOSED by his rulings (2026-11-13, asked as choices; they are HIS
rulings, not the assistant's choices), the GUIDE winning and his earlier reading WITHDRAWN:**

| # | the guide | his earlier play-reading | status |
|---|---|---|---|
| 1 | the **Jammer** needs the ANTENNA MOUND | JAMMER <- OVERSEER | **CLOSED `[RULED]`: the guide wins, the Antenna Mound; his reading withdrawn** (3.3) |
| 2 | the **Gun Spire** needs **1 Power Lung** | not confirmed by him | guide only, still open; his words that a Lung is built into any structure that needs one bear on it (4.1) |
| 3 | the **Shadower** is behind the TECH(NICAL) CENTER (12.4) | the ARMORY gives the Scout a Shadower | **CLOSED `[RULED]`: the guide wins, the Tech Centre; his reading withdrawn** (12.4) |

**THE FOURTH PLACE, now CLOSED** (it was flagged and unresolved in M1): the guide said the Forge's roster is
**Lancer, Mauler, Archer, Titan on top of the Kiln's**, and 2.4 had recorded one identical six-row list from his
captures. **The guide was right; his capture of the two panels was the Forge twice** -- his own correction, `[RULED]`
(2.4). The Kiln offers only the light three.

**NEW from the guide, none of it his yet:**
- **THE TWO FORMS**: the Scion combat ships -- **Scout, Sentry, Lancer, Warrior** -- each have a **Combat Mode**
  (high speed, rapid fire, **regenerates ammo**) and an **Assault Mode** (slower or stationary, heavier weapons,
  **regenerates hull**). A per-unit STANCE, generalising what the tree has (the Guardian deploys into a stationary
  turret; the Scavenger into an Extractor). `[NOT BUILT]`; its ammo half is blocked as the Service Bay's is.
- **A morph PAUSES PRODUCTION** while it transforms: a build time on an upgrade, and a risk/reward the guide
  recommends keeping. `[NOT BUILT]`
- **The Matriarch TRICKLES scrap** -- a passive income from the mobile HQ. `[NOT BUILT]`
- **The Healer requires the Dower** (a second source for 3.2's rule).
- **Titan requires Forge + Overseer Array; Archer requires the Overseer Array** -- two new prerequisites, guide
  only.
- **The Jammer BREAKS MISSILE LOCKS** -- a second effect, interacting with the TAG/rocket work.
- **NAMES**: the building's full name is the **ANTENNA MOUND** (the panel prints the short form, which is not the
  entity's name); the Scout is the **SCOUT DRONE**; the **Upgraded Extractor** is a distinct state ("double its
  scrap income" -- his own ruling has the upgrade making the store fill faster, section 5).
- **A whole Scion BUILD ORDER**, which is AI doctrine: section 16.

### 11.2 The hardpoint letters and the slot model

Every hull has slots by CATEGORY: **G Gun, C Cannon, R Rocket, M Mortar, S Special, Sh Shield** -- the shape his own
captured screens showed (`2x Gun (C)`, `1x Rocket (A)`, `1x Special`). The ISDF guide's `(C)`/`(A)` tag is a
different thing, a CLASS OF WEAPON ACCEPTED (section 13). **The loadout model**: an edit is a PRODUCTION
BLUEPRINT, edited at the producer and read at that producer's queue. The Stronghold "(3 key) drops weapon/shield
crates onto the field to refit existing ships": that crate is the **"ARMORY PICKUP"** the tree's own `weapon.hpp`
has been waiting for ("Armory pickups (C) are what will make the sim track [a hull's weapon]"). `[NOT BUILT]`

**A SLOT CAN EXIST AND HOLD NOTHING -- so "no special" is NOT "no slot"** `[RULED]` (2026-11-13, his answer on the
Tank). The Sabre HAS a Special hardpoint (`1S`, the guide) and his screen showed it EMPTY ("no Special") -- both are
true. The loadout model must therefore keep a slot's EXISTENCE and its CONTENTS apart: an empty slot is a slot with
nothing equipped (as his Mortar Bike's `1x Special (NONE)` already printed it), never an absent one. `[NOT BUILT]`

### 11.3 The Scion hardpoint matrix (`[REFERENCE]`, his to correct, nothing built)

| hull | hardpoints | assault mode |
|---|---|---|
| Scout Drone | 1G, 1S, 1Sh | sensor range up, hull regen |
| Sentry | 2G, 1R, 1S, 1Sh | movement locked, fire rate up, hull regen |
| Warrior | 2C, 1R, 1S, 1Sh | heavy slow-hover siege, hull regen |
| Lancer | 2R, 1G, 1S, 1Sh | chassis stabilised, rapid volleys |
| Mauler | 2C, 1M, 1S, 1Sh | (alien heavy walker -- the tree's Walker role) |
| Archer | 1G, 2M, 1Sh | anchored, extreme-range arcing barrages |
| Titan | 2C, 2R, 1M, 1S, 1Sh | near-immovable armoured bunker, fast hull regen |
| Guardian | 1C | deploys permanently into a stationary defence spire |

### 11.4 The weapons, by what unlocks them (`[REFERENCE]`)

- **defaults**: `FANG` (gun), `ION GUN` (cannon);
- **via the STRONGHOLD**: `QUILL`, `GAUSS GUN`, `STINGER`, `STANDARD MORTAR` (+ shields and the refit crates);
- **via the FORGE**: `PLASMA CANNON`, `HEAVY ROCKET (Rave/MLRS)`, `GAS/ACID MORTAR`;
- **via the ANTENNA MOUND**: `ARC STREAM`, `SONIC CANNON`, `MULTILOCK (Wasp)`, `RESIN/GOO MORTAR`.

**Five of those are MECHANICS, not names -- each a slice, not a table row, all `[NOT BUILT]`:**
1. `ARC STREAM` -- a continuous beam that ARCS onto nearby targets itself;
2. `SONIC CANNON` -- a cone that PASSES THROUGH SHIELDS to hit hull, and hits several targets;
3. `GAS/ACID MORTAR` -- a lingering cloud that damages over TIME (a persistent area effect);
4. `RESIN/GOO MORTAR` -- a SLOW debuff on movement AND turn rate;
5. `STINGER` (a swarm) and `MULTILOCK` (a locking pod) -- the same acquisition machinery FAF and TAG need, so
   they share ONE seam rather than three.

### 11.5 The SPECIAL slot: four ACTIVE abilities, all mechanics, none a table row (`[REFERENCE]`, `[NOT BUILT]`)

| special | what it does |
|---|---|
| `BLINK` | teleports the ship FORWARD along its facing -- dodges plasma and rockets, clears ridges, disengages |
| `CLOAK / JAMMER FIELD` | hides the craft from enemy radar and breaks incoming missile locks, draining energy |
| `EMP / SCRAMBLER` | a short-range pulse that DISRUPTS enemy targeting and weapons |
| `PROXIMITY / SEEKER MINES` | drops mines behind the craft to punish pursuers |

`CLOAK` is the unit-sized version of the Jammer building. `PROXIMITY MINES` is the SECOND naming of mines: his own
MSL Scout line said "TAG cannons and Prox mines (proximity)" first, and **no mines exist anywhere in the tree**.
`BLINK` is the largest of the four: a forward teleport interacts with terrain, pathing and the pick, not just
combat.

### 11.6 The SHIELD slot: ENERGY TRADES, not passive bars (`[REFERENCE]`, `[NOT BUILT]`)

| shield | what it does |
|---|---|
| `ABSORBTION` (the reference's own spelling, a misspelling) | converts a percentage of incoming damage back into ammo/energy |
| `DEFLECTION` | reduces incoming projectile damage while DRAINING the weapon energy pool |
| `STASIS` | absorbs a mass of damage briefly, rapidly consuming energy |

**The shape matters more than the names**: every Scion defence either GENERATES energy from being shot
(Absorbtion) or SPENDS it for protection (Deflection, Stasis), and the Scion offence regenerates it in Combat Mode.
So **ENERGY is one resource with a source and three sinks** -- one coherent mechanic, not three.

**ENERGY, NOT AMMO, is the missing Scion subsystem.** Scions carry **NO ammo**: a REGENERATING ENERGY POOL (in
Combat Mode), and shields that trade energy for protection. "Regenerates ammo" in the guide means energy. The
human side is separate ammo pools and passive shield bars (12.5). PROTEKT today has weapon COOLDOWNS and no rounds
and no energy.

**THE SPELLING IS DECIDED `[RULED]`** (2026-11-13, asked as a choice of two): the shield is spelled
**`Absorption`** in PROTEKT, and the reference's `Absorbtion` is a misspelling PROTEKT deliberately does not copy.
The divergence is recorded AS a decision beside the project's other deliberate ones (`TECH CENTRE`, `TRAINING
CTR.`; see **Names** at the top of this file), so a later session does not "fix" it back. (The table above keeps
the reference's own spelling in its heading only because it quotes the reference.)

### 11.7 The `C` key, and two modes that are two SHAPES

His words (2026-11-13): *"The way I can switch between the two modes when I'm on the Scion side is with the C key.
It takes a few seconds, it makes some interesting sound, and then my vehicle is in some different orientation. It
actually looks different. So, the C key toggles the mode."* This adds to the guide's two forms (11.1):

1. the toggle is keyed to **`C`** `[RULED]`;
2. it **TAKES TIME** -- a few seconds -- so it is a TRANSITION, not an instant flip of a flag;
3. it **has its own sound**;
4. it **changes the vehicle's orientation AND its look** ("it actually looks different") -- so the two modes are
   two SHAPES, not one shape with different numbers.

It is the Scion's "asymmetry through morphing" at the unit level, the same idea as the building upgrades (the Kiln
becoming the Forge). The modes' NAMES (Combat / Assault) stay the guide's; their existence, the key and the
transition are his. **The key and the existence of the modes are `[RULED]`; the mechanic -- the timed transition,
the sound, the second shape for each of the four hulls -- is `[NOT BUILT]`.** How long "a few seconds" is, and
whether the unit can act during it, are not given.

## 12. The ISDF loadouts and research `[REFERENCE]`

**Source: a fourth Gemini paste, same night (2026-11-13), on the ISDF.** Same rule: a guide, his play outranks it,
everything unbuilt is `[NOT BUILT]`, the matrix is his to correct.

### 12.1 The per-vehicle matrix (`[REFERENCE]`)

| vehicle | the reference's own name | chassis | hardpoints |
|---|---|---|---|
| Scout | Thunderbolt | fast hover, no armour | 2G (C), 1R (C), 1S |
| Turret | Rattler | hover, deploys to a sentry | 2G (A) |
| Tank | Sabre | medium hover, light | 1C (C), 2G (C), 1M, 1S |
| MSL Scout | Avenger | fast hover, no armour | 2R (C), 1S |
| Mortar Bike | Jackal | fast hover, no armour | 1M, 1S |
| Rocket Tank | Griffin | heavy tracked/armoured | 2R (C), 1R (A) |
| Assault Tank | Bulldog | heavy tracked/armoured | 1C (A), 2M |
| Walker | Atilla | bipedal, heavy (**8000 HP claimed**) | 2C (A), 2G (A) |
| Bomber | Liberator | aerial, light | 1 Bomb Bay (Day Wrecker) |

(Section 9 item 3 already records the Walker's combat numbers as the assistant's proposal, so the "8000 HP" is
the guide's claim, not a number adopted.)

### 12.2 Weapons by unlock (`[REFERENCE]`)

- **Armory**: Chain Gun, Pummel, SP-Stabber, Stinger/Thermal Hornet, MDM, Splinter, Day Wrecker (with the Bomber
  Bay).
- **Tech Centre**: Cold Fusion Laser, Pulse Stabber, Plasma Cannon, Blast Cannon, MAG, TAG Cannon, Shadower, Comet
  Cruise, Pop Gun.
- **Training Facility**: MAG.
- **Defaults**: Mini-Gun, AT-Stabber, FAF Missile, Standard Mortar.

### 12.3 THE TWO PARKED QUESTIONS IT ANSWERS (`[REFERENCE]`, not yet confirmed by him)

- **The TECH CENTRE is the advanced-weapons building**: it unlocks Cold Fusion Laser, Pulse Stabber, Plasma Cannon,
  Blast Cannon, the MAG, the TAG Cannon, the Shadower, the Comet Cruise and the Pop Gun. His own "I ASSUME the tech
  centre is just a requirement for something else" is answered: it is the human parallel to the alien ANTENNA
  MOUND's weapon unlocks (11.4).
- **The TRAINING FACILITY unlocks the MAG** (a chargeable railgun) -- **on top of the APC gate** he told the
  assistant (the notes do not spell that gate out; it is carried as he gave it).

### 12.4 The one disagreement -- CLOSED, the guide wins `[RULED]`

The guide put the `SHADOWER` behind the TECHNICAL CENTER; his earlier play-reading had the ARMORY giving the Scout
a Shadower. **He ruled (2026-11-13, by choice) that the TECH CENTRE unlocks it, not the Armory, and withdrew his
earlier reading.** It is his ruling, not the assistant's choice. (12.2's lists already put it under the Tech
Centre; the Armory's list does not name it.) `[NOT BUILT]` -- no unlock is in the tree.

### 12.5 The six ISDF specials (`[REFERENCE]`, `[NOT BUILT]`) -- the last item he owed

Proximity Mines / **M-Curtain** (a vertical curtain of mines sealing a canyon choke), **Solar Flare** (a decoy that
breaks heat-seeking locks), **Phantom VIR** (radar-jamming stealth), **RED Field** (scrambles enemy targeting,
deflects guided missiles), **SITE Camera** (a remote surveillance pod that sees through terrain within range),
**Thumper** (a seismic shockwave that tosses hovercraft into the air and disrupts their aim). The two sides'
specials are NOT the same set (11.5).

### 12.6 Where the guide matches his own captures (`[REFERENCE]` corroborating `[CAPTURED]`)

- the TANK: guide `1C (C), 2G (C), 1M, 1S` against his screen's "one AT-Stab Cannon, two Miniguns, one Mortar, no
  Special" -- **the guide lists a Special slot (1S) where his screen showed none equipped**; RESOLVED
  `[RULED]` (2026-11-13): the slot EXISTS and was EMPTY, both true -- "no special" is not "no slot" (11.2);
- the ROCKET TANK: `2R (C), 1R (A)` against his `2x Rocket (C): Shadower MSL` + `1x Rocket (A): Salvo RKT`;
- the MSL SCOUT: `2R (C), 1S` against his "TAG cannons and Prox mines (proximity)";
- the MORTAR BIKE: `1M, 1S` against his `MORTAR (C)` + `1x Special (NONE)`;
- `FAF` ("locks onto targets in front of your crosshair") -- his "you point anywhere and it finds enemy assets";
- `MDM` ("detonate the shell mid-air by pressing the fire key a second time") -- his "manually detonated";
- `TAG CANNON` ("a tracking dart followed by guided micro-missiles") -- his own description, word for word;
- the `DAY WRECKER`, the bomber's bomb launched from a Bomber Bay -- his "really big bomb, one building per bomb";
- the CHAIN GUN and the `SP-STABBER` as Armory unlocks -- his own report;
- ISDF has NO shield slots -- the assistant's own reading of the hardpoint screen;
- ammo replenished at SERVICE BAYS, SERVICE TRUCKS or ARMORY AMMO PODS -- his own Service Bay finding, ammo
  included.

## 13. The (C)/(A) rule, and the asymmetries

**THE (C)/(A) RULE** `[REFERENCE]`, resolving a detail the assistant twice recorded as "the mount letter,
unresolved" (`2x Gun (C)` on the Scout, `2x Gun (A)` on the Turret). The guide: *"hardpoints are tagged C for
Combat -- fast, light/medium weapons -- or A for Assault -- heavy, high-drain weapons -- which determines which
guns or cannons fit into a given slot."* So **the letters are the CLASS OF WEAPON A SLOT ACCEPTS**: **C = Combat**
(fast, light/medium), **A = Assault** (heavy, high-drain). It explains his Rocket Tank exactly: `2x Rocket (C)`
taking the Shadower MSL and `1x Rocket (A)` taking the Salvo RKT. (Not to be confused with the Scion's two FORMS,
Combat and Assault Mode, 11.1.)

**THE ASYMMETRIES, all of them his** (the table is the guide's words; the points marked are his):

| | Scion (alien) | ISDF (human) |
|---|---|---|
| armour | low/medium | HIGH |
| shield slot | TRUE | **FALSE -- no shield hardpoints at all** (also his own reading of the hardpoint screen) |
| ammo | a SHARED REGENERATING ENERGY pool | FINITE physical ammo, refillable (Service Bays, Service Trucks, Armory ammo pods) |
| forms | TWO (Combat / Assault) | ONE (a single chassis, with C and A hardpoint slots) |

- **No human JAMMER building** -- his: "There is no Jammer on the human side." Counter-detection is a Scion ability
  alone.
- **No human shield slot**; the Scion Stronghold (the Armory) grants shields where the human Armory does not -- a
  second asymmetry in the same direction, **his to confirm**.
- **A NUANCE** (`[REFERENCE]`): "no Jammer" is true of BUILDINGS. Human VEHICLES can still carry `PHANTOM VIR` (a
  radar-jamming stealth cloak) and the `RED FIELD`, so the asymmetry is building-versus-building, not "the humans
  cannot jam".
- The human side's power (positional, shared) and the Scions' (a lung per building) are the section 4
  asymmetry; the Factory, the Bomber Bay, the Jammer and the Stronghold are the section 1 ownership asymmetry.

## 14. Infantry: the on-foot state as a KIND -- his design

His words (2026-11-13): *"I'm GUESSING that one way to look at the on-foot part is as a new 'vehicle'. True
vehicles could have a flag indicating if there is anybody driving it. If not, and I'm in the 'on-foot vehicle', I
get automatically transferred into the unoccupied vehicle if I get near it, within, say, one metre. What do you
think?"* Status: **design `[RULED]` in outline, `[NOT BUILT]`**; the assistant's answer follows and is `[PROPOSED]`.

**The model.** The soldier is a **KIND** -- a hull you can be in. That is no new subsystem: the tree's rule is that
every ARMED kind can be driven (swapped into), the whole game one hull with an eye in it, so a person is a small
hull and the spec table, the pick, the drawing, the orders and the HUD apply unchanged. It also explains a gap the
project carried for weeks: **`Bailout`** (a verb in the reference's own list, `9`, a row with no functionality
today, 6) cannot exist because there is no hull to BECOME; the soldier kind makes it possible.

**The flag** -- an **OCCUPANCY flag**, "is a pilot in it". The game layer already tracks which unit the player is
in (`driven_unit`), so it is nearly free and gives BOTH directions: on foot + an EMPTY vehicle near -> enter it
(his case); in a vehicle -> **bail out**: the soldier appears beside it and the vehicle goes empty. **Keep it in the
GAME/SESSION layer at first**: if the SIM ever needs it (an unmanned vehicle behaving differently) it becomes
hashed state and a stated re-record; nothing needs that yet.

**NOT AUTOMATIC -- where the assistant differs** (his is automatic transfer within about ONE METRE): **proximity
ENABLES entry and a KEY confirms it**. Automatic means a soldier cannot WALK PAST his own tank -- he is pulled in
every time -- and removes the moment of intent, where the reference makes you enter deliberately; the key costs
nothing and is already the game's grammar: **near it, point at it, SPACE** (the aimed action he chose for the
exterior control panels, 3.6). If it feels fiddly in play, automatic is a ONE-LINE radius change, and he judges by
playing. **Not yet ruled by him**; both are recorded.

**Two consequences to size before the slice:**
1. **A person is much smaller than a unit.** A tile is about four tank lengths, so a soldier is a small fraction of
   a tile: a small **pick box**, no building footprint, and the movement rules for a body that walks.
2. **Enterability needs a "WHO" dimension.** The enterability tables (3.6) already distinguish on foot / by vehicle /
   a station you man; a soldier is what makes "on foot" real, so the two land together. Infantry is in the critical
   path of every exterior control panel and the APC's dismount.

## 15. Initial conditions as data, and the other rulings of the night

**His ask (round 20, 2026-11-13, verbatim):** *"Can we have specced out initial condition? So, for example,
normally, there is no factory. The constructor has to build it. If we had different starting conditions that can be
specified, testing wouldn't drift so much. Even if it's classes, like low, medium, heavy forces, meaning no tanks,
two tank, ten tanks, or something like that. That's a setting for each, the player, and another for the enemy.
Also, the enemy should be able to build scavengers, deploy them, make more vehicles, attack again, defend sanely.
Or even if we build the FOUNDATION for having this kind of ability to spec out the starting conditions, or AI
capability."* He explicitly accepts the FOUNDATION first. `[RULED]` as the ask; the design below is `[PROPOSED]`.

**Thread A -- the initial condition becomes SPECIFIABLE DATA.** His reason, in his words: "testing wouldn't drift so
much". Today a world is staged implicitly (a test writes a scenario file with spawn OFFSETS from a factory's tile
and inherits everything else from the built-in battle), so two tests meaning the same world can drift apart
unnoticed. **The seam already exists -- do not build a second one**: the scenario format (`scenario_toml`, kinds
by name, spawn offsets, the store's opening, the `start_ore` field a slice added for test rigs), and the sides
already swap (`--alien`, scenario DATA, not a second code path). So it is an EXTENSION:
- **a setting PER SIDE** -- what the player starts with and what the enemy starts with, independently;
- **force classes** -- **None / Light / Heavy** `[RULED]` (2026-11-13, asked as a choice; his answer, *"None /
  Light / Heavy -- no vehicles, two, ten"*): **per side**, the count meaning VEHICLES -- none = no vehicles, light =
  two, heavy = ten. This is the shape the starting-conditions slice builds. Underneath it, a list of (kind, how
  many, where, deployed-or-wheeled) stays the proposed data shape;
- **the BASE's shape** -- his case first: "normally, there is no factory; the constructor has to build it", so a
  preset says what is standing and what must be built;
- **the built-in battle becomes ONE NAMED PRESET**, a row in the same table rather than a special case.

**One question still owed by him**: should a preset also say what is DEPLOYED versus wheeled? (The counts being
ABSOLUTE is answered: none / two / ten.) Defaults stand unless he says otherwise. `[NOT BUILT]`

**Thread B -- the enemy plays the game** (his five behaviours: build scavengers, deploy them, make more vehicles,
attack again, defend sanely): see section 16. **Do NOT guess what exists.** The tree has a utility commander with
roles (attack, withdraw at 20 hp, repair, reposition, the mortar bike's scoot), an economy of sorts (scavengers
harvesting, with the spreading rule) and the enemy has producers; which of his five are present, partial or absent
is NOT known without reading the code. **The FIRST slice is a SURVEY**: read `src/ai/` and report, per behaviour,
EXISTS / PARTIAL / ABSENT with file and line evidence, plus what the AI does at the start of the default battle;
nothing changed. The slices follow from the report.

**The order, with its reason:** (1) the connection rule (round 19, small, corrects a rule he has since restated);
(2) the Service Bay's platform (a DEFECT he hit in play, so before a tool); (3) the initial-condition spec
(thread A -- the testing tool, making every later rig explicit); (4) the AI survey (evidence before work); (5) the
AI slices, from the survey. **Anything he sends that is a defect jumps ahead of all of it.**

**The other answers (2026-11-13), `[RULED]`:**
- **The store opens at 40 scrap**: "Let's keep the 40-scrap starting position for now." Unchanged.
- **Heat signature**: "Let's leave the heat signature out for now. We'll come back to that at some point."
  PARKED and named, not dropped -- it belongs with **armour type**, which has no home in the sim either.
- **Still open that he did not address**: the TECH CENTRE's job (parked as "a requirement for something else",
  12.3), the loadouts' SPECIALS (now listed, 11.5 and 12.5, guide-sourced), and the one force-class detail (a preset
  naming what is deployed versus wheeled); the classes themselves are ruled (above).

**The look: "something missing"** (his words: *"There is still something 'missing' from the 'look'. I don't know
what it is. Could it be the atlases? Could we get different ones for the ground?"*). He cannot name it; his guess
is the GROUND, and the tree agrees in one place: there IS a ground atlas with its own generator and tests
(`AtlasGround`, `AtlasSite`, `AtlasGenerator`, `AtlasFiles`, `AtlasLoading`, `AtlasInset`), and a KNOWN FLAT PATCH
is already on the open list: **ground tiles 45, 46 and 47 are flat colour**, carried from an earlier round and never
fixed. The response is a MEASUREMENT, not a guess -- how many tiles the atlas carries, which are flat or
near-flat, which repeat, how much of a typical map each covers -- then SHOW him alternatives as captures, because he
judges a look by looking. `[PROPOSED]` slices: **N1 the ground survey** (counted and measured, the flat tiles
named, a capture with each held up; nothing changed; the deliverable is the finding); **N2 a new ground atlas**
(authored from the survey, offered as captures, replacing the flat tiles at minimum).

## 16. The AI doctrine, from the Scion build order `[REFERENCE]`, `[PROPOSED]` as doctrine

The Gemini Scion research (11.1) gives a whole BUILD ORDER: **deploy immediately; 3-4 Scavengers; loose scrap
first; claim 2-3 pools; a Builder; upgrade the nearest Extractors; Kiln + Stronghold; Dower + Healer; morph to
Forge; Antenna -> Overseer; finish with Titans backed by Archers and Healers.**

**This is AI DOCTRINE as much as a build order**: it is the answer to his own "the enemy should be able to build
scavengers, deploy them, make more vehicles, attack again, defend sanely" (15), mapped onto his five behaviours:

| his behaviour | the build order's step |
|---|---|
| build scavengers | 3-4 Scavengers; loose scrap first |
| deploy them | deploy immediately; claim 2-3 pools; upgrade the nearest Extractors |
| make more vehicles | a Builder; Kiln + Stronghold; Dower + Healer; morph to Forge; Antenna -> Overseer |
| attack again | finish with Titans backed by Archers and Healers |
| defend sanely | not named by the build order -- the commander's existing roles (15) |

**It is testable**: does the enemy commander do these steps, in this order? Note the guide's own gates -- the
Healer needs the Dower, the Titan needs Forge + Overseer Array, the Archer the Overseer Array (11.1) -- and that
**in the tree today the Forge and the Overseer are upgrades in place and no upgrade is built** (2.6), so the steps
from the Forge onward cannot be executed yet. It is the order a Scion player is advised to follow, from a guide,
not a measurement of how his enemy ought to play; the AI survey (15) comes first.
