#include "protekt/ai/utility.hpp"

#include <algorithm>
#include <cstdlib>
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

Role role_of(sim::UnitKind kind) noexcept {
    switch (kind) {
    case sim::UnitKind::Tank:
    case sim::UnitKind::HoverTank:
    case sim::UnitKind::RocketTank:
    case sim::UnitKind::Scout:    // armed, and it fights: a light gun is still a gun at the front
    case sim::UnitKind::MslScout: // the scout's hull and a rocket: it fights at the front too
    case sim::UnitKind::AssaultTank:
    case sim::UnitKind::Walker:
    case sim::UnitKind::Apc: // a light gun, but a gun: it fights at the front
        return Role::Frontline;
    case sim::UnitKind::MortarBike:
    case sim::UnitKind::Bomber: // the lobbed shell, like the bike's: it stands off
        return Role::Standoff;
    case sim::UnitKind::Turret:
    case sim::UnitKind::DeployedTurret: // the player's to deploy; the AI never does
    case sim::UnitKind::Factory:
    case sim::UnitKind::Scavenger: // unarmed: it hauls, so there is no decision to make about it
    case sim::UnitKind::Recycler:  // unarmed too: it is the base-to-be, with no hunt to run
    case sim::UnitKind::DeployedRecycler:  // the base itself: it never moves, so nothing to decide
    case sim::UnitKind::DeployedScavenger: // a fixture on its pool: nothing to decide either
        return Role::None;
    }
    return Role::None;
}

bool is_combatant(sim::UnitKind kind) noexcept { return role_of(kind) != Role::None; }

int threat_of(sim::UnitKind kind) noexcept {
    switch (kind) {
    case sim::UnitKind::Tank:
    case sim::UnitKind::HoverTank:
    case sim::UnitKind::MortarBike:
    case sim::UnitKind::RocketTank:
    case sim::UnitKind::AssaultTank:
    case sim::UnitKind::Walker:
    case sim::UnitKind::Bomber:
        return 3;
    case sim::UnitKind::Turret:
    case sim::UnitKind::DeployedTurret: // the same gun, fixed to its spot
    case sim::UnitKind::Apc:            // a light gun, like the scout's
    case sim::UnitKind::Scout:          // a light raider: armed, but outgunned by any tank's 3
    case sim::UnitKind::MslScout:       // the same raider hull, with the rocket's bite
        return 2;
    case sim::UnitKind::Factory:
        return 1;
    case sim::UnitKind::Scavenger: // unarmed: nothing to shoot back with, nothing to fear from it
    case sim::UnitKind::Recycler: // unarmed too: it cannot hurt anything, whatever it later becomes
    case sim::UnitKind::DeployedRecycler:  // and deployed it is still unarmed
    case sim::UnitKind::DeployedScavenger: // unarmed on its pool, as it was on wheels
        return 0;
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
    // Only combatants move and hunt; turrets and factories hold whatever happens.
    if (!is_combatant(self.kind)) {
        return decision;
    }
    const sim::UnitView* home = home_factory(observation);
    // Hysteresis without memory: a tank is "withdrawn" when it is wounded and standing at its own
    // living factory, so it stays until topped up. A wounded tank in the field keeps fighting.
    const std::int64_t repair_radius = sim::kRepairRadiusTiles;
    // The band's top is capped at the kind's own full health: a mortar bike is built at 75 hp, so
    // an absolute 80 would read a mortar bike at full health as forever "still healing" and keep it
    // home.
    const int topped_up = std::min<int>(tuning.return_hp, sim::profile(self.kind).hp);
    const bool healing_at_home = home != nullptr && self.hp < topped_up
                                 && distance_squared(self, *home) <= repair_radius * repair_radius;
    if (self.hp <= tuning.retreat_hp || healing_at_home) {
        decision.intent = Intent::Retreat;
        decision.x = home != nullptr ? home->x : self.x;
        decision.y = home != nullptr ? home->y : self.y;
        return decision;
    }
    if (const sim::UnitView* target = choose_target(self, observation, tuning)) {
        // A target it cannot see is a hill it would be shooting. It keeps the target and moves:
        // sideways first, because charging a target behind a rise is how a unit ends up pressed
        // against the rise, and straight at it only when no step to either side gives a line.
        // A standoff unit needs no line: its shell lobs over the rise, so it engages from where it
        // stands (the sim holds it at its weapon range) instead of walking up to the hill.
        if (role_of(self.kind) == Role::Frontline
            && terrain_hides(observation, sim::Fixed::from_raw(self.raw_x),
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

namespace {

/// The middle of a pool's tile, in Q16.16.
constexpr std::int64_t pool_centre_raw(int tile) noexcept {
    return (static_cast<std::int64_t>(tile) << sim::Fixed::kShift) + (sim::Fixed::kOne / 2);
}

/// A VEHICLE: something that drives. A turret crawls but is a post, not a vehicle.
bool is_vehicle(sim::UnitKind kind) noexcept {
    return kind != sim::UnitKind::Turret && sim::profile(kind).mobile;
}

} // namespace

bool vehicle_points_at(const sim::UnitView& vehicle, const Facing& facing,
                       const sim::Pool& pool) noexcept {
    const std::int64_t dx = pool_centre_raw(pool.tile_x) - vehicle.raw_x;
    const std::int64_t dy = pool_centre_raw(pool.tile_y) - vehicle.raw_y;
    const std::int64_t reach = static_cast<std::int64_t>(sim::radar_radius_tiles(vehicle.kind))
                               << sim::Fixed::kShift;
    if ((dx * dx) + (dy * dy) > reach * reach) {
        return false; // the vehicle cannot see it
    }
    const std::int64_t along
        = (static_cast<std::int64_t>(facing.vx) * dx) + (static_cast<std::int64_t>(facing.vy) * dy);
    const std::int64_t across
        = (static_cast<std::int64_t>(facing.vx) * dy) - (static_cast<std::int64_t>(facing.vy) * dx);
    // Within the cone: ahead of it, and the sideways part no more than tan(cone) of the forward.
    return along > 0 && std::abs(across) * 1'000'000 <= sim::kPointingConeTanMillionths * along;
}

void UtilityCommander::remember_facings(const sim::Observation& observation) {
    for (const sim::UnitView& unit : observation.own) {
        if (!is_vehicle(unit.kind) || (unit.raw_vx == 0 && unit.raw_vy == 0)) {
            continue; // standing still keeps the facing it had
        }
        const auto found = std::find_if(facings_.begin(), facings_.end(),
                                        [&unit](const Facing& f) { return f.unit == unit.id; });
        if (found != facings_.end()) {
            found->vx = unit.raw_vx;
            found->vy = unit.raw_vy;
        } else {
            facings_.push_back(Facing{unit.id, unit.raw_vx, unit.raw_vy});
        }
    }
}

void UtilityCommander::deploy_scavengers(const sim::Observation& observation,
                                         std::vector<sim::Order>& out) {
    // A claim ends when its scavenger is gone (dead, or deployed -- then the pool is taken), or its
    // time is up.
    std::erase_if(claims_, [&](const DeployClaim& claim) {
        const bool alive = std::any_of(
            observation.own.begin(), observation.own.end(), [&claim](const sim::UnitView& u) {
                return u.id == claim.scavenger && u.kind == sim::UnitKind::Scavenger;
            });
        return !alive
               || observation.tick - claim.tick
                      >= static_cast<sim::Tick>(tuning_.deploy_claim_ticks);
    });
    const auto enemy_scavenger = [](const sim::UnitView& u) {
        return u.side == sim::Side::Enemy && u.kind == sim::UnitKind::Scavenger;
    };
    // Litter in reach of any of the side's scavengers is worth collecting: no deploying then.
    for (const sim::UnitView& scav : observation.own) {
        if (!enemy_scavenger(scav)) {
            continue;
        }
        const std::int64_t reach = static_cast<std::int64_t>(sim::radar_radius_tiles(scav.kind))
                                   << sim::Fixed::kShift;
        for (const sim::Scrap& piece : observation.scrap) {
            const std::int64_t dx = static_cast<std::int64_t>(piece.x.raw) - scav.raw_x;
            const std::int64_t dy = static_cast<std::int64_t>(piece.y.raw) - scav.raw_y;
            if ((dx * dx) + (dy * dy) <= reach * reach) {
                return;
            }
        }
    }
    for (const sim::Pool& pool : observation.pools) {
        const auto holds_pool = [&pool](const sim::UnitView& u) {
            return u.kind == sim::UnitKind::DeployedScavenger && u.x == pool.tile_x
                   && u.y == pool.tile_y;
        };
        const bool claimed
            = std::any_of(claims_.begin(), claims_.end(), [&pool](const DeployClaim& c) {
                  return c.pool_x == pool.tile_x && c.pool_y == pool.tile_y;
              });
        if (claimed || std::any_of(observation.own.begin(), observation.own.end(), holds_pool)
            || std::any_of(observation.enemy.begin(), observation.enemy.end(), holds_pool)) {
            continue; // one scavenger per pool, from either side
        }
        const bool pointed_at = std::any_of(
            observation.own.begin(), observation.own.end(), [&](const sim::UnitView& vehicle) {
                if (vehicle.side != sim::Side::Enemy || !is_vehicle(vehicle.kind)) {
                    return false;
                }
                const auto facing
                    = std::find_if(facings_.begin(), facings_.end(),
                                   [&vehicle](const Facing& f) { return f.unit == vehicle.id; });
                return facing != facings_.end() && vehicle_points_at(vehicle, *facing, pool);
            });
        if (!pointed_at) {
            continue;
        }
        // The nearest scavenger not already sent, the lower id on a tie.
        const sim::UnitView* pick = nullptr;
        std::int64_t best = 0;
        for (const sim::UnitView& scav : observation.own) {
            const bool sent
                = std::any_of(claims_.begin(), claims_.end(),
                              [&scav](const DeployClaim& c) { return c.scavenger == scav.id; });
            if (!enemy_scavenger(scav) || sent) {
                continue;
            }
            const std::int64_t dx = pool_centre_raw(pool.tile_x) - scav.raw_x;
            const std::int64_t dy = pool_centre_raw(pool.tile_y) - scav.raw_y;
            const std::int64_t d2 = (dx * dx) + (dy * dy);
            if (pick == nullptr || d2 < best) {
                pick = &scav;
                best = d2;
            }
        }
        if (pick == nullptr) {
            return; // no scavenger left to send to this or any other pool
        }
        out.push_back(sim::Order{pick->id, sim::orders::Deploy{pool.tile_x, pool.tile_y}});
        claims_.push_back(DeployClaim{pick->id, pool.tile_x, pool.tile_y, observation.tick});
    }
}

void UtilityCommander::think(const sim::Observation& observation, std::vector<sim::Order>& out) {
    remember_facings(
        observation); // every call, not only on the beat: a heading is learned in motion
    if (tuning_.cycle_ticks <= 0
        || observation.tick % static_cast<sim::Tick>(tuning_.cycle_ticks) != 0) {
        return;
    }
    decisions_.clear();
    for (const sim::UnitView& unit : observation.own) {
        const Decision decision = decide(unit, observation, tuning_);
        decisions_.push_back(decision);
        // A combatant that holds needs no order; a turret holds by being told to guard its post.
        if (decision.intent == Intent::Hold && unit.kind != sim::UnitKind::Turret) {
            continue;
        }
        out.push_back(order_for(decision, unit));
    }
    deploy_scavengers(observation, out);
}

const std::vector<Decision>& UtilityCommander::decisions() const noexcept { return decisions_; }

const UtilityTuning& UtilityCommander::tuning() const noexcept { return tuning_; }

} // namespace protekt::ai
