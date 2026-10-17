#pragma once

#include <cstdint>
#include <vector>

#include "protekt/ai/commander.hpp"
#include "protekt/sim/units.hpp"

/// A commander that decides from the state of the battlefield rather than from a script.
///
/// The scripted opponent attacks whatever is nearest, from any distance, at any health -- so its
/// tanks walk the length of the map one at a time and leave the base undefended. This decides from
/// what a unit can actually see, how hurt it is, and what it is supposed to be guarding.
///
/// Two rules shape it.
///
/// **Decisions are integers.** The AI needs a total order over a handful of options, not physical
/// accuracy, and integer arithmetic keeps every tie-break exact: two runs of the same battle cannot
/// disagree about which of two equidistant enemies dies first. No floats anywhere in a decision.
///
/// **Posture is a gate, not a tug of war.** A hurt tank withdraws; it does not weigh withdrawing
/// against shooting and lose because an enemy happens to be close. If the two competed on one
/// scale, the retreat threshold in the tuning would not describe the behaviour, and a constant
/// nobody can read off the outcome is a constant nobody can tune.
namespace protekt::ai {

/// What a unit means to do this cycle. Named and exposed, because an unused selling point is an
/// untestable one: "why is that tank going home?" is answerable only if the reason is a value
/// somebody can read.
enum class Intent : std::uint8_t {
    Hold = 0,       ///< nothing to do: keep the post, keep firing
    Engage = 1,     ///< attack the target it has chosen
    Reposition = 2, ///< nothing to shoot at: go back to the base area
    Retreat = 3,    ///< hurt: withdraw toward the base, whatever is in front of it
    Advance = 4,    ///< nothing in range and a war to go to: close the distance
};
inline constexpr int kIntentCount = 5;

/// The numbers, in one place, because they are balance rather than logic. Whole tiles and hit
/// points.
struct UtilityTuning {
    int engage_range_tiles{14}; ///< what a tank will consider engaging
    int retreat_hp{20};         ///< at or below this, a tank withdraws
    int station_radius_tiles{
        8};                  ///< how far from the base a tank will stray with nothing to shoot at
    int cycle_ticks{15};     ///< half a second at 30 Hz. Slow is the design, not a limitation.
    int advance_percent{50}; ///< how far along home->enemy-base a tank with nothing to shoot at
                             ///< will push. 50 stages both sides on the same midpoint, which is
                             ///< what makes two of these commanders actually meet.
};

/// One unit's decision, kept separate from the order it becomes: the interesting claims are about
/// the reason ("it withdrew because it was hurt"), and an order only ever shows the consequence.
struct Decision {
    sim::UnitId unit{};
    Intent intent{Intent::Hold};
    int x{}; ///< Reposition / Retreat: the tile it is heading for
    int y{};
    sim::UnitId target{}; ///< Engage: what it intends to kill
    std::int32_t score{}; ///< the score that won, for a debug overlay and for the tests
};

/// How dangerous a kind is. Tanks shoot back and move; factories do neither.
[[nodiscard]] int threat_of(sim::UnitKind kind) noexcept;

/// What a unit is worth shooting, from `self`'s tile. Higher is better; ties are broken by the
/// caller keeping the first best, and the observation is ordered by unit id, so ties go to the
/// lower id. `100 * threat - 5 * distance`, so a tank twelve tiles away beats the factory next to
/// you.
[[nodiscard]] std::int32_t score_engage(const UnitView& self, const UnitView& enemy) noexcept;

/// Squared Euclidean distance in whole tiles. Squared, because a comparison is all that is needed
/// and a square root would introduce a rounding the tie-breaks then depend on.
[[nodiscard]] std::int64_t distance_squared(const UnitView& a, const UnitView& b) noexcept;

/// The base a side falls back to: its lowest-id living factory, or nullptr if it has none left.
[[nodiscard]] const UnitView* home_factory(const Observation& observation) noexcept;

/// The same question for the other side: its lowest-id living factory, or nullptr. A commander that
/// cannot name the enemy's base cannot go there, and an opponent that never leaves its own half
/// never fights -- measured, before this existed: 0 shots in 2400 ticks with a utility commander on
/// each side, because neither of them ever closed the distance.
[[nodiscard]] const UnitView* enemy_factory(const Observation& observation) noexcept;

/// The enemy worth attacking, or nullptr if there is none in range. Posture is decided elsewhere;
/// this only answers "which one".
[[nodiscard]] const UnitView* choose_target(const UnitView& self, const Observation& observation,
                                            const UtilityTuning& tuning) noexcept;

/// The whole decision for one unit, in one pure function, so the policy can be read without reading
/// the loop that applies it.
[[nodiscard]] Decision decide(const UnitView& self, const Observation& observation,
                              const UtilityTuning& tuning) noexcept;

/// The utility opponent. Deterministic: same observation in, same decisions out, no hidden state
/// beyond the tuning and the last decisions it reported.
class UtilityCommander final : public ICommander {
public:
    explicit UtilityCommander(UtilityTuning tuning = {}) noexcept;

    void think(const Observation& observation, std::vector<sim::Order>& out) override;

    /// What it decided last time it thought, whether or not the decision became an order. An AI
    /// that can only be observed through its orders cannot be debugged.
    [[nodiscard]] const std::vector<Decision>& decisions() const noexcept;

    [[nodiscard]] const UtilityTuning& tuning() const noexcept;

private:
    UtilityTuning tuning_{};
    std::vector<Decision> decisions_{};
};

} // namespace protekt::ai
