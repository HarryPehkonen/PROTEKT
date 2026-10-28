#pragma once

#include <vector>

#include "protekt/sim/outcome.hpp"
#include "protekt/sim/units.hpp"

namespace protekt::sim {

/// ONE WAY THE WAR ENDS, as data rather than as code (OBJ). The condition fires when every unit of
/// `kind` that `side` started with is gone; the verdict is what that means for the player. The list
/// is ordered and the order is the meaning: the first condition that fires hands down the verdict,
/// so a scenario can rule that losing your hauler loses the war before anything about recyclers is
/// asked.
///
/// It fires only for a side that HAD such a unit: a scenario that gives nobody a recycler asks
/// nothing of this condition, which is how a rig or a sandbox stays undecided for ever.
struct Objective {
    WarOutcome verdict{WarOutcome::Undecided};
    Side side{Side::Player};
    UnitKind kind{UnitKind::Recycler};

    /// Plain data, compared as plain data: a scenario's conditions can be held up against
    /// another's.
    friend bool operator==(const Objective&, const Objective&) = default;
};

/// The conditions the game plays unless its scenario says otherwise: your recycler lost is a loss,
/// theirs destroyed is a win, and the loss is checked first so a pair that dies together reads as a
/// loss. THIS IS THE ONE PLACE the built-in rule lives -- `ScenarioData`'s own default carries it,
/// so a game started from a scenario file that mentions no conditions still has a war to win.
[[nodiscard]] std::vector<Objective> built_in_objectives();

} // namespace protekt::sim
