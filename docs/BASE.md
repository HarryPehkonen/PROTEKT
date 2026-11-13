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
`src/game/menu.cpp`); the rest are captured and not yet in it.

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
alien factory's own list is supplied"* -- and it IS supplied now (2.4), so that comment is out of date.

### 2.4 The Kiln and the Forge -- alien, the same six `[CAPTURED]`

| key | row | cost |
|---|---|---|
| 1 | `Sentry` | 50 |
| 2 | `Lancer` | 55 |
| 3 | `Warrior` | 55 |
| 4 | `Archer` | 65 |
| 5 | `Mauler` | 70 |
| 6 | `Titan` | 90 |

One page, six rows, **identical on both panels**: the Kiln and the Forge it upgrades into offer one production
list between them, so the reference's "early production" against "most of your forces" is not a difference in his
game.

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

**Footprints are PROVISIONAL** (he has given none for the aliens): Kiln 2x2, Gun Spire 1x1, Antenna 1x1, Dower
2x2, Stronghold 2x1 (the one rectangle, so `~` turns it), Jammer 1x1. **Looks** are authored in the Harvester's
idiom (plated organic lofts and blades, no boxes). **`[NOT BUILT]`: every function** -- the Kiln produces nothing,
the Gun Spire does not shoot, the Antenna detects nothing, the Dower services and repairs nothing, the Jammer jams
nothing, the Stronghold constructs no weapons and no shields -- **and the Power Lungs** (one per building,
destructible, disabling, rebuilt free) do not exist.

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

Footprints are `[PROPOSED]` starting points he has accepted; the tree has a per-kind table,
`footprint_of(kind)` (units.hpp): a width and a height. Only the deployed recycler (2x2) has a row today.

### 3.1 Human

| building | row | cost | footprint | prerequisite | what it does | upgrade |
|---|---|---|---|---|---|---|
| Power Generator | `POWER` | 30 | 2x2 | -- | provides **3 units of power** | -- |
| Factory | `FACTORY` | 55 | 4x4 | -- | builds the human eight (2.3) | **none** -- his recollection |
| Relay Bunker | `RELAY BUNKER` | 50 | 2x2 | -- | **extends the human build area** (wireless power) | -- |
| Armory | `ARMORY` | 60 | 3x3 | -- | arms vehicles | -- |
| Service Bay | `SERVICE BAY` | 50 | 3x3 | -- | repairs vehicles | -- |
| Gun Tower | `GUN TOWER` | 50 | 1x1 | -- | base defence; a power CONSUMER | -- |
| Training Centre | `TRAINING CTR.` | 70 | 3x3 | -- | **UNKNOWN -- parked by him** | -- |
| Tech Centre | `TECH CENTRE` | 80 | 3x3 | -- | **UNKNOWN -- parked by him** | -- |
| Bomber Bay | `BOMBER BAY` | 100 | 4x4 | -- | houses the bomber, which never leaves it on its own | -- |

### 3.2 Alien

Lung counts are `[RULED]` where they are given: a building carries **one** lung and its upgrade carries **two**.
Where a count is not given it is `?` and is his to set.

| building | row | cost | footprint | prerequisite | what it does | upgrade | lungs |
|---|---|---|---|---|---|---|---|
| Power Lung | -- | 0 | 2x2 | -- | a building's own power, attached to it | -- | the lung itself |
| Kiln | `KILN` | 60 | 3x3 | a built Power Lung `[REFERENCE]` | early production; the six (2.4) | `-> FORGE`, 60 scrap | 1 |
| Forge | -- | 120 as a structure; 60 to upgrade a Kiln | 3x3 | the Kiln upgraded | production; the six (2.4) | -- | 2 |
| Antenna Mound | `ANTENNA` | 60 | 3x3 | a built Kiln `[RULED]` | the relay/comms role | `-> OVERSEER ARRAY`, 80 scrap, **needs a Forge** | 1 |
| Overseer Array | -- | -- | 3x3 | the Antenna upgraded | -- | -- | 2 |
| Dower | `DOWER` | 50 | 3x3 | a built Kiln `[RULED]` | services and repairs vehicles; **needed for Healers** | -- | ? |
| Stronghold | `STRONGHOLD` | 70 | 3x3 | a Forge `[RULED]` | the alien **Armory**, and it also grants shields | -- | ? |
| Jammer | `JAMMER` | 50 | 2x2 | an Overseer Array `[RULED]` | blinds the enemy's radar and degrades their targeting nearby | -- | ? |
| Gun Spire | `GUN SPIRE` | 75 | 1x1 | a Power Lung `[REFERENCE]` | the alien gun tower | -- | ? |

### 3.3 The prerequisite tree, as it stands

Confirmed in his own play: the **ANTENNA** and the **DOWER** each need a built **KILN**; the **STRONGHOLD** needs a
**FORGE**; the **JAMMER** needs an **OVERSEER**; the **ANTENNA'S UPGRADE** needs a Forge. From the reference,
unconfirmed: the Kiln and the Gun Spire need a Power Lung; the Forge needs the Kiln and two Power Lungs; the
Overseer Array needs a Forge, an Antenna and two Power Lungs.

"Needs a Kiln" should read **"a Kiln or a Forge"**, since a Forge IS the upgraded Kiln: an upgrade must not take
a prerequisite away.

## 4. Power

**The sides are asymmetric, by his ruling.**

**Human: shared, and positional.**  A Power Generator grants **3 units** of power; consumers draw **different**
amounts; **the number on the screen is the SURPLUS**, not a currency. Scrap buys a generator and nothing else.
And the human build area is **ADJACENT to the deployed recycler or to a Relay Bunker** -- wireless power -- so the
Relay Bunker's job is to EXTEND where you may build. This rule is human-only, in his words: *"That's only for the
human side."*

**Alien: one lung per building, and a lung is a target.**  Each alien building carries its own Power Lung, so no
adjacency rule applies and losing a lung disables ONE building rather than the grid. A lung **looks like a
rotating, crystalline, translucent bulb about a metre tall** `[RULED]`, and its size is provisional and his to
change. Lung counts: Kiln 1, Forge 2, Antenna Mound 1, Overseer Array 2 `[RULED]`; the rest unset.
**Destroying a lung DISABLES the building**; the **Builder** rebuilds it with **`6 Add Lung`**, which is **free**
`[RULED]`.

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
| `6` | `Add Lung` | alien only; `[NOT BUILT]` |
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
  only captured look so far is the lung itself;
- the alien factory's six vehicles, Sentry through Titan, which have **no geometry of their own at all** today.

The lung's own marks: rotating, crystalline, translucent, about a metre tall, one per building and two after an
upgrade.

## 9. What is still open

1. **What do the Training Centre and the Tech Centre unlock?**  Parked by him, so those two rows would ship inert
   or absent.
2. **Where is an unmet requirement reported?**  PROTEKT dims a row that does not apply; the reference does not dim
   at all -- in the capture where the Kiln an Antenna needs had not been built, the ANTENNA row looked like every
   other row -- and reports at the point of action instead (`ADD N SCRAP`). Does that line NAME a missing building
   too?
3. Defaults he has not contradicted: a claimed scrap pile frees itself when its claimer gives up; more scavengers
   than piles share the nearest; the Walker's combat numbers are the assistant's proposal; the alien kit's masses
   are re-authored only if a smaller hull cut does not settle a Guardian.

## 10. Provenance

- **His captures** (Battlezone Combat Commander 2.0.185), 2026-11-10: the CONSTRUCT panel; the Builder's build
  menu; the Builder's top menu; the Builder's UPGRADE list with a Kiln and two Extractors; the Kiln's production
  panel; the Forge and its lungs; the Overseer Array. These fix every roster, cost, key and panel idiom marked
  `[CAPTURED]`.
- **Published guides**: the IGN unofficial Battlezone II guide (structures, requirements and prices), the level
  editor FAQ (the path-point names, which is how the Scion counterparts are known), the Steam buildings guide
  (upgrades, power lungs) and reviews. Used for the roles and the requirement tree marked `[REFERENCE]`.
- **His earlier rulings** already recorded in `docs/REQUIREMENTS.md` §B.5 Power (2026-10-24) and in the faction
  sections.
- **The tree**: `src/game/menu.cpp` (the three shipped lists), `include/protekt/sim/spec.hpp` (costs and build
  times), `include/protekt/game/faction.hpp`, `src/game/row.cpp` (`kind_label`), `src/game/scene.cpp`
  (`append_deploy_preview`, the ship gauge).
