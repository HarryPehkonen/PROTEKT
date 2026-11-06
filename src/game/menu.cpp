#include "protekt/game/menu.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <utility>

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
void append_build_list(std::vector<MenuRow>& rows, const MenuInput& in) {
    for (const BuildItem& item : build_items(in.faction, in.subject)) {
        const bool affordable = in.ore >= item.cost;
        rows.push_back(MenuRow{item.key, named_cost(item.label, item.cost), affordable,
                               affordable ? MenuDim::None : MenuDim::Unaffordable});
    }
}

} // namespace

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
    case sim::UnitKind::Constructor:
    case sim::UnitKind::ServiceTruck:
    case sim::UnitKind::ServicePod:
        return MenuSubject::Vehicle;
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
    // Nothing selected: no menu, so nothing is drawn and a frame without a menu stays
    // byte-identical to one that never heard of this model.
    if (in.subject == MenuSubject::None && !in.attack && !in.go_here) {
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
