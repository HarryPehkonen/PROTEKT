#include "protekt/sim/sim.hpp"

#include <algorithm>
#include <cstddef>

namespace protekt::sim {

UnitProfile profile(UnitKind kind) noexcept {
    switch (kind) {
    case UnitKind::Tank:
        return UnitProfile{kTankHp,
                           Fixed::from_raw(kTankSpeedRaw),
                           Fixed::from_raw(kTankRangeRaw),
                           kTankDamage,
                           kTankCooldownTicks,
                           true};
    case UnitKind::Turret:
        return UnitProfile{kTurretHp,     Fixed::zero(),        Fixed::from_raw(kTurretRangeRaw),
                           kTurretDamage, kTurretCooldownTicks, false};
    case UnitKind::Factory:
        return UnitProfile{kFactoryHp, Fixed::zero(), Fixed::zero(), 0, 0, false};
    }
    return UnitProfile{};
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

// floor(sqrt(n)) for n >= 0, by the bitwise method: no floating point, no loop-count surprises.
std::int64_t isqrt(std::int64_t n) noexcept {
    if (n <= 0) {
        return 0;
    }
    auto rest = static_cast<std::uint64_t>(n);
    std::uint64_t root = 0;
    std::uint64_t bit = std::uint64_t{1} << 62;
    while (bit > rest) {
        bit >>= 2;
    }
    while (bit != 0) {
        if (rest >= root + bit) {
            rest -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return static_cast<std::int64_t>(root);
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

bool order_is_valid(const std::vector<Unit>& units, const Order& order) noexcept {
    const Unit* unit = find_const(units, order.unit);
    if (unit == nullptr || !unit->alive) {
        return false;
    }
    if (order.kind == OrderKind::Attack) {
        const Unit* target = find_const(units, order.target);
        return target != nullptr && target->alive && target->side != unit->side;
    }
    return static_cast<int>(order.kind) < kOrderKindCount;
}

void apply_order(std::vector<Unit>& units, const Order& order) noexcept {
    // Re-validated here: the unit may have died between enqueue and the tick it takes effect.
    if (!order_is_valid(units, order)) {
        return;
    }
    Unit* unit = find_mut(units, order.unit);
    switch (order.kind) {
    case OrderKind::MoveTo:
    case OrderKind::GuardAt:
    case OrderKind::Patrol:
        unit->stance = order.kind == OrderKind::MoveTo    ? Stance::MoveTo
                       : order.kind == OrderKind::GuardAt ? Stance::GuardAt
                                                          : Stance::Patrol;
        unit->guard_x = clamp_pos(order.x);
        unit->guard_y = clamp_pos(order.y);
        unit->patrol_x = clamp_pos(order.x2);
        unit->patrol_y = clamp_pos(order.y2);
        unit->target = UnitId{};
        break;
    case OrderKind::Attack:
        unit->stance = Stance::Attack;
        unit->target = order.target;
        break;
    case OrderKind::Stop:
        unit->stance = Stance::Idle;
        unit->target = UnitId{};
        unit->vx = Fixed::zero();
        unit->vy = Fixed::zero();
        break;
    }
}

bool impassable(std::uint64_t seed, Fixed x, Fixed y) noexcept {
    const world::Tile tile = world::sample(seed, x.floor_int(), y.floor_int());
    return terrain_speed_factor(tile.terrain).raw == 0;
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
void steer(std::vector<Unit>& units, Unit& unit, const UnitProfile& prof) noexcept {
    unit.vx = Fixed::zero();
    unit.vy = Fixed::zero();
    Fixed goal_x = unit.guard_x;
    Fixed goal_y = unit.guard_y;
    std::int64_t stop_radius = kArriveRadiusRaw;
    switch (unit.stance) {
    case Stance::MoveTo:
    case Stance::GuardAt:
    case Stance::Patrol:
        break;
    case Stance::Attack: {
        const Unit* target = find_const(units, unit.target);
        if (target == nullptr || !target->alive) {
            unit.stance = Stance::Idle;
            unit.target = UnitId{};
            return;
        }
        goal_x = target->x;
        goal_y = target->y;
        if (prof.weapon_range.raw > 0) {
            stop_radius = prof.weapon_range.raw;
        }
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
    const std::int64_t dist = isqrt(d2); // >= stop_radius > 0
    const std::int64_t dx = static_cast<std::int64_t>(goal_x.raw) - unit.x.raw;
    const std::int64_t dy = static_cast<std::int64_t>(goal_y.raw) - unit.y.raw;
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

// Moves a unit by its velocity, scaled by the terrain it is entering. A move into a crater is
// retried along each axis separately (so a unit slides along a rim); failing that it stays.
void move(std::uint64_t seed, Unit& unit) noexcept {
    if (unit.vx.raw == 0 && unit.vy.raw == 0) {
        return;
    }
    const std::int64_t candidates[3][2]
        = {{unit.vx.raw, unit.vy.raw}, {unit.vx.raw, 0}, {0, unit.vy.raw}};
    for (const auto& candidate : candidates) {
        if (candidate[0] == 0 && candidate[1] == 0) {
            continue;
        }
        const Fixed full_x = clamp_pos(Fixed::from_raw(static_cast<std::int32_t>(
            std::clamp<std::int64_t>(unit.x.raw + candidate[0], 0, kMaxPosRaw))));
        const Fixed full_y = clamp_pos(Fixed::from_raw(static_cast<std::int32_t>(
            std::clamp<std::int64_t>(unit.y.raw + candidate[1], 0, kMaxPosRaw))));
        const world::Tile tile = world::sample(seed, full_x.floor_int(), full_y.floor_int());
        const std::int64_t factor = terrain_speed_factor(tile.terrain).raw;
        if (factor == 0) {
            continue;
        }
        const std::int64_t sx = (candidate[0] * factor) >> Fixed::kShift;
        const std::int64_t sy = (candidate[1] * factor) >> Fixed::kShift;
        const Fixed nx = Fixed::from_raw(
            static_cast<std::int32_t>(std::clamp<std::int64_t>(unit.x.raw + sx, 0, kMaxPosRaw)));
        const Fixed ny = Fixed::from_raw(
            static_cast<std::int32_t>(std::clamp<std::int64_t>(unit.y.raw + sy, 0, kMaxPosRaw)));
        if (impassable(seed, nx, ny)) {
            continue;
        }
        unit.x = nx;
        unit.y = ny;
        return;
    }
    unit.vx = Fixed::zero();
    unit.vy = Fixed::zero();
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
        const UnitProfile prof = profile(spawn.kind);
        Unit unit{};
        unit.id = UnitId{sim.next_id_++};
        unit.kind = spawn.kind;
        unit.side = spawn.side;
        // Out-of-range spawns are clamped onto the map rather than rejected: total, not UB.
        const Coord tile
            = nearest_passable(scenario.seed, std::clamp(spawn.tile_x, 0, kMapTiles - 1),
                               std::clamp(spawn.tile_y, 0, kMapTiles - 1));
        unit.x = Fixed::from_int(tile.x);
        unit.y = Fixed::from_int(tile.y);
        unit.hp = prof.hp;
        unit.cooldown = prof.cooldown_ticks; // a fresh gun starts reloading
        unit.stance = Stance::Idle;
        unit.alive = true;
        sim.units_.push_back(unit);
    }
    return sim;
}

void Sim::enqueue(const Order& order) {
    // OrderSink is the human commander's channel: it commands the Player side only.
    const Unit* unit = find_const(units_, order.unit);
    if (!order_is_valid(units_, order) || unit->side != Side::Player) {
        return;
    }
    pending_.push_back(order);
    log_.push_back(Command{tick_, order});
}

void Sim::step() {
    // 1. orders, in the order enqueued.
    for (const Order& order : pending_) {
        apply_order(units_, order);
    }
    pending_.clear();

    // 2. movement, ascending id (units_ is stored in id order).
    const std::uint64_t seed = scenario_.seed;
    for (Unit& unit : units_) {
        if (!unit.alive) {
            continue;
        }
        const UnitProfile prof = profile(unit.kind);
        if (!prof.mobile) {
            unit.vx = Fixed::zero();
            unit.vy = Fixed::zero();
            continue;
        }
        steer(units_, unit, prof);
        move(seed, unit);
    }

    // 3. fire, ascending id. Damage lands immediately, so a unit killed earlier in this phase
    //    does not shoot back this tick.
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
        if (unit.cooldown > 0) {
            continue;
        }
        const std::int64_t range = prof.weapon_range.raw;
        Unit* best = nullptr;
        // An explicit Attack order outranks auto-engagement: while the named target is alive and
        // in range the gun engages it, even when a closer hostile is in range too.
        if (unit.stance == Stance::Attack && unit.target.valid()) {
            Unit* named = find_mut(units_, unit.target);
            if (named != nullptr && named->alive && named->side != unit.side
                && dist2(unit.x, unit.y, named->x, named->y) <= range * range) {
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
                if (d2 <= range * range && d2 < best_d2) { // strict: ties keep the lowest id
                    best = &other;
                    best_d2 = d2;
                }
            }
        }
        if (best == nullptr) {
            continue;
        }
        best->hp = best->hp > prof.damage ? static_cast<std::uint16_t>(best->hp - prof.damage)
                                          : std::uint16_t{0};
        if (best->hp == 0) {
            best->alive = false;
            best->stance = Stance::Dead;
            best->vx = Fixed::zero();
            best->vy = Fixed::zero();
        }
        unit.cooldown = prof.cooldown_ticks;
    }

    // 4.
    ++tick_;
}

std::uint32_t Sim::tick() const noexcept { return tick_; }

const std::vector<Unit>& Sim::units() const noexcept { return units_; }

const Unit* Sim::find(UnitId id) const noexcept { return find_const(units_, id); }

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
        h.u64(u.hp);
        h.u64(u.cooldown);
        h.u64(static_cast<std::uint64_t>(u.stance));
        h.u64(u.target.value);
        h.i32(u.guard_x.raw);
        h.i32(u.guard_y.raw);
        h.i32(u.patrol_x.raw);
        h.i32(u.patrol_y.raw);
        h.u64(u.alive ? 1 : 0);
    }
    // Orders accepted but not yet applied are part of the state: they will change the next tick.
    h.u64(pending_.size());
    for (const Order& o : pending_) {
        h.u64(static_cast<std::uint64_t>(o.kind));
        h.u64(o.unit.value);
        h.i32(o.x.raw);
        h.i32(o.y.raw);
        h.i32(o.x2.raw);
        h.i32(o.y2.raw);
        h.u64(o.target.value);
    }
    return h.value();
}

} // namespace protekt::sim
