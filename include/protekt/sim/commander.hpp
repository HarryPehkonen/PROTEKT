#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "protekt/sim/orders.hpp"
#include "protekt/sim/units.hpp"

namespace protekt::sim {

class Sim;

/// What a commander is allowed to see. Deliberately a plain data snapshot rather than the sim
/// itself, so a commander cannot reach around behind the interface -- the human, the scripted
/// opponent and (later) the AI all get exactly this and nothing else.
struct UnitView {
    UnitId id{};
    UnitKind kind{};
    Side side{};
    std::int32_t x{};
    std::int32_t y{}; ///< tile-floor of the unit's fixed-point position
    std::uint16_t hp{};
    std::int32_t raw_x{}; ///< the same position in Q16.16, for questions the tile cannot answer
    std::int32_t raw_y{};
};

struct Observation {
    Tick tick{};
    std::vector<UnitView> own{};
    std::vector<UnitView> enemy{};
    /// The ground, when the observer knows it: the seed that `sim::can_see` and
    /// `sim::tile_passable` answer from. Empty for a hand-built observation, and then nothing is
    /// hidden behind anything.
    std::optional<std::uint64_t> terrain{};
};

/// The one interface every commander implements: perception in, orders out. A human's input layer,
/// the scripted opponent and a future AI are interchangeable behind it.
///
/// It lives WITH THE SIM, not with any commander, because the sim is what calls it: `run_battle`
/// drives a battle through this interface, so the sim defines the contract it requires and the
/// implementations live above it (`ai/`, `cmd/`, `game/`). Declaring it in `ai/` put the dependency
/// the wrong way round -- `sim/observer.hpp` had to include the AI's header to name a parameter
/// type.
class ICommander {
public:
    virtual ~ICommander() = default;
    virtual void think(const Observation& observation, std::vector<Order>& out) = 0;

protected:
    // Copying or moving through the interface would slice the derived commander, so only the
    // derived classes may copy and move it.
    ICommander() = default;
    ICommander(const ICommander&) = default;
    ICommander& operator=(const ICommander&) = default;
    ICommander(ICommander&&) = default;
    ICommander& operator=(ICommander&&) = default;
};

/// Living units only, in ascending id order. The sim builds this -- it decides what a commander may
/// know -- which is why it is declared here and not in the AI's header.
[[nodiscard]] Observation observe(const Sim& sim, Side side);

} // namespace protekt::sim
