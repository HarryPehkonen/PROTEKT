#include "protekt/game/menu.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <utility>

#include "protekt/game/row.hpp"
#include "protekt/sim/spec.hpp"

namespace protekt::game {
namespace {

/// A row of a selection's menu that is the same every time it is drawn: a key, a verb and whether
/// the verb applies to the selection at all. The state row and the build rows are NOT here -- they
/// are recomputed from `MenuInput` -- and those two are the whole reason this is a function.
struct FixedRow {
    std::string_view key;
    std::string_view verb;
    MenuDim dim;
};

/// The key column's `~`, written once; `SPACE` and `TAB` are shared with the nav chain (menu.hpp).
constexpr std::string_view kTilde{"~"};

/// A recycler / matriarch that has not been deployed: it can be told where to go and where to
/// deploy there, and the turret-rotation verb is drawn but does not apply until it has buildings
/// to rotate.
constexpr std::array kRecyclerRows{
    FixedRow{kTilde, kVerbRotateBuildings, MenuDim::DoesNotApply},
    FixedRow{"1", kVerbFollowMe, MenuDim::None},
    FixedRow{"2", kVerbGoToNav, MenuDim::None},
    FixedRow{"3", kVerbDeploy, MenuDim::None},
};

/// A harvester / scavenger. Note what is NOT here: 6, 7 and 8 are missing on purpose and the gap is
/// drawn, because the reference shows a visible one -- keys are a pair of {key, verb}, never a
/// position in a list, and the free keys are left free for whatever fills them later. `~ SELECT
/// UNIT` is NOT here because its ink depends on the selection size (`select_unit_row`, drawn first
/// by `menu_rows`); `5 Pick Me Up` is dimmed because it does not apply to this selection, its ink
/// the same as a build row that is merely too expensive, and only `MenuDim` tells them apart.
constexpr std::array kHarvesterRows{
    FixedRow{"1", kVerbFollowMe, MenuDim::None},
    FixedRow{"2", kVerbGoToNav, MenuDim::None},
    FixedRow{"3", kVerbScavenge, MenuDim::None},
    FixedRow{"4", kVerbHold, MenuDim::None},
    FixedRow{"5", kVerbPickMeUp, MenuDim::DoesNotApply},
    // 6, 7, 8: the gap.
    FixedRow{"9", kVerbBailout, MenuDim::None},
    FixedRow{"0", kVerbRecycle, MenuDim::None},
};

/// `~ Select Unit`, the one row whose ink is the selection's size: it narrows a group to one of its
/// units, so a lone unit has nothing to select within itself and the row is dim for it. Every
/// subject that draws the row draws it through here, so the rule has one home.
[[nodiscard]] MenuRow select_unit_row(const MenuInput& in) {
    return MenuRow{kTilde, std::string{kVerbSelectUnit}, in.group,
                   in.group ? MenuDim::None : MenuDim::DoesNotApply};
}

/// A selected VEHICLE -- a tank, hover tank, mortar bike, rocket tank or scout. The same ten rows
/// as the reference's tank and scout captures (2026-10-26); only the state row's default verb
/// differs between them, and the model leaves that to the caller. `~ Select Unit` is NOT here
/// because its ink depends on the selection size: `menu_rows` draws it first, dimmed for a lone
/// unit.
constexpr std::array kVehicleRows{
    FixedRow{"1", kVerbFollowMe, MenuDim::None},
    FixedRow{"2", kVerbGoToNav, MenuDim::None},
    FixedRow{"3", kVerbAttack, MenuDim::None},
    FixedRow{"4", kVerbHold, MenuDim::None},
    FixedRow{"5", kVerbPickMeUp, MenuDim::DoesNotApply},
    // 6, 7, 8: the gap.
    FixedRow{"9", kVerbBailout, MenuDim::None},
    FixedRow{"0", kVerbRecycle, MenuDim::None},
};

/// A SERVICE TRUCK (AC): the vehicle's rows with `3` turned from `Attack` to `Repair`. A truck is
/// unarmed, so Attack was never its verb; Repair is the job it exists for, and choosing it lists
/// the wounded. `~ Select Unit` is drawn first by `menu_rows`, as for a vehicle.
constexpr std::array kServiceTruckRows{
    FixedRow{"1", kVerbFollowMe, MenuDim::None},
    FixedRow{"2", kVerbGoToNav, MenuDim::None},
    FixedRow{"3", kVerbRepair, MenuDim::None},
    FixedRow{"4", kVerbHold, MenuDim::None},
    FixedRow{"5", kVerbPickMeUp, MenuDim::DoesNotApply},
    // 6, 7, 8: the gap.
    FixedRow{"9", kVerbBailout, MenuDim::None},
    FixedRow{"0", kVerbRecycle, MenuDim::None},
};

/// The key `7 Undeploy` sits on; named so the row's key is a view into a static, not a literal the
/// compiler may take for a temporary string (see `MenuRow`'s deleted constructor).
constexpr std::string_view kUndeployKey{"7"};

/// A TURRET, plain or deployed: the vehicle's ten rows, the same keys on the same verbs, with the
/// two that are a turret's own in the vehicle table's gap -- `6 Defend` (drive to within range of
/// an enemy, deploy there, shoot) and `7 Undeploy`. `~ Select Unit` is drawn first by `menu_rows`,
/// as for a vehicle, and `7`'s ink depends on the selection (a turret that is not deployed has
/// nothing to take down), so it too is drawn by `menu_rows` and is not in this table. `SPACE
/// Deploy` is not here either: it is the state row's, and only in its context.
constexpr std::array kTurretHeadRows{
    FixedRow{"1", kVerbFollowMe, MenuDim::None},         FixedRow{"2", kVerbGoToNav, MenuDim::None},
    FixedRow{"3", kVerbAttack, MenuDim::None},           FixedRow{"4", kVerbHold, MenuDim::None},
    FixedRow{"5", kVerbPickMeUp, MenuDim::DoesNotApply}, FixedRow{"6", kVerbDefend, MenuDim::None},
};
constexpr std::array kTurretTailRows{
    FixedRow{"9", kVerbBailout, MenuDim::None},
    FixedRow{"0", kVerbRecycle, MenuDim::None},
};

/// A DEPLOYED recycler, above its build list. `~ DROP OFF` is drawn as the reference draws it and
/// means nothing to us yet: the string is rendered, no behaviour is invented behind it.
constexpr std::array kDeploymentRows{
    FixedRow{kTilde, kVerbDropOff, MenuDim::None},
};

/// A build row that names a unit kind: its cost is the SIM's price for that kind
/// (`UnitSpec::cost`), read from the one table, so the number drawn on the row is the number the
/// queue charges.
[[nodiscard]] constexpr BuildItem kinded(std::string_view key, std::string_view label,
                                         sim::UnitKind kind) {
    return BuildItem{key, label, sim::spec_of(kind).cost, kind};
}

/// The HUMAN recycler's build list, in key order: Harri's own list, verbatim. Every row names a
/// kind and takes its cost from the spec table, so the number drawn is the number the queue
/// charges. The Scavenger is the SAME `UnitKind` the alien list calls a Harvester (Harri's ruling:
/// the faction names the machine, the sim keeps one kind).
constexpr std::array kHumanBuildList{
    kinded("1", "Scavenger", sim::UnitKind::Scavenger),
    kinded("2", "Scout", sim::UnitKind::Scout),
    // The turret is a vehicle until it deploys, so row 3 queues the kind that already exists.
    kinded("3", "Turret", sim::UnitKind::Turret),
    kinded("4", "Tug", sim::UnitKind::Tug),
    kinded("5", "Constructor", sim::UnitKind::Constructor),
    kinded("6", "Service Truck", sim::UnitKind::ServiceTruck),
    kinded("7", "Service Pod", sim::UnitKind::ServicePod),
};

/// The ALIEN recycler's build list, in key order: the names and costs the reference capture showed
/// (it was the code's only list until factions existed, and was mislabelled as the player's). Its
/// row 1 is the SAME `UnitKind` the human list calls a Scavenger.
constexpr std::array kAlienBuildList{
    kinded("1", "Harvester", sim::UnitKind::Scavenger),
    // The alien Scout is documented at 45 against the human 50, but the sim charges one price per
    // kind and a row must show what the queue charges, so this row reads the kind's own price.
    // OWED by Harri: whether the alien scout is a kind of its own.
    kinded("2", "Scout", sim::UnitKind::Scout),
    // A NAME row, like the human Turret: the structure-placement feature it needs does not exist.
    BuildItem{"3", "Guardian", 40, std::nullopt},
    BuildItem{"4", "Hauler", 30, std::nullopt},
    BuildItem{"5", "Builder", 40, std::nullopt},
    BuildItem{"6", "Healer", 50, std::nullopt},
    BuildItem{"7", "Service Pod", 2, std::nullopt},
};

/// The FACTORY's build list: Harri's own factory menu, eight rows in key order (he said "just
/// propose something ... you build, I play, then we correct", 2026-10-30). The labels and the costs
/// are his menu screenshot's; the cost is read from the spec table, so the number drawn is the
/// number the queue charges. His `MSL Scout` is the missile scout, a kind of its own, and NOT the
/// recycler's `Scout`.
///
/// It belongs to the HUMAN faction. The ALIEN factory draws this same list for now: the alien
/// factory's cost list and the alien NAMES for these eight vehicles are both OWED by Harri (the
/// only alien names known are for the recycler's roles and the guardian), so the costs and the
/// names here are PROVISIONAL for the aliens until he supplies them.
constexpr std::array kFactoryBuildList{
    kinded("1", "Mortar Bike", sim::UnitKind::MortarBike),
    kinded("2", "MSL Scout", sim::UnitKind::MslScout),
    kinded("3", "Tank", sim::UnitKind::Tank),
    kinded("4", "Rocket Tank", sim::UnitKind::RocketTank),
    kinded("5", "Assault Tank", sim::UnitKind::AssaultTank),
    kinded("6", "Walker", sim::UnitKind::Walker),
    kinded("7", "APC", sim::UnitKind::Apc),
    kinded("8", "Bomber", sim::UnitKind::Bomber),
};

/// The CONSTRUCTOR's nine buildings, in key order. Every cost is CONFIRMED off the owner's own
/// captures (2026-11-11). NAME rows: no `UnitKind` names a structure yet, so no row can place
/// anything (slice J3 brings the first).
///
/// TWO LABELS DIVERGE FROM THE REFERENCE ON PURPOSE -- the owner ruled Canadian spelling, so it
/// reads `TECH CENTRE` where the reference prints "Tech Center"; and `TRAINING CTR.` where the
/// reference prints `TRAINING FAC.`, his own abbreviation ("Training Centre can be abbreviated to
/// Training Ctr."), because the full name does not fit the panel. Do not "correct" either.
constexpr std::array kConstructItems{
    kinded("1", "POWER", sim::UnitKind::Power), // the first building (J3): its cost is the spec's
    BuildItem{"2", "FACTORY", 55, std::nullopt},
    BuildItem{"3", "RELAY BUNKER", 50, std::nullopt},
    BuildItem{"4", "ARMORY", 60, std::nullopt},
    BuildItem{"5", "SERVICE BAY", 50, std::nullopt},
    BuildItem{"6", "GUN TOWER", 50, std::nullopt},
    BuildItem{"7", "TRAINING CTR.", 70, std::nullopt},
    BuildItem{"8", "TECH CENTRE", 80, std::nullopt},
    BuildItem{"9", "BOMBER BAY", 100, std::nullopt},
};

/// The ALIEN BUILDER's six buildings (round 17, J5), off the owner's own capture. No second page.
/// Name rows: no alien building has a `UnitKind`, so nothing here can be placed yet.
constexpr std::array kAlienConstructItems{
    BuildItem{"1", "KILN", 60, std::nullopt},       BuildItem{"2", "GUN SPIRE", 75, std::nullopt},
    BuildItem{"3", "ANTENNA", 60, std::nullopt},    BuildItem{"4", "DOWER", 50, std::nullopt},
    BuildItem{"5", "STRONGHOLD", 70, std::nullopt}, BuildItem{"6", "JAMMER", 50, std::nullopt},
};
static_assert(kAlienConstructItems.size() == kAlienConstructRows);

/// A CONSTRUCTOR (round 17, J1): the vehicle's rows with `3` turned from `Attack` to `Construct`,
/// which opens the CONSTRUCT panel. The reference's row reads `Build`; ours reads `Construct`, the
/// panel's own heading. `~ Select Unit` is drawn first by `menu_rows`, as for a vehicle.
constexpr std::array kConstructorRows{
    FixedRow{"1", kVerbFollowMe, MenuDim::None},
    FixedRow{"2", kVerbGoToNav, MenuDim::None},
    FixedRow{"3", kVerbConstruct, MenuDim::None},
    FixedRow{"4", kVerbHold, MenuDim::None},
    FixedRow{"5", kVerbPickMeUp, MenuDim::DoesNotApply},
    // 6, 7, 8: the gap.
    FixedRow{"9", kVerbBailout, MenuDim::None},
    FixedRow{"0", kVerbRecycle, MenuDim::None},
};

/// The item's name and the cost the row shows, as one string: the cost is drawn ON the build row,
/// so it rides in the verb and there is no second place for the number to drift to.
[[nodiscard]] std::string named_cost(std::string_view label, int cost) {
    return std::string{label} + " " + std::to_string(cost);
}

/// What the top row reads: the armed command if there is one, otherwise the selection SPACE would
/// make. Empty is a real answer -- nothing is armed and nothing selectable is under the cursor, so
/// the row names no verb and shows its key alone.
[[nodiscard]] std::string state_verb(const MenuInput& in) {
    switch (in.armed.command) {
    case MenuCommand::Deploy:
        return std::string{kVerbDeploy};
    case MenuCommand::Build:
        return std::string{in.armed.item};
    case MenuCommand::None:
        break;
    }
    if (in.attack) {
        return std::string{kVerbAttack};
    }
    if (in.repair) {
        return std::string{kVerbRepair};
    }
    if (in.deploy) {
        return std::string{kVerbDeploy};
    }
    if (in.go_here) {
        return std::string{kVerbGoHere};
    }
    return in.target == MenuTarget::Selectable ? std::string{kVerbSelect} : std::string{};
}

/// The row everything else hangs under: always first, always live, never dimmed. An armed command
/// reads its distance at the right, when there is one to read.
[[nodiscard]] MenuRow state_row(const MenuInput& in) {
    MenuRow row{kRowKeySpace, state_verb(in), true, MenuDim::None};
    if (in.armed.command != MenuCommand::None && in.metres.has_value()) {
        row.right = metres_text(*in.metres);
    }
    return row;
}

void append_fixed(std::vector<MenuRow>& rows, std::span<const FixedRow> fixed) {
    for (const FixedRow& row : fixed) {
        rows.push_back(MenuRow{row.key, std::string{row.verb}, row.dim == MenuDim::None, row.dim});
    }
}

/// The build list, with each row weighed against what is banked. An unaffordable row is dimmed for
/// `MenuDim::Unaffordable` -- not `DoesNotApply`: the item is buildable, it is simply not paid for.
void append_priced_rows(std::vector<MenuRow>& rows, const MenuInput& in,
                        std::span<const BuildItem> items) {
    for (const BuildItem& item : items) {
        const bool affordable = in.ore >= item.cost;
        rows.push_back(MenuRow{item.key, named_cost(item.label, item.cost), affordable,
                               affordable ? MenuDim::None : MenuDim::Unaffordable});
        rows.back().cost = item.cost;
        rows.back().banked = in.ore;
    }
}

void append_build_list(std::vector<MenuRow>& rows, const MenuInput& in) {
    append_priced_rows(rows, in, build_items(in.faction, in.subject));
}

/// THE CONSTRUCT PANEL: `TAB Close Menu`, `~ Rotate Buildings` (greyed until a building is armed),
/// the `CONSTRUCT` heading, then the nine keyed rows -- twelve in all. Each keyed row is weighed
/// against the bank by the SAME predicate every build row uses, and `Interaction` refuses a press
/// by reading the row this returns, so the dim and the refusal cannot disagree.
void append_construct_panel(std::vector<MenuRow>& rows, const MenuInput& in) {
    rows.push_back(MenuRow{kRowKeyTab, std::string{kVerbCloseMenu}, true, MenuDim::None});
    // `~ Rotate Buildings` is live while a building is armed (it turns the footprint a quarter) and
    // greyed otherwise, as it always was.
    rows.push_back(
        in.placing
            ? MenuRow{kTilde, std::string{kVerbRotateBuildings}, true, MenuDim::None}
            : MenuRow{kTilde, std::string{kVerbRotateBuildings}, false, MenuDim::DoesNotApply});
    MenuRow heading{std::string_view{}, std::string{kConstructHeading}, true, MenuDim::None};
    heading.heading = true;
    rows.push_back(std::move(heading));
    // THE ONE PREDICATE for "can this panel row be built": the requirement set (buildings, then
    // power) and then the bank. The dim, the press and the refusal all read the row this makes.
    const std::span<const BuildItem> items = construct_items(in.faction);
    for (std::size_t i = 0; i < items.size(); ++i) {
        const BuildItem& item = items[i];
        const ConstructRequirement need = construct_requirement(in.faction, i);
        std::uint16_t missing = 0;
        for (std::size_t b = 0; b < kConstructBuildingCount; ++b) {
            if (((need.buildings >> b) & 1U) != 0U && in.standing.at(b) <= 0) {
                missing = static_cast<std::uint16_t>(missing | (1U << b));
            }
        }
        // Precedence: what waiting cannot fix is named before the scrap that is merely short.
        MenuDim reason = MenuDim::None;
        if (missing != 0U) {
            reason = MenuDim::NeedsBuilding;
        } else if (need.power && spare_power(in.standing) < 1) {
            reason = MenuDim::NeedsPower;
        } else if (in.ore < item.cost) {
            reason = MenuDim::Unaffordable;
        }
        rows.push_back(
            MenuRow{item.key, named_cost(item.label, item.cost), reason == MenuDim::None, reason});
        rows.back().cost = item.cost;
        rows.back().banked = in.ore;
        rows.back().missing = missing;
        rows.back().faction = in.faction;
    }
}

/// The keys a list row is pressed with, by position in the list: 1-9, then 0 for the tenth.
/// Statics, so a row's key view outlives the row (see `MenuRow`'s deleted constructor).
constexpr std::array<std::string_view, kAttackListRows> kListKeys{"1", "2", "3", "4", "5",
                                                                  "6", "7", "8", "9", "0"};

/// A list of units as rows: each names its asset by the player's faction (they are the player's
/// own), reads its distance in the right column when `with_distance`, and carries the unit it is
/// about. The repair list and the function key's unit list are drawn by this one function.
void append_unit_rows(std::vector<MenuRow>& rows, const MenuInput& in,
                      const std::vector<TargetRow>& list, bool with_distance, bool leads) {
    rows.reserve(rows.size() + list.size());
    for (const TargetRow& entry : list) {
        // `TargetRow::key` is the digit (1-9, 0 for the tenth); its position is the digit less one.
        MenuRow row{kListKeys.at(static_cast<std::size_t>((entry.key + kAttackListRows - 1)
                                                          % kAttackListRows)),
                    std::string{kind_label(entry.kind, in.faction)}, true, MenuDim::None};
        if (with_distance) {
            row.right = metres_text(entry.metres);
        }
        row.unit = entry.id;
        row.leads = leads;
        rows.push_back(std::move(row));
    }
}

/// The nav list as rows: each is named `NAV`, keyed by its mark's slot digit (the key the list
/// carries is already that digit, 1-9 then 0) and reads its distance. A mark names no unit, so no
/// row carries one and the drawer gives it no health or ammunition lines.
void append_nav_rows(std::vector<MenuRow>& rows, const std::vector<NavRow>& list) {
    rows.reserve(rows.size() + list.size());
    for (const NavRow& entry : list) {
        MenuRow row{kListKeys.at(static_cast<std::size_t>((entry.key + kAttackListRows - 1)
                                                          % kAttackListRows)),
                    std::string{kVerbNav}, true, MenuDim::None};
        row.right = metres_text(entry.metres);
        rows.push_back(std::move(row));
    }
}

} // namespace

namespace {
/// "a Factory", "an Armory": the building as a refusal names it (the panel's own labels, in words).
constexpr std::array<std::string_view, kConstructBuildingCount> kBuildingNames{
    "a Power",     "a Factory",       "a Relay Bunker", "an Armory",    "a Service Bay",
    "a Gun Tower", "a Training Ctr.", "a Tech Centre",  "a Bomber Bay",
};

/// The alien names, by `AlienBuilding` slot; the slots past the eighth are unused.
constexpr std::array<std::string_view, kConstructBuildingCount> kAlienBuildingNames{
    "a Kiln",   "a Gun Spire", "an Antenna",  "a Dower", "a Stronghold",
    "a Jammer", "a Forge",     "an Overseer", "",
};

/// "Needs a Factory" / "Needs an Armory and a Service Bay": EVERY missing building, in panel order.
std::string needs_buildings_text(std::uint16_t missing, Faction faction) {
    const auto& names = faction == Faction::Alien ? kAlienBuildingNames : kBuildingNames;
    std::string text;
    for (std::size_t b = 0; b < kConstructBuildingCount; ++b) {
        if (((missing >> b) & 1U) != 0U) {
            text += text.empty() ? "Needs " : " and ";
            text += names.at(b);
        }
    }
    return text.empty() ? std::string{"Needs another building first"} : text;
}
} // namespace

std::string refusal_message(const MenuRow& row) {
    // No `default:`: a new MenuDim that is not mapped here is a -Wswitch error, not silence.
    switch (row.reason) {
    case MenuDim::None:
    case MenuDim::DoesNotApply:
        return {}; // not a build refusal: a live row, or an action row with its own wording
    case MenuDim::Unaffordable:
        return "Add " + std::to_string(row.cost - row.banked) + " scrap (have "
               + std::to_string(row.banked) + ", costs " + std::to_string(row.cost) + ")";
    case MenuDim::NeedsBuilding:
        return needs_buildings_text(row.missing, row.faction);
    case MenuDim::NeedsPower:
        return "Needs power";
    case MenuDim::NeedsUpgrade: // unreachable today
        return "Needs an upgrade first";
    }
    return {};
}

int spare_power(const StandingBuildings& standing) noexcept {
    const auto count
        = [&standing](ConstructBuilding b) { return standing.at(static_cast<std::size_t>(b)); };
    return kPowerSuppliedPerPlant * count(ConstructBuilding::Power)
           - (count(ConstructBuilding::Factory) + count(ConstructBuilding::RelayBunker)
              + count(ConstructBuilding::GunTower));
}

StandingBuildings standing_buildings(const sim::Sim& sim, sim::Side side, Faction faction) {
    StandingBuildings standing{};
    if (faction == Faction::Alien) {
        return standing; // no alien building has a kind yet: nothing stands
    }
    for (const sim::Unit& unit : sim.units()) {
        if (!unit.alive || unit.side != side) {
            continue;
        }
        // Only the kinds that exist: the other seven buildings have no `UnitKind` yet.
        if (unit.kind == sim::UnitKind::Power) {
            ++standing.at(static_cast<std::size_t>(ConstructBuilding::Power));
        } else if (unit.kind == sim::UnitKind::Factory) {
            ++standing.at(static_cast<std::size_t>(ConstructBuilding::Factory));
        }
    }
    return standing;
}

namespace {
/// THE ALIEN TREE, off the owner's own play: the gate is a BUILDING, never power. ANTENNA and DOWER
/// need a Kiln, JAMMER an Overseer (the Antenna upgraded in place), STRONGHOLD a Forge (the Kiln
/// upgraded in place). The reference's further rules (a Kiln or Gun Spire needing a Power Lung, a
/// Forge needing the Kiln and two Power Lungs) are NOT confirmed and are NOT implemented.
ConstructRequirement alien_requirement(std::size_t row) noexcept {
    const auto bit = [](AlienBuilding b) {
        return static_cast<std::uint16_t>(1U << static_cast<unsigned>(b));
    };
    using A = AlienBuilding;
    switch (row) {
    case 2: // ANTENNA
    case 3: // DOWER
        return {bit(A::Kiln), false};
    case 4: // STRONGHOLD
        return {bit(A::Forge), false};
    case 5: // JAMMER
        return {bit(A::Overseer), false};
    default: // KILN, GUN SPIRE
        return {};
    }
}
} // namespace

ConstructRequirement construct_requirement(Faction faction, std::size_t row) noexcept {
    if (faction == Faction::Alien) {
        return alien_requirement(row);
    }
    const auto bit = [](ConstructBuilding b) {
        return static_cast<std::uint16_t>(1U << static_cast<unsigned>(b));
    };
    using B = ConstructBuilding;
    // THE OWNER'S TREE, in row order (see the plan, round 17).
    switch (row) {
    case 1: // FACTORY
    case 2: // RELAY BUNKER
    case 5: // GUN TOWER
        return {0, true};
    case 3: // ARMORY
    case 4: // SERVICE BAY
        return {bit(B::Factory), false};
    case 6: // TRAINING CTR.
        return {bit(B::Armory), false};
    case 7: // TECH CENTRE
        return {static_cast<std::uint16_t>(bit(B::Armory) | bit(B::ServiceBay)), false};
    case 8: // BOMBER BAY
        return {bit(B::TrainingCentre), false};
    default: // POWER: scrap only
        return {};
    }
}

std::span<const BuildItem> construct_items(Faction faction) noexcept {
    return faction == Faction::Alien ? std::span<const BuildItem>{kAlienConstructItems}
                                     : std::span<const BuildItem>{kConstructItems};
}

std::string metres_text(int metres) { return std::to_string(metres) + "m"; }

std::span<const BuildItem> build_items(Faction faction, MenuSubject subject) noexcept {
    switch (subject) {
    case MenuSubject::Deployment:
        return faction == Faction::Human ? std::span<const BuildItem>{kHumanBuildList}
                                         : std::span<const BuildItem>{kAlienBuildList};
    case MenuSubject::Factory:
        return kFactoryBuildList; // both factions, until the alien factory's own list is supplied
    case MenuSubject::None:
    case MenuSubject::Recycler:
    case MenuSubject::Harvester:
    case MenuSubject::Turret:
    case MenuSubject::Vehicle:
    case MenuSubject::ServiceTruck:
    case MenuSubject::Constructor:
        break;
    }
    return {};
}

MenuSubject menu_subject_of_kind(sim::UnitKind kind) noexcept {
    switch (kind) {
    case sim::UnitKind::Factory: // `top menu -> 2`: the factory's own menu
        return MenuSubject::Factory;
    case sim::UnitKind::DeployedRecycler: // `top menu -> 1`: the deployed recycler's own menu
        return MenuSubject::Deployment;
    case sim::UnitKind::Recycler:
        return MenuSubject::Recycler;
    case sim::UnitKind::Scavenger:
        return MenuSubject::Harvester;
    case sim::UnitKind::DeployedScavenger: // no key selects it, and it takes no command: no menu
    case sim::UnitKind::Power:             // a placed building: nothing selects it yet, no menu
        return MenuSubject::None;
    case sim::UnitKind::Turret:
    case sim::UnitKind::DeployedTurret:
        return MenuSubject::Turret;
    case sim::UnitKind::Tank:
    case sim::UnitKind::HoverTank:
    case sim::UnitKind::MortarBike:
    case sim::UnitKind::RocketTank:
    case sim::UnitKind::Scout: // the reference's tank and scout captures share ONE table
    case sim::UnitKind::AssaultTank:
    case sim::UnitKind::Walker:
    case sim::UnitKind::Apc:
    case sim::UnitKind::Bomber:
    case sim::UnitKind::MslScout:
    case sim::UnitKind::Tug:
    case sim::UnitKind::ServicePod:
        return MenuSubject::Vehicle;
    case sim::UnitKind::Constructor: // round 17 J1: its own menu, as the truck has
        return MenuSubject::Constructor;
    case sim::UnitKind::ServiceTruck:
        return MenuSubject::ServiceTruck;
    }
    return MenuSubject::None;
}

MenuSubject menu_subject_of(const sim::Sim& sim, std::span<const sim::UnitId> selected) noexcept {
    bool any = false;
    MenuSubject subject = MenuSubject::None;
    for (const sim::UnitId id : selected) {
        const sim::Unit* unit = sim.find(id);
        if (unit == nullptr || !unit->alive) {
            continue;
        }
        // One subject per menu -- compared as SUBJECTS, not kinds, so a group of turrets of which
        // some are deployed is still a group of turrets and owns the turret menu.
        const MenuSubject mine = menu_subject_of_kind(unit->kind);
        if (any && mine != subject) {
            return MenuSubject::None;
        }
        subject = mine;
        any = true;
    }
    return subject;
}

std::vector<MenuRow> menu_rows(const MenuInput& in) {
    std::vector<MenuRow> rows;
    // An open repair list IS the menu: no state row, no verbs, only the way out and the patients.
    if (in.repair_list != nullptr && !in.repair_list->empty()) {
        rows.push_back(MenuRow{kRowKeyTab, std::string{kVerbCloseMenu}, true, MenuDim::None});
        append_unit_rows(rows, in, *in.repair_list, true, true);
        return rows;
    }
    // ... and so is an open unit list: `~` on a function key.
    if (in.unit_list != nullptr && !in.unit_list->empty()) {
        rows.push_back(MenuRow{kRowKeyTab, std::string{kVerbCloseMenu}, true, MenuDim::None});
        append_unit_rows(rows, in, *in.unit_list, false, true);
        return rows;
    }
    // ... and so is an open attack list (AW): `3 Attack`. Any key closes it, as it does the others.
    if (in.attack_list != nullptr && !in.attack_list->empty()) {
        rows.push_back(MenuRow{kRowKeyTab, std::string{kVerbCloseMenu}, true, MenuDim::None});
        append_unit_rows(rows, in, *in.attack_list, true, false);
        return rows;
    }
    // ... and so is an open nav list (AX): `2 Go to Nav`.
    if (in.nav_list != nullptr && !in.nav_list->empty()) {
        rows.push_back(MenuRow{kRowKeyTab, std::string{kVerbCloseMenu}, true, MenuDim::None});
        append_nav_rows(rows, *in.nav_list);
        return rows;
    }
    // ... and so is the Constructor's CONSTRUCT panel (round 17, J1): `3 Construct`.
    if (in.construct_panel && in.subject == MenuSubject::Constructor) {
        append_construct_panel(rows, in);
        return rows;
    }
    // Nothing selected: no menu, so nothing is drawn and a frame without a menu stays
    // byte-identical to one that never heard of this model.
    if (in.subject == MenuSubject::None && !in.attack && !in.repair && !in.go_here) {
        return rows;
    }
    rows.push_back(state_row(in));
    if (in.subject == MenuSubject::None) {
        return rows; // armed units with no menu of their own: the offer, and nothing under it
    }
    rows.push_back(MenuRow{kRowKeyTab, std::string{kVerbCloseMenu}, true, MenuDim::None});
    switch (in.subject) {
    case MenuSubject::Recycler:
        append_fixed(rows, kRecyclerRows);
        break;
    case MenuSubject::Harvester:
        rows.push_back(select_unit_row(in));
        append_fixed(rows, kHarvesterRows);
        break;
    case MenuSubject::Deployment:
    case MenuSubject::Factory:
        append_fixed(rows, kDeploymentRows);
        append_build_list(rows, in);
        break;
    case MenuSubject::Vehicle:
        // `~ Select Unit` first: the reference's row order.
        rows.push_back(select_unit_row(in));
        append_fixed(rows, kVehicleRows);
        break;
    case MenuSubject::ServiceTruck:
        rows.push_back(select_unit_row(in));
        append_fixed(rows, kServiceTruckRows);
        break;
    case MenuSubject::Constructor:
        rows.push_back(select_unit_row(in));
        append_fixed(rows, kConstructorRows);
        break;
    case MenuSubject::Turret:
        rows.push_back(select_unit_row(in));
        append_fixed(rows, kTurretHeadRows);
        rows.push_back(MenuRow{kUndeployKey, std::string{kVerbUndeploy}, in.deployed,
                               in.deployed ? MenuDim::None : MenuDim::DoesNotApply});
        append_fixed(rows, kTurretTailRows);
        break;
    case MenuSubject::None:
        // Answered above. Listed so a new subject is a compile-time finding, not a silent gap.
        break;
    }
    return rows;
}

} // namespace protekt::game
