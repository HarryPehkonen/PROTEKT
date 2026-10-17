#include "protekt/ai/utility.hpp"

namespace protekt::ai {

namespace {

/// Floor of the square root, by integer search, so a distance in tiles needs no floating point.
[[nodiscard]] std::int64_t isqrt(std::int64_t value) noexcept {
    std::int64_t root = 0;
    while ((root + 1) * (root + 1) <= value) {
        ++root;
    }
    return root;
}

[[nodiscard]] sim::Order order_for(const Decision& decision, const UnitView& self) {
    sim::Order order{};
    order.unit = decision.unit;
    switch (decision.intent) {
    case Intent::Engage:
        order.kind = sim::OrderKind::Attack;
        order.target = decision.target;
        break;
    case Intent::Reposition:
    case Intent::Retreat:
    case Intent::Advance:
        order.kind = sim::OrderKind::MoveTo;
        order.x = sim::Fixed::from_int(decision.x);
        order.y = sim::Fixed::from_int(decision.y);
        break;
    case Intent::Hold:
        order.kind = sim::OrderKind::GuardAt;
        order.x = sim::Fixed::from_int(self.x);
        order.y = sim::Fixed::from_int(self.y);
        break;
    }
    return order;
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

std::int64_t distance_squared(const UnitView& a, const UnitView& b) noexcept {
    const std::int64_t dx = static_cast<std::int64_t>(a.x) - b.x;
    const std::int64_t dy = static_cast<std::int64_t>(a.y) - b.y;
    return dx * dx + dy * dy;
}

std::int32_t score_engage(const UnitView& self, const UnitView& enemy) noexcept {
    const std::int64_t distance = isqrt(distance_squared(self, enemy));
    const std::int64_t threat = threat_of(enemy.kind);
    return static_cast<std::int32_t>(100 * threat - 5 * distance);
}

const UnitView* home_factory(const Observation& observation) noexcept {
    const UnitView* home = nullptr;
    for (const UnitView& unit : observation.own) {
        if (unit.kind == sim::UnitKind::Factory && (home == nullptr || unit.id < home->id)) {
            home = &unit;
        }
    }
    return home;
}

const UnitView* enemy_factory(const Observation& observation) noexcept {
    const UnitView* base = nullptr;
    for (const UnitView& unit : observation.enemy) {
        if (unit.kind == sim::UnitKind::Factory && (base == nullptr || unit.id < base->id)) {
            base = &unit;
        }
    }
    return base;
}

Decision advance_point(const UnitView& self, const UnitView& home, const UnitView& enemy_home,
                       const UtilityTuning& tuning) noexcept {
    Decision decision{};
    decision.unit = self.id;
    decision.intent = Intent::Advance;
    decision.x = home.x + ((enemy_home.x - home.x) * tuning.advance_percent) / 100;
    decision.y = home.y + ((enemy_home.y - home.y) * tuning.advance_percent) / 100;
    return decision;
}

const UnitView* choose_target(const UnitView& self, const Observation& observation,
                              const UtilityTuning& tuning) noexcept {
    const std::int64_t range = tuning.engage_range_tiles;
    const UnitView* best = nullptr;
    std::int32_t best_score = 0;
    for (const UnitView& enemy : observation.enemy) {
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

Decision decide(const UnitView& self, const Observation& observation,
                const UtilityTuning& tuning) noexcept {
    Decision decision{};
    decision.unit = self.id;
    // Only tanks move and hunt; turrets and factories hold whatever happens.
    if (self.kind != sim::UnitKind::Tank) {
        return decision;
    }
    const UnitView* home = home_factory(observation);
    if (self.hp <= tuning.retreat_hp) {
        decision.intent = Intent::Retreat;
        decision.x = home != nullptr ? home->x : self.x;
        decision.y = home != nullptr ? home->y : self.y;
        return decision;
    }
    if (const UnitView* target = choose_target(self, observation, tuning)) {
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
        if (const UnitView* enemy_home = enemy_factory(observation)) {
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

void UtilityCommander::think(const Observation& observation, std::vector<sim::Order>& out) {
    if (tuning_.cycle_ticks <= 0
        || observation.tick % static_cast<sim::Tick>(tuning_.cycle_ticks) != 0) {
        return;
    }
    decisions_.clear();
    for (const UnitView& unit : observation.own) {
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
