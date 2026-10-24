#include "protekt/sim/sim.hpp"

#include "protekt/sim/spec.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <type_traits>
#include <variant>

namespace protekt::sim {

UnitProfile profile(UnitKind kind) noexcept {
    if (static_cast<std::size_t>(kind) >= kUnitSpecs.size()) {
        return UnitProfile{};
    }
    const UnitSpec& spec = spec_of(kind);
    return UnitProfile{spec.hp,     spec.max_speed, spec.range,
                       spec.damage, spec.cooldown,  spec.max_speed.raw > 0};
}

Fixed terrain_speed_factor(world::Terrain terrain) noexcept {
    switch (terrain) {
    case world::Terrain::Flat:
        return Fixed::from_int(1);
    case world::Terrain::Rough:
        return Fixed::from_raw(Fixed::kOne * 3 / 4);
    case world::Terrain::Ridge:
        return Fixed::from_raw(Fixed::kOne / 2);
    case world::Terrain::Crater:
        return Fixed::zero();
    }
    return Fixed::zero();
}

namespace {

// The last representable position: just inside the map's far edge.
constexpr std::int32_t kMaxPosRaw = kMapTiles * Fixed::kOne - 1;

/// "No piece" from `nearest_scrap`.
constexpr std::size_t kNoScrap = static_cast<std::size_t>(-1);
/// The pick-up and hand-over radii, as squared raw distances (see kFetchRadiusTiles).
constexpr std::int64_t kFetchRadiusRaw2
    = (static_cast<std::int64_t>(kFetchRadiusTiles) << Fixed::kShift)
      * (static_cast<std::int64_t>(kFetchRadiusTiles) << Fixed::kShift);
constexpr std::int64_t kDeliverRadiusRaw2
    = (static_cast<std::int64_t>(kDeliverRadiusTiles) << Fixed::kShift)
      * (static_cast<std::int64_t>(kDeliverRadiusTiles) << Fixed::kShift);

Fixed clamp_pos(Fixed v) noexcept {
    return Fixed::from_raw(std::clamp<std::int32_t>(v.raw, 0, kMaxPosRaw));
}

// Positions and goals are always inside the map, so coordinate differences are < 2^24 raw and
// their squares fit comfortably in int64.
std::int64_t dist2(Fixed ax, Fixed ay, Fixed bx, Fixed by) noexcept {
    const std::int64_t dx = static_cast<std::int64_t>(bx.raw) - ax.raw;
    const std::int64_t dy = static_cast<std::int64_t>(by.raw) - ay.raw;
    return dx * dx + dy * dy;
}

Unit* find_mut(std::vector<Unit>& units, UnitId id) noexcept {
    if (!id.valid() || id.value > units.size()) {
        return nullptr;
    }
    Unit& unit = units[id.value - 1];
    return unit.id == id ? &unit : nullptr;
}

const Unit* find_const(const std::vector<Unit>& units, UnitId id) noexcept {
    if (!id.valid() || id.value > units.size()) {
        return nullptr;
    }
    const Unit& unit = units[id.value - 1];
    return unit.id == id ? &unit : nullptr;
}

/// The unit an order shoots at, if its kind has one that must be a living enemy.
const UnitId* order_target(const Order& order) noexcept {
    if (const auto* attack = std::get_if<orders::Attack>(&order.payload)) {
        return &attack->target;
    }
    if (const auto* fire = std::get_if<orders::FocusFire>(&order.payload)) {
        return &fire->target;
    }
    if (const auto* follow = std::get_if<orders::Follow>(&order.payload)) {
        return &follow->target; // a living unit of either side, never itself: the same rule
    }
    return nullptr;
}

bool order_is_valid(const std::vector<Unit>& units, const Order& order) noexcept {
    const Unit* unit = find_const(units, order.unit);
    if (unit == nullptr || !unit->alive) {
        return false;
    }
    if (const UnitId* id = order_target(order)) {
        const Unit* target = find_const(units, *id);
        // Any live unit is a legal explicit target, the issuer's own side included: the player may
        // deliberately shoot his own. Only auto-engagement is restricted to hostiles. A unit may
        // never name ITSELF, though: it sits at distance zero from itself, so the order would be
        // accepted and the unit would shoot itself dead without the player aiming at anything.
        return target != nullptr && target->alive && target->id != unit->id;
    }
    return true;
}

/// The world as the mover sees it: one question, answered from the seed. A designed map answers the
/// same question from its own tiles, and a route planner asks it many times, without the sim
/// caring.
struct WorldGround {
    std::uint64_t seed;
    [[nodiscard]] std::int64_t factor(int tile_x, int tile_y) const noexcept {
        const world::Tile tile = world::sample(seed, tile_x, tile_y);
        return terrain_speed_factor(tile.terrain).raw;
    }
};

/// Where a position-bearing order sends the unit; unused points are the origin, as they always
/// were.
void apply_posted(Unit& unit, Stance stance, Fixed x, Fixed y, Fixed x2, Fixed y2) noexcept {
    unit.stance = stance;
    unit.guard_x = clamp_pos(x);
    unit.guard_y = clamp_pos(y);
    unit.patrol_x = clamp_pos(x2);
    unit.patrol_y = clamp_pos(y2);
    unit.target = UnitId{};
    unit.route.age = kRouteAgeFresh; // a new goal re-plans at once; the same goal keeps its route
}

/// One tick of sideways drift. Applied where the order is, not stored: the lateral term is an input
/// like a throttle, so a tick with no Strafe order has none, and a zero direction moves nothing.
/// The step goes through the same terrain rule as any move, but a strafe never starts or ends a
/// detour -- that state belongs to the unit's MoveTo.
///
/// The vector's LENGTH is the throttle the driver's eased input asks for, capped at one tile-unit
/// (Fixed::kOne); its direction is where. A full-length vector therefore gives exactly 3/4 of the
/// unit's speed, as it always did -- which is why a commander that sends one (the AI) behaves
/// exactly as before. Integer parts only, and a zero length returns before any division.
void apply_strafe(Unit& unit, const orders::Strafe& strafe, std::uint64_t seed) noexcept {
    const UnitProfile prof = profile(unit.kind);
    const std::int64_t length = isqrt(static_cast<std::int64_t>(strafe.dx.raw) * strafe.dx.raw
                                      + static_cast<std::int64_t>(strafe.dy.raw) * strafe.dy.raw);
    if (!prof.mobile || length == 0) {
        return;
    }
    const std::int64_t full = prof.max_speed.raw * kStrafeSpeedNumerator / kStrafeSpeedDenominator;
    const std::int64_t speed = full * std::min<std::int64_t>(length, Fixed::kOne) / Fixed::kOne;
    if (speed == 0) {
        return;
    }
    StepRequest request;
    request.x = unit.x;
    request.y = unit.y;
    request.want_dx = strafe.dx.raw * speed / length;
    request.want_dy = strafe.dy.raw * speed / length;
    request.speed = speed;
    const StepPlan plan = plan_step(request, WorldGround{seed});
    if (!plan.moved) {
        return;
    }
    unit.x = Fixed::from_raw(static_cast<std::int32_t>(
        std::clamp<std::int64_t>(unit.x.raw + plan.step.dx, 0, kMaxPosRaw)));
    unit.y = Fixed::from_raw(static_cast<std::int32_t>(
        std::clamp<std::int64_t>(unit.y.raw + plan.step.dy, 0, kMaxPosRaw)));
    unit.still_ticks = 0; // moved: a turret is unmounted
}

void apply_order(std::vector<Unit>& units, std::vector<Projectile>& projectiles,
                 std::vector<Sim::Shot>& shots, const Order& order, std::uint64_t seed) noexcept {
    // Re-validated here: the unit may have died between enqueue and the tick it takes effect.
    if (!order_is_valid(units, order)) {
        return;
    }
    Unit& unit = *find_mut(units, order.unit);
    // Dispatch by kind() and read with get_if, rather than std::visit: this function is noexcept
    // and `std::visit` is not -- bad_variant_access is a terminate this code can never actually
    // reach, and a terminate is not a failure mode to admit into the tick loop. The switch has NO
    // default, so a new kind is a -Wswitch error rather than a silently ignored order.
    switch (order.kind()) {
    case OrderKind::MoveTo:
        if (const auto* o = std::get_if<orders::MoveTo>(&order.payload)) {
            apply_posted(unit, Stance::MoveTo, o->x, o->y, Fixed{}, Fixed{});
        }
        break;
    case OrderKind::GuardAt:
        if (const auto* o = std::get_if<orders::GuardAt>(&order.payload)) {
            apply_posted(unit, Stance::GuardAt, o->x, o->y, Fixed{}, Fixed{});
        }
        break;
    case OrderKind::Patrol:
        if (const auto* o = std::get_if<orders::Patrol>(&order.payload)) {
            apply_posted(unit, Stance::Patrol, o->x, o->y, o->x2, o->y2);
        }
        break;
    case OrderKind::Attack:
        if (const auto* o = std::get_if<orders::Attack>(&order.payload)) {
            unit.stance = Stance::Attack;
            unit.target = o->target;
            unit.route.age = kRouteAgeFresh;
        }
        break;
    case OrderKind::Follow:
        if (const auto* o = std::get_if<orders::Follow>(&order.payload)) {
            unit.stance = Stance::Follow;
            unit.target = o->target;
            unit.route.age = kRouteAgeFresh;
        }
        break;
    case OrderKind::Stop:
        unit.stance = Stance::Idle;
        unit.target = UnitId{};
        unit.focus_target = UnitId{}; // and hold fire
        unit.route = Route{};
        unit.vx = Fixed::zero();
        unit.vy = Fixed::zero();
        break;
    case OrderKind::FocusFire:
        if (const auto* o = std::get_if<orders::FocusFire>(&order.payload)) {
            // Nothing but the target: no stance, no destination, no velocity. The shot it names
            // clears it, and so does Stop.
            unit.focus_target = o->target;
        }
        break;
    case OrderKind::Strafe:
        if (const auto* o = std::get_if<orders::Strafe>(&order.payload)) {
            apply_strafe(unit, *o, seed);
        }
        break;
    case OrderKind::FireLob:
        if (const auto* o = std::get_if<orders::FireLob>(&order.payload)) {
            // The player's mortar: FireTowards' rules (armed, reloaded, mounted, no naming
            // oneself, no Fired event) with the MORTAR's shell, damage and reload.
            const UnitProfile prof = profile(unit.kind);
            const UnitProfile shell = profile(UnitKind::Mortar);
            if (prof.weapon_range.raw > 0 && prof.damage > 0 && unit.cooldown == 0
                && is_mounted(unit)) {
                const UnitId aimed_at = o->aimed_at == unit.id ? UnitId{} : o->aimed_at;
                projectiles.push_back(launch_lob(unit, o->dir_x, o->dir_y, o->dir_z, aimed_at,
                                                 shell.damage, seed, o->round));
                shots.push_back(Sim::Shot{unit.id, UnitId{}});
                unit.cooldown = shell.cooldown_ticks;
            }
        }
        break;
    case OrderKind::FireTowards:
        if (const auto* o = std::get_if<orders::FireTowards>(&order.payload)) {
            // The player's trigger: the bolt is launched here, where the order is applied, and the
            // gun reloads as it does after any other shot. A gun still reloading, or a unit with
            // none, fires nothing. The bolt flies on through this tick's step 3, along the
            // direction and nowhere else. `aimed_at` only says who it may hurt on its own side.
            // A unit may not name itself: its own bolt starts on top of it.
            const UnitProfile prof = profile(unit.kind);
            if (prof.weapon_range.raw > 0 && prof.damage > 0 && unit.cooldown == 0
                && is_mounted(unit)) {
                const UnitId aimed_at = o->aimed_at == unit.id ? UnitId{} : o->aimed_at;
                projectiles.push_back(launch_towards(unit, o->dir_x, o->dir_y, o->dir_z, aimed_at,
                                                     prof.damage, seed));
                // The rule: the sim reports `Fired` for the shots it decided to make (gun fire); a
                // player's manual shot is the app's to announce, with its own sound. So the Shot is
                // recorded with NO target whatever `aimed_at` says -- a valid target here is what
                // the observer turns into a Fired event, and the player would hear it twice.
                shots.push_back(Sim::Shot{unit.id, UnitId{}});
                unit.cooldown = prof.cooldown_ticks;
            }
        }
        break;
    }
}

struct Coord {
    int x;
    int y;
};

// The spawn tile itself when passable; otherwise the first passable tile found scanning square
// rings of growing radius in row-major order, so a unit never starts inside a crater.
Coord nearest_passable(std::uint64_t seed, int x, int y) noexcept {
    auto passable = [seed](int tx, int ty) {
        return tx >= 0 && ty >= 0 && tx < kMapTiles && ty < kMapTiles
               && terrain_speed_factor(world::sample(seed, tx, ty).terrain).raw != 0;
    };
    if (passable(x, y)) {
        return Coord{x, y};
    }
    for (int r = 1; r < kMapTiles; ++r) {
        for (int dy = -r; dy <= r; ++dy) {
            const int step = (dy == -r || dy == r) ? 1 : 2 * r;
            for (int dx = -r; dx <= r; dx += step) {
                if (passable(x + dx, y + dy)) {
                    return Coord{x + dx, y + dy};
                }
            }
        }
    }
    return Coord{x, y};
}

// Sets unit.vx/vy for this tick from the unit's stance. Pure integer steering: head straight at
// the goal at max speed; once inside the arrive radius (or, for a chase, inside weapon range)
// stand still. A patrol that arrives swaps its two ends, so it walks back and forth.
constexpr std::int64_t kWaypointArriveRaw = 1 << 15; ///< half a tile

[[nodiscard]] TileXY tile_of(Fixed x, Fixed y) noexcept {
    return TileXY{static_cast<std::uint8_t>(std::clamp(x.floor_int(), 0, kMapTiles - 1)),
                  static_cast<std::uint8_t>(std::clamp(y.floor_int(), 0, kMapTiles - 1))};
}

[[nodiscard]] Fixed tile_center(std::uint8_t tile) noexcept {
    return Fixed::from_raw((static_cast<std::int32_t>(tile) << Fixed::kShift) + (Fixed::kOne / 2));
}

/// Keeps `unit.route` serving `goal`: plans when the goal tile has drifted more than
/// kReplanGoalDriftTiles from the one served, when the unit has strayed more than
/// kReplanOffRouteTiles from its leg, or when a truncated route has run out -- the last two (and
/// the first, once a route exists) no sooner than kReplanMinTicks after the previous plan. A plan
/// that finds no route leaves the unit with none, which is today's steer-at-goal.
void update_route(Unit& unit, Fixed goal_x, Fixed goal_y, const WorldGround& ground,
                  std::uint32_t& plans) noexcept {
    Route& route = unit.route;
    if (route.age < kRouteAgeFresh) {
        ++route.age;
    }
    const TileXY goal = tile_of(goal_x, goal_y);
    bool replan = false;
    if (!route.has_goal) {
        replan = true;
    } else {
        const int drift = std::max(std::abs(static_cast<int>(goal.x) - route.goal.x),
                                   std::abs(static_cast<int>(goal.y) - route.goal.y));
        const bool settled = route.age >= kReplanMinTicks;
        const bool drifted = drift > kReplanGoalDriftTiles && settled;
        const bool spent = !route.following() && route.partial;
        if (drifted || spent) {
            replan = true;
        } else if (route.following() && settled) {
            const std::int64_t ax = static_cast<std::int64_t>(route.from.x) * 256 + 128;
            const std::int64_t ay = static_cast<std::int64_t>(route.from.y) * 256 + 128;
            const std::int64_t bx
                = static_cast<std::int64_t>(route.points.at(route.next).x) * 256 + 128;
            const std::int64_t by
                = static_cast<std::int64_t>(route.points.at(route.next).y) * 256 + 128;
            const std::int64_t off
                = point_segment_dist2(unit.x.raw >> 8, unit.y.raw >> 8, ax, ay, bx, by);
            const std::int64_t limit = static_cast<std::int64_t>(kReplanOffRouteTiles) * 256;
            replan = off > limit * limit;
        }
    }
    if (!replan) {
        return;
    }
    ++plans;
    (void)plan_route(ground, tile_of(unit.x, unit.y), goal, route);
}

/// A kind with a gun. `range` is the one thing the sim, the spec table and the AI all agree on for
/// "armed": a zero range means it never fires and is never a threat.
bool armed_kind(UnitKind kind) noexcept { return spec_of(kind).range.raw > 0; }

/// IS THIS SIDE SHORT A GUN AND ABLE TO PAY FOR ONE? The condition C4's production beat enacts and
/// C8's verdict about the war asks about, written ONCE. A side at its starting strength does not
/// build (a factory replaces a loss, it never rearms), and a side that cannot afford a tank does
/// not either. Two callers reading one function is what stops the outcome from disagreeing with the
/// factory that enacts it.
[[nodiscard]] bool short_and_affordable(int armed_living, int armed_started,
                                        std::uint16_t ore) noexcept {
    return armed_living < armed_started && ore >= kTankOreCost;
}

/// Where a factory's new tank appears: the factory's own tile when nothing is standing on it,
/// otherwise the first free passable tile on growing square rings, to `kBuildSpawnRadiusTiles`.
/// `{-1, -1}` means the yard is full: no build, and no ore spent.
Coord builder_spot(std::uint64_t seed, const std::vector<Unit>& units, Fixed fx,
                   Fixed fy) noexcept {
    const TileXY home = tile_of(fx, fy);
    const auto free_tile = [seed, &units](int tx, int ty) {
        if (!tile_passable(seed, tx, ty)) {
            return false;
        }
        for (const Unit& unit : units) {
            const TileXY at = tile_of(unit.x, unit.y);
            if (unit.alive && at.x == tx && at.y == ty) {
                return false;
            }
        }
        return true;
    };
    if (free_tile(home.x, home.y)) {
        return Coord{home.x, home.y};
    }
    for (int r = 1; r <= kBuildSpawnRadiusTiles; ++r) {
        for (int ty = home.y - r; ty <= home.y + r; ++ty) {
            for (int tx = home.x - r; tx <= home.x + r; ++tx) {
                const bool on_ring
                    = ty == home.y - r || ty == home.y + r || tx == home.x - r || tx == home.x + r;
                if (on_ring && free_tile(tx, ty)) {
                    return Coord{tx, ty};
                }
            }
        }
    }
    return Coord{-1, -1};
}

/// A unit as `create` makes one, for the one path that adds a unit after the battle has started:
/// the factory's build (C4). The tile is a whole tile, as a spawn's is.
Unit fresh_unit(std::uint32_t id, UnitKind kind, Side side, Coord tile) noexcept {
    const UnitProfile prof = profile(kind);
    Unit unit{};
    unit.id = UnitId{id};
    unit.kind = kind;
    unit.side = side;
    unit.x = Fixed::from_int(tile.x);
    unit.y = Fixed::from_int(tile.y);
    unit.hp = prof.hp;
    unit.cooldown = prof.cooldown_ticks; // a fresh gun starts reloading
    if (kind == UnitKind::Turret) {
        unit.still_ticks = kMountTicks; // every turret starts mounted
    }
    unit.stance = Stance::Idle;
    unit.alive = true;
    return unit;
}

/// The nearest piece of wreckage, ties broken by the order the pieces were made in -- which is the
/// order `scraps_` itself is in, so "the nearest" is a function of the litter and not of the walk.
std::size_t nearest_scrap(const std::vector<Scrap>& scraps, Fixed x, Fixed y) noexcept {
    std::size_t best = kNoScrap;
    std::int64_t best_d = 0;
    for (std::size_t i = 0; i < scraps.size(); ++i) {
        const std::int64_t d = dist2(x, y, scraps.at(i).x, scraps.at(i).y);
        if (best == kNoScrap || d < best_d) {
            best = i;
            best_d = d;
        }
    }
    return best;
}

/// Wreckage settles where a hauler can reach it: a piece the hash dropped on ground no vehicle may
/// enter -- a crater -- moves to the nearest tile one can, and never onto a tile another piece of
/// the same wreck already holds. Deterministic, like the scatter itself: the same battle settles
/// the same way. Without this a piece can lie in a crater where the only way to it is a tile the
/// hauler cannot stand on, and a scavenger drives at the crater rim forever.
void settle_litter(std::vector<Scrap>& scraps, std::size_t first, std::uint64_t seed) noexcept {
    for (std::size_t i = first; i < scraps.size(); ++i) {
        TileXY at = tile_of(scraps.at(i).x, scraps.at(i).y);
        if (!tile_passable(seed, at.x, at.y)) {
            const Coord spot = nearest_passable(seed, at.x, at.y);
            at = TileXY{static_cast<std::uint8_t>(spot.x), static_cast<std::uint8_t>(spot.y)};
        }
        // One tile, one piece of this wreck: step east (a standable tile, the same way every time)
        // while a piece of the same wreck already lies where this one would.
        for (int step = 0; step < kScrapSpan; ++step) {
            bool taken = false;
            for (std::size_t j = first; j < i && !taken; ++j) {
                const TileXY other = tile_of(scraps.at(j).x, scraps.at(j).y);
                taken = other.x == at.x && other.y == at.y;
            }
            if (!taken) {
                break;
            }
            const Coord next = nearest_passable(seed, static_cast<int>(at.x) + 1, at.y);
            at = TileXY{static_cast<std::uint8_t>(next.x), static_cast<std::uint8_t>(next.y)};
        }
        scraps.at(i) = Scrap{tile_center(at.x), tile_center(at.y)};
    }
}

/// The lowest-id living factory of a side: the same "home" every other rule in the sim means by it
/// (repair, retreat), asked here for a caller that is only passing through.
const Unit* home_factory(const std::vector<Unit>& units, Side side) noexcept {
    const Unit* home = nullptr;
    for (const Unit& unit : units) {
        if (unit.alive && unit.kind == UnitKind::Factory && unit.side == side
            && (home == nullptr || unit.id < home->id)) {
            home = &unit;
        }
    }
    return home;
}

/// THE FETCH (C2). Called for every living scavenger before it is steered, in ascending unit order,
/// so a piece a lower id picked up this tick is already gone when a higher id looks for one.
///
/// Picking up and handing over are matters of where the hauler IS, not of what it was told:
/// wreckage under the tracks is taken even by a scavenger a player is driving elsewhere, and a load
/// that reaches its own yard is banked however it got there. The GOAL is the sim's business alone,
/// and only for a scavenger with no orders of its own (`Idle` or already `Fetch`): carrying, the
/// goal is home; empty, it is the nearest piece; with no wreckage left, the fetch ends and it
/// stands. Any order sets a stance of its own, which is what stops the fetch -- the sim never
/// argues with one.
void fetch_step(std::vector<Unit>& units, std::vector<Scrap>& scraps,
                std::array<std::uint16_t, 2>& ore, Unit& unit) noexcept {
    if (unit.cargo == 0) {
        const std::size_t pick = nearest_scrap(scraps, unit.x, unit.y);
        if (pick != kNoScrap
            && dist2(unit.x, unit.y, scraps.at(pick).x, scraps.at(pick).y) <= kFetchRadiusRaw2) {
            scraps.erase(scraps.begin() + static_cast<std::ptrdiff_t>(pick));
            unit.cargo = 1;
        }
    }
    const Unit* home = home_factory(units, unit.side);
    if (unit.cargo > 0 && home != nullptr
        && dist2(unit.x, unit.y, home->x, home->y) <= kDeliverRadiusRaw2) {
        unit.cargo = 0;
        ++ore.at(static_cast<std::size_t>(unit.side));
    }
    if (unit.stance != Stance::Idle && unit.stance != Stance::Fetch) {
        return; // under an order: it collects what it drives over, and goes where it was sent
    }
    if (unit.cargo > 0) {
        if (home == nullptr) {
            unit.stance = Stance::Idle; // hauling with no base to bank it at: hold where it stands
            return;
        }
        unit.guard_x = home->x;
        unit.guard_y = home->y;
        unit.stance = Stance::Fetch;
        return;
    }
    const std::size_t pick = nearest_scrap(scraps, unit.x, unit.y);
    if (pick == kNoScrap) {
        unit.stance = Stance::Idle;
        return;
    }
    unit.guard_x = scraps.at(pick).x;
    unit.guard_y = scraps.at(pick).y;
    unit.stance = Stance::Fetch;
}

void steer(std::vector<Unit>& units, Unit& unit, const UnitProfile& prof, const WorldGround& ground,
           std::uint32_t& plans) noexcept {
    unit.vx = Fixed::zero();
    unit.vy = Fixed::zero();
    Fixed goal_x = unit.guard_x;
    Fixed goal_y = unit.guard_y;
    std::int64_t stop_radius = kArriveRadiusRaw;
    switch (unit.stance) {
    case Stance::MoveTo:
    case Stance::GuardAt:
    case Stance::Patrol:
    // The fetch steers at whatever goal `fetch_step` wrote into guard_x/guard_y this tick.
    case Stance::Fetch:
        break;
    case Stance::Attack: {
        const Unit* target = find_const(units, unit.target);
        if (target == nullptr || !target->alive) {
            unit.stance = Stance::Idle;
            unit.target = UnitId{};
            return;
        }
        if (unit.kind == UnitKind::Turret) {
            return; // shoots the named target in place (see step 4); an attack never moves a turret
        }
        goal_x = target->x;
        goal_y = target->y;
        if (prof.weapon_range.raw > 0) {
            stop_radius = prof.weapon_range.raw;
        }
        break;
    }
    case Stance::Follow: {
        // Shadow the target's CURRENT position: close to the radius, hold inside it. Unlike an
        // attack there is no weapon range here and nothing else is ever chased -- the gun fires
        // from step 4 at whatever is in range, wherever the follower happens to be.
        const Unit* target = find_const(units, unit.target);
        if (target == nullptr || !target->alive) {
            unit.stance = Stance::Idle; // moot: defend in place
            unit.target = UnitId{};
            return;
        }
        goal_x = target->x;
        goal_y = target->y;
        stop_radius = static_cast<std::int64_t>(kFollowRadiusTiles) << Fixed::kShift;
        break;
    }
    case Stance::Idle:
    case Stance::Dead:
        return;
    }

    const std::int64_t d2 = dist2(unit.x, unit.y, goal_x, goal_y);
    if (d2 < stop_radius * stop_radius) {
        if (unit.stance == Stance::Patrol) {
            std::swap(unit.guard_x, unit.patrol_x);
            std::swap(unit.guard_y, unit.patrol_y);
        }
        return;
    }
    // The route, if there is one: aim at the current waypoint (a tile center) and pop it on
    // arrival; the goal itself is only steered at once the waypoints are spent.
    update_route(unit, goal_x, goal_y, ground, plans);
    Fixed aim_x = goal_x;
    Fixed aim_y = goal_y;
    while (unit.route.following()) {
        const TileXY wp = unit.route.points.at(unit.route.next);
        if (!unit.route.partial && unit.route.next + 1 == unit.route.count) {
            // The last waypoint is the goal's own tile: the live goal (which may have moved, or
            // sit off the tile center) is a better thing to steer at, so the route ends here.
            unit.route.from = wp;
            ++unit.route.next;
            break;
        }
        aim_x = tile_center(wp.x);
        aim_y = tile_center(wp.y);
        if (dist2(unit.x, unit.y, aim_x, aim_y) >= kWaypointArriveRaw * kWaypointArriveRaw) {
            break;
        }
        unit.route.from = wp;
        ++unit.route.next;
        aim_x = goal_x;
        aim_y = goal_y;
    }
    const std::int64_t dist = isqrt(dist2(unit.x, unit.y, aim_x, aim_y));
    if (dist == 0) {
        return;
    }
    const std::int64_t dx = static_cast<std::int64_t>(aim_x.raw) - unit.x.raw;
    const std::int64_t dy = static_cast<std::int64_t>(aim_y.raw) - unit.y.raw;
    const std::int64_t speed = prof.max_speed.raw;
    std::int64_t vx = dx;
    std::int64_t vy = dy;
    if (dist > speed) {
        vx = dx * speed / dist;
        vy = dy * speed / dist;
    }
    unit.vx = Fixed::from_raw(static_cast<std::int32_t>(vx));
    unit.vy = Fixed::from_raw(static_cast<std::int32_t>(vy));
}

// The unmount gate. A turret that wants to move (steer gave it a velocity) but is mounted first
// spends kUnmountTicks unmounting: no movement, no fire (still_ticks is 0 for the duration, so it
// is not mounted). A turret that stops wanting to move mid-way abandons the unmount and mounts
// again after a fresh kMountTicks of stillness -- the progress is not kept. Returns whether the
// turret may move this tick.
[[nodiscard]] bool turret_may_move(Unit& unit) noexcept {
    if (unit.vx.raw == 0 && unit.vy.raw == 0) {
        if (unit.unmount_ticks != 0) {
            unit.unmount_ticks = 0;
            unit.still_ticks = 1; // this tick was still too: the pre-step skipped its count
        }
        return true; // nothing to move anyway
    }
    if (unit.unmount_ticks == 0 && unit.still_ticks < kMountTicks) {
        return true; // already unmounted: crawls
    }
    if (unit.unmount_ticks == 0) {
        unit.still_ticks = 0; // the gun comes off its mount now
    }
    unit.unmount_ticks = static_cast<std::uint16_t>(unit.unmount_ticks + 1);
    if (unit.unmount_ticks >= kUnmountTicks) {
        unit.unmount_ticks = 0; // unmounted (still_ticks is 0): it crawls from the next tick
    }
    unit.vx = Fixed::zero();
    unit.vy = Fixed::zero();
    return false;
}

// Moves a unit by the step `plan_step` chose: straight on where it can, along a rim where it must,
// and around an obstacle it cannot pass. The choice itself lives in sim/steering.hpp, which is the
// seam a planned route will plug into.
void move(const WorldGround& ground, const UnitProfile& prof, Unit& unit) noexcept {
    if (unit.vx.raw == 0 && unit.vy.raw == 0) {
        return;
    }
    StepRequest request;
    request.x = unit.x;
    request.y = unit.y;
    request.want_dx = unit.vx.raw;
    request.want_dy = unit.vy.raw;
    request.speed = prof.max_speed.raw;
    request.detour = unit.detour;

    const StepPlan plan = plan_step(request, ground);
    unit.detour = plan.detour;
    if (!plan.moved) {
        unit.vx = Fixed::zero();
        unit.vy = Fixed::zero();
        return;
    }
    unit.x = Fixed::from_raw(static_cast<std::int32_t>(
        std::clamp<std::int64_t>(unit.x.raw + plan.step.dx, 0, kMaxPosRaw)));
    unit.y = Fixed::from_raw(static_cast<std::int32_t>(
        std::clamp<std::int64_t>(unit.y.raw + plan.step.dy, 0, kMaxPosRaw)));
    unit.still_ticks = 0; // moved: a turret is unmounted
}

// FNV-1a over little-endian bytes: specified here, so the hash is the same on every machine.
class Hasher {
public:
    void u64(std::uint64_t value) noexcept {
        for (int shift = 0; shift < 64; shift += 8) {
            state_ = (state_ ^ ((value >> shift) & 0xFFU)) * 0x100000001B3ULL;
        }
    }
    void i32(std::int32_t value) noexcept { u64(static_cast<std::uint32_t>(value)); }
    [[nodiscard]] std::uint64_t value() const noexcept { return state_; }

private:
    std::uint64_t state_ = 0xCBF29CE484222325ULL;
};

} // namespace

Sim Sim::create(const Scenario& scenario) {
    Sim sim;
    sim.scenario_ = scenario;
    sim.units_.reserve(scenario.spawns.size());
    for (const Spawn& spawn : scenario.spawns) {
        // Out-of-range spawns are clamped onto the map rather than rejected: total, not UB.
        const Coord tile
            = nearest_passable(scenario.seed, std::clamp(spawn.tile_x, 0, kMapTiles - 1),
                               std::clamp(spawn.tile_y, 0, kMapTiles - 1));
        sim.units_.push_back(fresh_unit(sim.next_id_++, spawn.kind, spawn.side, tile));
    }
    return sim;
}

void Sim::enqueue(const Order& order) {
    // The generic command channel: the human's input layer, a replay being replayed, and an AI all
    // arrive here. The sim has no idea WHO is issuing, so it must not filter by side -- ownership
    // is the commander's responsibility. Filtering here silently killed the enemy commander.
    if (!order_is_valid(units_, order)) {
        return;
    }
    pending_.push_back(order);
    log_.push_back(Command{tick_, order});
}

void Sim::step() {
    // 1. orders, in the order enqueued.
    shots_.clear();
    // A turret that has not moved yet this tick has been still one tick longer; any move below
    // (a strafe in this step's orders, or the mover) puts it back to zero.
    for (Unit& unit : units_) {
        if (unit.kind == UnitKind::Turret && unit.still_ticks < kMountTicks
            && unit.unmount_ticks == 0) {
            ++unit.still_ticks;
        }
    }
    for (const Order& order : pending_) {
        apply_order(units_, projectiles_, shots_, order, scenario_.seed);
    }
    pending_.clear();

    // 2. movement, ascending id (units_ is stored in id order).
    const std::uint64_t seed = scenario_.seed;
    const WorldGround ground{seed};
    for (Unit& unit : units_) {
        if (!unit.alive) {
            continue;
        }
        const UnitProfile prof = profile(unit.kind);
        // The fetch, before the mover: it writes the goal the mover is about to steer at (C2).
        if (unit.kind == UnitKind::Scavenger) {
            fetch_step(units_, scraps_, ore_, unit);
        }
        if (!prof.mobile) {
            unit.vx = Fixed::zero();
            unit.vy = Fixed::zero();
            continue;
        }
        steer(units_, unit, prof, ground, route_plans_);
        if (unit.kind == UnitKind::Turret && !turret_may_move(unit)) {
            continue;
        }
        move(ground, prof, unit);
    }

    // 3. the bolts already in the air, in spawn order. Before the guns, so a unit killed here does
    //    not shoot this tick, and a bolt fired this tick has not moved yet.
    impacts_.clear();
    landings_.clear();
    advance_projectiles(projectiles_, units_, impacts_, landings_, seed);

    // 4. fire, ascending id. The shot leaves the gun now; its damage arrives with it.
    for (Unit& unit : units_) {
        if (!unit.alive) {
            continue;
        }
        const UnitProfile prof = profile(unit.kind);
        if (prof.weapon_range.raw <= 0 || prof.damage == 0) {
            continue;
        }
        if (unit.cooldown > 0) {
            --unit.cooldown;
        }
        if (unit.cooldown > 0 || !is_mounted(unit)) {
            continue; // reloading, or a turret that has not redeployed (the gun reloads meanwhile)
        }
        const std::int64_t range = prof.weapon_range.raw;
        Unit* best = nullptr;
        // What the trigger named outranks everything else. It is the only order that says "shoot
        // THIS one" without touching stance or movement, which is what lets a player who is driving
        // a unit choose a target without the sim steering there.
        if (unit.focus_target.valid()) {
            Unit* focused = find_mut(units_, unit.focus_target);
            if (focused != nullptr && focused->alive
                && dist2(unit.x, unit.y, focused->x, focused->y) <= range * range) {
                best = focused;
            }
        }
        // An explicit Attack order outranks auto-engagement: while the named target is alive, in
        // range and in sight the gun engages it, even when a closer hostile is in range too.
        if (unit.stance == Stance::Attack && unit.target.valid()) {
            Unit* named = find_mut(units_, unit.target);
            if (named != nullptr && named->alive
                && dist2(unit.x, unit.y, named->x, named->y) <= range * range
                && has_line(unit, *named, seed)) {
                best = named;
            }
        }
        if (best == nullptr) {
            std::int64_t best_d2 = range * range + 1;
            for (Unit& other : units_) {
                if (!other.alive || other.side == unit.side) {
                    continue;
                }
                const std::int64_t d2 = dist2(unit.x, unit.y, other.x, other.y);
                // A gun does not shoot a hill: without a line the enemy is not a target, and a
                // farther one it can see is.
                if (d2 <= range * range && d2 < best_d2 // strict: ties keep the lowest id
                    && has_line(unit, other, seed)) {
                    best = &other;
                    best_d2 = d2;
                }
            }
        }
        if (best == nullptr) {
            retarget_aim(unit, UnitId{}); // nothing to aim at: the belief goes with it
            continue;
        }
        retarget_aim(unit, best->id);
        projectiles_.push_back(launch(unit, *best, prof.damage, seed));
        shots_.push_back(Shot{unit.id, best->id});
        unit.cooldown = prof.cooldown_ticks;
        // The focus is spent by the shot it names: one click, one bolt. Whoever it was fired at,
        // the choice of target does not persist; auto-engagement resumes on the next chance.
        if (unit.focus_target.valid() && best->id == unit.focus_target) {
            unit.focus_target = UnitId{};
        }
    }

    // 5. repair, ascending id. AFTER movement and fire on purpose: a unit shot dead this tick is
    //    already !alive and is skipped, so repair never undoes a kill, and the +1 lands on the hp
    //    the tick's damage left. `tick_ % interval` keeps the beat global: no per-unit state.
    if (tick_ % static_cast<std::uint64_t>(kRepairTickInterval) == 0) {
        const std::int64_t radius_raw = static_cast<std::int64_t>(kRepairRadiusTiles) << 16;
        for (Unit& unit : units_) {
            if (!unit.alive || unit.kind == UnitKind::Factory) {
                continue;
            }
            const std::uint16_t max_hp = profile(unit.kind).hp;
            if (unit.hp >= max_hp) {
                continue;
            }
            for (const Unit& factory : units_) {
                if (factory.alive && factory.kind == UnitKind::Factory && factory.side == unit.side
                    && dist2(unit.x, unit.y, factory.x, factory.y) <= radius_raw * radius_raw) {
                    ++unit.hp;
                    break;
                }
            }
        }
    }

    // 6. THE WRECKS (C1). Every unit this tick's damage killed leaves scrap on the ground. The dead
    //    are read off `impacts_` -- an impact reports the victim's hit points AFTER the hit, so
    //    hp == 0 is exactly a kill -- and scattered in ASCENDING VICTIM ID, so the litter's order
    //    (and so the state hash) is a function of the battle, not of the order the bolts flew in.
    //    A dead unit is never hit again in the same step, so one victim means one wreck.
    if (!impacts_.empty()) {
        std::vector<UnitId> wrecked;
        for (const Impact& impact : impacts_) {
            if (impact.victim_hp == 0) {
                wrecked.push_back(impact.victim);
            }
        }
        std::sort(wrecked.begin(), wrecked.end());
        wrecked.erase(std::unique(wrecked.begin(), wrecked.end()), wrecked.end());
        for (const UnitId id : wrecked) {
            const Unit* dead = find_const(units_, id);
            if (dead != nullptr) {
                const std::size_t first = scraps_.size();
                scatter(scraps_, scenario_.seed, id, dead->x, dead->y);
                // The pieces land where the hash says; then they settle where a hauler can reach
                // them (and where a second piece of the same wreck is not already lying).
                settle_litter(scraps_, first, scenario_.seed);
            }
        }
    }
    // 7. THE BUILD (C4). On the production beat, a living factory replaces a lost GUN: a side whose
    //    living armed units are fewer than the armed units it started with, and which can afford
    //    it, gets a tank -- built at the factory, so a base under siege builds into the siege. The
    //    kind it lost is not remembered: a tank is what a factory makes (the only `buildable`
    //    kind). Ore is spent here and nowhere else: repair, repairs and replenishment are always
    //    free, whatever the ledger says.
    if (tick_ % static_cast<std::uint64_t>(kBuildIntervalTicks) == 0) {
        // Counted before the build, so one factory's new tank cannot make another factory think the
        // side is up to strength (or the other way round).
        std::array<int, 2> armed_started{};
        std::array<int, 2> armed_living{};
        for (const Spawn& spawn : scenario_.spawns) {
            if (armed_kind(spawn.kind)) {
                ++armed_started.at(static_cast<std::size_t>(spawn.side));
            }
        }
        for (const Unit& unit : units_) {
            if (unit.alive && armed_kind(unit.kind)) {
                ++armed_living.at(static_cast<std::size_t>(unit.side));
            }
        }
        for (std::size_t i = 0; i < units_.size(); ++i) {
            const Unit& factory = units_.at(i);
            if (!factory.alive || factory.kind != UnitKind::Factory) {
                continue;
            }
            const auto side = static_cast<std::size_t>(factory.side);
            if (!short_and_affordable(armed_living.at(side), armed_started.at(side),
                                      ore_.at(side))) {
                continue;
            }
            const Coord spot = builder_spot(scenario_.seed, units_, factory.x, factory.y);
            if (spot.x < 0) {
                continue; // nowhere to put it (the yard is full): no build, and no ore spent
            }
            units_.push_back(fresh_unit(next_id_++, UnitKind::Tank, factory.side, spot));
            ore_.at(side) -= kTankOreCost;
            ++armed_living.at(side);
            // One build per beat, and `units_` has just moved: the loop's indices are stale, so it
            // ends here. The next beat builds again if the side is still short.
            break;
        }
    }
    ++tick_;
}

std::uint32_t Sim::tick() const noexcept { return tick_; }

std::uint32_t Sim::route_plans() const noexcept { return route_plans_; }

bool Sim::kind_wiped_out(Side side, UnitKind kind) const noexcept {
    bool had_one = false;
    for (const Unit& unit : units_) {
        if (unit.side != side || unit.kind != kind) {
            continue;
        }
        if (unit.alive) {
            return false;
        }
        had_one = true; // a dead unit stays in the table, so "had one" needs no second record
    }
    return had_one;
}

WarOutcome Sim::war_outcome() const noexcept {
    // The player's loss is checked first, so a recycler pair that dies together reads as a loss.
    if (kind_wiped_out(Side::Player, UnitKind::Recycler)) {
        return WarOutcome::PlayerLost;
    }
    if (kind_wiped_out(Side::Enemy, UnitKind::Recycler)) {
        return WarOutcome::PlayerWon;
    }
    return WarOutcome::Undecided;
}

std::uint64_t Sim::seed() const noexcept { return scenario_.seed; }

bool tile_passable(std::uint64_t seed, int tile_x, int tile_y) noexcept {
    return tile_x >= 0 && tile_y >= 0 && tile_x < kMapTiles && tile_y < kMapTiles
           && terrain_speed_factor(world::sample(seed, tile_x, tile_y).terrain).raw != 0;
}

const std::vector<Unit>& Sim::units() const noexcept { return units_; }

const Unit* Sim::find(UnitId id) const noexcept { return find_const(units_, id); }

const std::vector<Projectile>& Sim::projectiles() const noexcept { return projectiles_; }

const std::vector<Scrap>& Sim::scraps() const noexcept { return scraps_; }

std::uint16_t Sim::ore(Side side) const noexcept { return ore_.at(static_cast<std::size_t>(side)); }

const std::vector<Impact>& Sim::impacts() const noexcept { return impacts_; }

const std::vector<Sim::Shot>& Sim::shots() const noexcept { return shots_; }

const std::vector<Landing>& Sim::landings() const noexcept { return landings_; }

const std::vector<Command>& Sim::command_log() const noexcept { return log_; }

std::uint64_t Sim::state_hash() const noexcept {
    Hasher h;
    h.u64(scenario_.seed);
    h.u64(tick_);
    h.u64(next_id_);
    h.u64(units_.size());
    for (const Unit& u : units_) {
        h.u64(u.id.value);
        h.u64(static_cast<std::uint64_t>(u.kind));
        h.u64(static_cast<std::uint64_t>(u.side));
        h.i32(u.x.raw);
        h.i32(u.y.raw);
        h.i32(u.vx.raw);
        h.i32(u.vy.raw);
        h.i32(static_cast<std::int32_t>(u.detour.dx));
        h.i32(static_cast<std::int32_t>(u.detour.dy));
        h.u64(u.detour.ticks_left);
        h.u64(u.hp);
        h.u64(u.cooldown);
        h.u64(u.still_ticks);
        if (u.unmount_ticks != 0) {
            h.u64(u.unmount_ticks); // only when set, so a sim that never unmounts hashes as before
        }
        h.u64(static_cast<std::uint64_t>(u.stance));
        h.u64(u.target.value);
        h.u64(u.focus_target.value);
        h.i32(u.guard_x.raw);
        h.i32(u.guard_y.raw);
        h.i32(u.patrol_x.raw);
        h.i32(u.patrol_y.raw);
        h.i32(u.aim_x.raw);
        h.i32(u.aim_y.raw);
        h.u64(u.aim_target.value);
        h.u64(u.alive ? 1 : 0);
        if (u.cargo != 0) {
            h.u64(u.cargo); // only when set, like unmount_ticks: a haul-less sim hashes as before
        }
        // The route: the goal it serves, the leg it is on, and every waypoint.
        h.u64(u.route.has_goal ? 1 : 0);
        h.u64(u.route.partial ? 1 : 0);
        h.u64(u.route.goal.x);
        h.u64(u.route.goal.y);
        h.u64(u.route.from.x);
        h.u64(u.route.from.y);
        h.u64(u.route.age);
        h.u64(u.route.count);
        h.u64(u.route.next);
        for (std::size_t i = 0; i < u.route.count; ++i) {
            h.u64(u.route.points.at(i).x);
            h.u64(u.route.points.at(i).y);
        }
    }
    // Bolts in the air are state. An empty sky adds nothing to the stream, so a battle that never
    // fired a shot hashes exactly as it did before bolts existed.
    if (!projectiles_.empty()) {
        h.u64(projectiles_.size());
        for (const Projectile& p : projectiles_) {
            h.i32(p.x.raw);
            h.i32(p.y.raw);
            h.i32(p.z.raw);
            h.i32(p.vx.raw);
            h.i32(p.vy.raw);
            h.i32(p.vz.raw);
            h.u64(p.shooter.value);
            h.u64(static_cast<std::uint64_t>(p.side));
            h.u64(p.aimed_at.value);
            h.u64(p.damage);
            h.u64(p.age);
            h.u64(p.learns ? 1 : 0);
            h.u64(static_cast<std::uint64_t>(p.closest_d2));
            h.i32(p.miss_x.raw);
            h.i32(p.miss_y.raw);
            // The bouncer's state only for a bouncer, so a contact battle hashes as it always did.
            if (p.round != Round::Contact) {
                h.u64(static_cast<std::uint64_t>(p.round));
                h.u64(p.bounces);
                h.u64(p.resting ? 1 : 0);
                h.u64(p.rested);
                h.u64(p.bounced_off.value);
            }
            // Ballistics only when a bolt has any, so a straight single-target battle hashes as it
            // always did.
            if (p.gravity.raw != 0 || p.splash_radius.raw != 0
                || p.max_travel != kProjectileRange) {
                h.i32(p.gravity.raw);
                h.i32(p.splash_radius.raw);
                h.i32(p.max_travel.raw);
            }
        }
    }
    // The wreckage on the ground is state (C1). Bare ground adds nothing to the stream, so a battle
    // that has not lost a unit hashes exactly as it did before wrecks existed.
    if (!scraps_.empty()) {
        h.u64(scraps_.size());
        for (const Scrap& s : scraps_) {
            h.i32(s.x.raw);
            h.i32(s.y.raw);
        }
    }
    // The ledger (C3), for the same reason: a war that has delivered nothing hashes as it did
    // before ore existed.
    if (ore_.at(0) != 0 || ore_.at(1) != 0) {
        h.u64(ore_.at(0));
        h.u64(ore_.at(1));
    }
    // Orders accepted but not yet applied are part of the state: they will change the next tick.
    h.u64(pending_.size());
    for (const Order& o : pending_) {
        h.u64(static_cast<std::uint64_t>(o.kind()));
        h.u64(o.unit.value);
        // The hashed stream keeps its original shape -- x, y, x2, y2, target, zero where the kind
        // has none -- through the same reader the replay writer uses, so there is one definition of
        // a payload's contents rather than two.
        const OrderFields f = fields_of(o.payload);
        for (const Fixed position : f.positions) {
            h.i32(position.raw);
        }
        h.u64(f.target.value);
    }
    return h.value();
}

} // namespace protekt::sim
