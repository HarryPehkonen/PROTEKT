#include "protekt/game/menu.hpp"

#include <array>
#include <cstddef>

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

/// The key column's three words, written once: the state row is the only row whose verb moves.
constexpr std::string_view kSpace{"SPACE"};
constexpr std::string_view kTab{"TAB"};
constexpr std::string_view kTilde{"~"};

constexpr std::string_view kCloseMenu{"CLOSE MENU"};

/// A recycler / matriarch that has not been deployed: it can be told where to go and where to
/// deploy there, and the turret-rotation verb is drawn but does not apply until it has buildings
/// to rotate.
constexpr std::array kRecyclerRows{
    FixedRow{kTilde, "ROTATE BUILDINGS", MenuDim::DoesNotApply},
    FixedRow{"1", "FOLLOW ME", MenuDim::None},
    FixedRow{"2", "GO TO NAV", MenuDim::None},
    FixedRow{"3", "DEPLOY", MenuDim::None},
};

/// A harvester / scavenger. Note what is NOT here: 6, 7 and 8 are missing on purpose and the gap is
/// drawn, because the reference shows a visible one -- keys are a pair of {key, verb}, never a
/// position in a list, and the free keys are left free for whatever fills them later. `~ SELECT
/// UNIT` and `5 PICK ME UP` are dimmed because they do not apply to this selection; their ink is
/// the same as a build row that is merely too expensive, and only `MenuDim` tells them apart.
constexpr std::array kHarvesterRows{
    FixedRow{kTilde, "SELECT UNIT", MenuDim::DoesNotApply},
    FixedRow{"1", "FOLLOW ME", MenuDim::None},
    FixedRow{"2", "GO TO NAV", MenuDim::None},
    FixedRow{"3", "SCAVENGE", MenuDim::None},
    FixedRow{"4", "HOLD", MenuDim::None},
    FixedRow{"5", "PICK ME UP", MenuDim::DoesNotApply},
    // 6, 7, 8: the gap.
    FixedRow{"9", "BAILOUT", MenuDim::None},
    FixedRow{"0", "RECYCLE", MenuDim::None},
};

/// A DEPLOYED recycler, above its build list. `~ DROP OFF` is drawn as the reference draws it and
/// means nothing to us yet: the string is rendered, no behaviour is invented behind it.
constexpr std::array kDeploymentRows{
    FixedRow{kTilde, "DROP OFF", MenuDim::None},
};

/// The player's build list, in key order, with the costs the reference capture draws on the rows.
/// RB5 owns where these costs really come from -- there is no per-kind cost in the sim today, only
/// `kTankOreCost` -- and the R3 roster owns the item names; this table is the reference's reading
/// of them, kept in one place so the two later slices have exactly one thing to replace.
constexpr std::array kPlayerBuildList{
    BuildItem{"1", "HARVESTER", 20},  BuildItem{"2", "SCOUT", 45},   BuildItem{"3", "GUARDIAN", 40},
    BuildItem{"4", "HAULER", 30},     BuildItem{"5", "BUILDER", 40}, BuildItem{"6", "HEALER", 50},
    BuildItem{"7", "SERVICE POD", 2},
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
        return "DEPLOY";
    case MenuCommand::Build:
        return std::string{in.armed.item};
    case MenuCommand::None:
        break;
    }
    return in.target == MenuTarget::Selectable ? std::string{"SELECT"} : std::string{};
}

/// The row everything else hangs under: always first, always live, never dimmed.
[[nodiscard]] MenuRow state_row(const MenuInput& in) {
    return MenuRow{std::string{kSpace}, state_verb(in), true, MenuDim::None};
}

void append_fixed(std::vector<MenuRow>& rows, std::span<const FixedRow> fixed) {
    for (const FixedRow& row : fixed) {
        rows.push_back(MenuRow{std::string{row.key}, std::string{row.verb},
                               row.dim == MenuDim::None, row.dim});
    }
}

/// The build list, with each row weighed against what is banked. An unaffordable row is dimmed for
/// `MenuDim::Unaffordable` -- not `DoesNotApply`: the item is buildable, it is simply not paid for.
void append_build_list(std::vector<MenuRow>& rows, const MenuInput& in) {
    for (const BuildItem& item : build_items(in.side)) {
        const bool affordable = in.ore >= item.cost;
        rows.push_back(MenuRow{std::string{item.key}, named_cost(item.label, item.cost), affordable,
                               affordable ? MenuDim::None : MenuDim::Unaffordable});
    }
}

} // namespace

std::span<const BuildItem> build_items(sim::Side side) noexcept {
    return side == sim::Side::Player ? std::span<const BuildItem>{kPlayerBuildList}
                                     : std::span<const BuildItem>{};
}

std::vector<MenuRow> menu_rows(const MenuInput& in) {
    std::vector<MenuRow> rows;
    // Nothing selected: no menu, so nothing is drawn and a frame without a menu stays
    // byte-identical to one that never heard of this model.
    if (in.subject == MenuSubject::None) {
        return rows;
    }
    rows.push_back(state_row(in));
    // A lone deployed turret has no verbs of its own: the reference collapses its menu to the state
    // row, so there is nothing under it -- not even CLOSE MENU.
    if (in.subject == MenuSubject::Turret) {
        return rows;
    }
    rows.push_back(MenuRow{std::string{kTab}, std::string{kCloseMenu}, true, MenuDim::None});
    switch (in.subject) {
    case MenuSubject::Recycler:
        append_fixed(rows, kRecyclerRows);
        break;
    case MenuSubject::Harvester:
        append_fixed(rows, kHarvesterRows);
        break;
    case MenuSubject::Deployment:
        append_fixed(rows, kDeploymentRows);
        append_build_list(rows, in);
        break;
    case MenuSubject::None:
    case MenuSubject::Turret:
        // Answered above. Listed so a new subject is a compile-time finding, not a silent gap.
        break;
    }
    return rows;
}

} // namespace protekt::game
