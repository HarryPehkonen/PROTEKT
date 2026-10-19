#include "protekt/ai/utility.hpp"

#include <optional>

#include "protekt/sim/projectile.hpp"
#include "protekt/sim/sim.hpp"

namespace protekt::ai {

namespace {

[[nodiscard]] bool terrain_hides(const sim::Observation& observation, sim::Fixed from_x,
                                 sim::Fixed from_y, const sim::UnitView& enemy) noexcept {
    return observation.terrain.has_value()
           && !sim::can_see(*observation.terrain, from_x, from_y, sim::Fixed::from_raw(enemy.raw_x),
                            sim::Fixed::from_raw(enemy.raw_y), enemy.kind);
}

/// Where to step to get a line to `enemy`: a tile square to the line between the two, as near as it
/// can be found, trying one side and then the other at each distance. Empty when no tile within
/// reach has both a floor to stand on and a line to the enemy -- then there is nothing to step to,
/// and no route is searched for.
[[nodiscard]] std::optional<Decision> sidestep(const sim::UnitView& self,
                                               const sim::UnitView& enemy,
                                               const sim::Observation& observation,
                                               const UtilityTuning& tuning) noexcept {
    const std::int64_t dx = static_cast<std::int64_t>(enemy.x) - self.x;
    const std::int64_t dy = static_cast<std::int64_t>(enemy.y) - self.y;
    const std::int64_t length = sim::isqrt(dx * dx + dy * dy);
    if (length == 0 || tuning.sidestep_tiles <= 0) {
        return std::nullopt;
    }
    const std::int64_t range = tuning.engage_range_tiles;
    for (int reach = 1; reach <= tuning.sidestep_reach; ++reach) {
        const std::int64_t step = static_cast<std::int64_t>(reach) * tuning.sidestep_tiles;
        for (const std::int64_t side : {1, -1}) { // left of the line first, then right
            const int x = static_cast<int>(self.x - side * dy * step / length);
            const int y = static_cast<int>(self.y + side * dx * step / length);
            const std::int64_t ex = static_cast<std::int64_t>(enemy.x) - x;
            const std::int64_t ey = static_cast<std::int64_t>(enemy.y) - y;
            if (ex * ex + ey * ey > range * range
                || !sim::tile_passable(observation.terrain.value_or(0), x, y)
                || terrain_hides(observation, sim::Fixed::from_int(x), sim::Fixed::from_int(y),
                                 enemy)) {
                continue;
            }
            Decision decision{};
            decision.unit = self.id;
            decision.intent = Intent::Reposition;
            decision.target = enemy.id;
            decision.x = x;
            decision.y = y;
            return decision;
        }
    }
    return std::nullopt;
}

[[nodiscard]] sim::Order order_for(const Decision& decision, const sim::UnitView& self) {
    switch (decision.intent) {
    case Intent::Engage:
        return sim::Order{decision.unit, sim::orders::Attack{decision.target}};
    case Intent::Reposition:
    case Intent::Retreat:
    case Intent::Advance:
        return sim::Order{decision.unit, sim::orders::MoveTo{sim::Fixed::from_int(decision.x),
                                                             sim::Fixed::from_int(decision.y)}};
    case Intent::Hold:
        return sim::Order{decision.unit, sim::orders::GuardAt{sim::Fixed::from_int(self.x),
                                                              sim::Fixed::from_int(self.y)}};
    }
    return sim::Order{decision.unit, sim::orders::Stop{}};
}

} // namespace

int threat_of(sim::UnitKind kind) noexcept {
    switch (kind) {
    case sim::UnitKind::Tank:
        return 3;
    case sim::UnitKind::Turret:
        return 2;
    case sim::UnitKind::Factory:
        return 1;
    }
    return 0;
}

std::int64_t distance_squared(const sim::UnitView& a, const sim::UnitView& b) noexcept {
    const std::int64_t dx = static_cast<std::int64_t>(a.x) - b.x;
    const std::int64_t dy = static_cast<std::int64_t>(a.y) - b.y;
    return dx * dx + dy * dy;
}

std::int32_t score_engage(const sim::UnitView& self, const sim::UnitView& enemy) noexcept {
    const std::int64_t distance = sim::isqrt(distance_squared(self, enemy));
    const std::int64_t threat = threat_of(enemy.kind);
    return static_cast<std::int32_t>(100 * threat - 5 * distance);
}

const sim::UnitView* home_factory(const sim::Observation& observation) noexcept {
    const sim::UnitView* home = nullptr;
    for (const sim::UnitView& unit : observation.own) {
        if (unit.kind == sim::UnitKind::Factory && (home == nullptr || unit.id < home->id)) {
            home = &unit;
        }
    }
    return home;
}

const sim::UnitView* enemy_factory(const sim::Observation& observation) noexcept {
    const sim::UnitView* base = nullptr;
    for (const sim::UnitView& unit : observation.enemy) {
        if (unit.kind == sim::UnitKind::Factory && (base == nullptr || unit.id < base->id)) {
            base = &unit;
        }
    }
    return base;
}

Decision advance_point(const sim::UnitView& self, const sim::UnitView& home,
                       const sim::UnitView& enemy_home, const UtilityTuning& tuning) noexcept {
    Decision decision{};
    decision.unit = self.id;
    decision.intent = Intent::Advance;
    decision.x = home.x + ((enemy_home.x - home.x) * tuning.advance_percent) / 100;
    decision.y = home.y + ((enemy_home.y - home.y) * tuning.advance_percent) / 100;
    return decision;
}

const sim::UnitView* choose_target(const sim::UnitView& self, const sim::Observation& observation,
                                   const UtilityTuning& tuning) noexcept {
    const std::int64_t range = tuning.engage_range_tiles;
    const sim::UnitView* best = nullptr;
    std::int32_t best_score = 0;
    for (const sim::UnitView& enemy : observation.enemy) {
        if (distance_squared(self, enemy) > range * range) {
            continue;
        }
        const std::int32_t score = score_engage(self, enemy);
        if (best == nullptr || score > best_score) { // strict: ties keep the lower id
            best = &enemy;
            best_score = score;
        }
    }
    return best;
}

Decision decide(const sim::UnitView& self, const sim::Observation& observation,
                const UtilityTuning& tuning) noexcept {
    Decision decision{};
    decision.unit = self.id;
    // Only tanks move and hunt; turrets and factories hold whatever happens.
    if (self.kind != sim::UnitKind::Tank) {
        return decision;
    }
    const sim::UnitView* home = home_factory(observation);
    if (self.hp <= tuning.retreat_hp) {
        decision.intent = Intent::Retreat;
        decision.x = home != nullptr ? home->x : self.x;
        decision.y = home != nullptr ? home->y : self.y;
        return decision;
    }
    if (const sim::UnitView* target = choose_target(self, observation, tuning)) {
        // A target it cannot see is a hill it would be shooting. It keeps the target and moves:
        // sideways first, because charging a target behind a rise is how a unit ends up pressed
        // against the rise, and straight at it only when no step to either side gives a line.
        if (terrain_hides(observation, sim::Fixed::from_raw(self.raw_x),
                          sim::Fixed::from_raw(self.raw_y), *target)) {
            if (const std::optional<Decision> aside
                = sidestep(self, *target, observation, tuning)) {
                return *aside;
            }
            decision.intent = Intent::Advance;
            decision.target = target->id;
            decision.x = target->x;
            decision.y = target->y;
            return decision;
        }
        decision.intent = Intent::Engage;
        decision.target = target->id;
        decision.score = score_engage(self, *target);
        return decision;
    }
    const std::int64_t radius = tuning.station_radius_tiles;
    // Nothing in range. If both bases are known, the war is somewhere and this is a soldier: it
    // goes to the staging point between them and holds there once it arrives. Re-deciding the same
    // move forever would be harmless to the simulation and a lie in the decision log.
    if (home != nullptr) {
        if (const sim::UnitView* enemy_home = enemy_factory(observation)) {
            const Decision staging = advance_point(self, *home, *enemy_home, tuning);
            const std::int64_t dx = static_cast<std::int64_t>(staging.x) - self.x;
            const std::int64_t dy = static_cast<std::int64_t>(staging.y) - self.y;
            if ((dx * dx) + (dy * dy) > radius * radius) {
                decision = staging;
            }
            return decision; // arrived, or on the way: either way it does not also go home
        }
        if (distance_squared(self, *home) > radius * radius) {
            decision.intent = Intent::Reposition;
            decision.x = home->x;
            decision.y = home->y;
        }
    }
    return decision;
}

UtilityCommander::UtilityCommander(UtilityTuning tuning) noexcept : tuning_(tuning) {}

void UtilityCommander::think(const sim::Observation& observation, std::vector<sim::Order>& out) {
    if (tuning_.cycle_ticks <= 0
        || observation.tick % static_cast<sim::Tick>(tuning_.cycle_ticks) != 0) {
        return;
    }
    decisions_.clear();
    for (const sim::UnitView& unit : observation.own) {
        const Decision decision = decide(unit, observation, tuning_);
        decisions_.push_back(decision);
        // A tank that holds needs no order; a turret holds by being told to guard its post.
        if (decision.intent == Intent::Hold && unit.kind != sim::UnitKind::Turret) {
            continue;
        }
        out.push_back(order_for(decision, unit));
    }
}

const std::vector<Decision>& UtilityCommander::decisions() const noexcept { return decisions_; }

const UtilityTuning& UtilityCommander::tuning() const noexcept { return tuning_; }

} // namespace protekt::ai
