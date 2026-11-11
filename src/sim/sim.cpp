#include "protekt/sim/sim.hpp"

#include "protekt/rng.hpp"
#include "protekt/sim/spec.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <optional>
#include <span>
#include <type_traits>
#include <variant>

namespace protekt::sim {

UnitProfile profile(UnitKind kind) noexcept {
    if (static_cast<std::size_t>(kind) >= kUnitSpecs.size()) {
        return UnitProfile{};
    }
    const UnitSpec& spec = spec_of(kind);
    return UnitProfile{spec.hp,       spec.max_speed,         spec.range(),         spec.damage,
                       spec.cooldown, spec.max_speed.raw > 0, spec.turn_decidegrees};
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
    if (const auto* defend = std::get_if<orders::Defend>(&order.payload)) {
        return &defend->target; // chosen by the issuer, as an attack's is: the same rule again
    }
    if (const auto* repair = std::get_if<orders::Repair>(&order.payload)) {
        return &repair->target; // living and never the truck itself, as the rest are
    }
    return nullptr;
}

/// Whether `queue` takes one more entry of `kind` under `batch`: a QUEUE IS ONE BATCH (Harri,
/// 2026-10-28). An empty queue takes anything, so a batch's first entry fixes its kind and its
/// token; after that only the same kind under the same token joins, up to the cap -- which is the
/// batch's own, not a pool shared with any other. A lone build's id lies above every token an order
/// can carry, so no order matches it: it closes the queue to everything until it is done.
bool build_fits(const BuildQueue& queue, UnitKind kind, std::uint32_t batch) noexcept {
    if (queue.count == 0) {
        return true;
    }
    const BuildEntry& front = queue.entries.at(0);
    return queue.count < kBuildQueueDepth && front.kind == kind && front.batch == batch;
}

bool order_is_valid(const std::vector<Unit>& units, const std::vector<Pool>& pools,
                    const Order& order, std::uint64_t seed) noexcept {
    const Unit* unit = find_const(units, order.unit);
    if (unit == nullptr || !unit->alive) {
        return false;
    }
    // A DEPLOYED SCAVENGER CAN NEVER BE UN-DEPLOYED (Harri, 2026-10-31): it takes no order at all,
    // so there is no recall, no move and no second deploy. Its 20 is committed while it lives.
    if (unit->kind == UnitKind::DeployedScavenger) {
        return false;
    }
    if (const auto* deploy = std::get_if<orders::Deploy>(&order.payload)) {
        // A recycler deploys onto a site it can stand on; a scavenger onto a biometal POOL no other
        // deployed scavenger holds (ONE PER POOL, either side's). Nothing else deploys -- which is
        // also what refuses a second deploy, since the first changed the kind.
        if (unit->kind == UnitKind::Scavenger) {
            const Pool* pool = pool_at(pools, deploy->tile_x, deploy->tile_y);
            return pool != nullptr && !pool_taken(units, *pool);
        }
        if (is_turret(unit->kind)) {
            // A turret deploys wherever it can stand: a spot, not a footprint.
            return tile_passable(seed, deploy->tile_x, deploy->tile_y)
                   && !building_blocks(units, deploy->tile_x, deploy->tile_y);
        }
        return unit->kind == UnitKind::Recycler
               && deploy_site_ok(seed, units, deploy->tile_x, deploy->tile_y);
    }
    // UN-DEPLOY is a deployed turret's alone (a plain turret has nothing to take down, and a
    // deployed scavenger takes no order at all, above).
    if (std::holds_alternative<orders::Undeploy>(order.payload)) {
        return unit->kind == UnitKind::DeployedTurret;
    }
    // DEFEND is a turret's, plain or deployed, and needs somebody to defend against: the generic
    // target rule below (a living unit, never itself) is the rest of it.
    if (std::holds_alternative<orders::Defend>(order.payload) && !is_turret(unit->kind)) {
        return false;
    }
    // REPAIR is a truck's, and only for a patient of its own side: a truck mends what is its
    // side's, and the generic target rule below is the rest of it (living, never itself).
    if (const auto* repair = std::get_if<orders::Repair>(&order.payload)) {
        const Unit* patient = find_const(units, repair->target);
        if (unit->kind != UnitKind::ServiceTruck || patient == nullptr
            || patient->side != unit->side) {
            return false;
        }
    }
    if (const auto* build = std::get_if<orders::Build>(&order.payload)) {
        // A BUILD goes to a producer, by KIND (a side may have several), for a kind that has a
        // price and a time, under a token an order can carry, and only into a queue that is empty
        // or already holds this very batch (`build_fits`). The same check runs when the order is
        // applied; `Sim::enqueue` additionally weighs the builds already pending this tick.
        return is_producer(unit->kind) && is_queueable(build->kind)
               && build->batch <= kMaxBatchToken
               && build_fits(unit->queue, build->kind, build->batch);
    }
    // CANCELLING a queue is a producer's, whatever the queue holds (an empty one takes it and is
    // unchanged), so an order laid down in the same tick as a build is never judged against a
    // queue it has not seen yet.
    if (std::holds_alternative<orders::CancelBuild>(order.payload)) {
        return is_producer(unit->kind);
    }
    if (const auto* recycle = std::get_if<orders::Recycle>(&order.payload)) {
        // A RECYCLE goes into a LIVING recycler of the unit's OWN side: the base is the side's, and
        // recycling into the enemy's would hand it the scrap. A unit may not name itself (the
        // recycler cannot recycle itself), and the target must be a recycler kind -- the vehicle or
        // the building, since "the recycler" means the base either way.
        const Unit* target = find_const(units, recycle->recycler);
        return target != nullptr && target->alive && target->id != unit->id
               && target->side == unit->side
               && (target->kind == UnitKind::Recycler
                   || target->kind == UnitKind::DeployedRecycler);
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

/// The world as the mover sees it: one question, answered from the seed and from the buildings
/// standing on it. A designed map answers the same question from its own tiles, and a route planner
/// asks it many times, without the sim caring. A building's footprint cannot be entered, exactly as
/// a crater cannot (`building_blocks`); the terrain's own answer is unchanged.
struct WorldGround {
    std::uint64_t seed;
    std::span<const Unit> units;
    /// Whether any unit in `units` stands on a footprint. While none does -- every battle until
    /// something deploys -- `building_blocks` is "no" for every tile, and asking it per tile anyway
    /// cost the route planner a fifth of its time (protekt-measure 2400: 99.6 s without the
    /// question, 122.2 s asking it of every tile). `ground_of` reads it off the units.
    bool buildings;
    [[nodiscard]] std::int64_t factor(int tile_x, int tile_y) const noexcept {
        if (buildings && building_blocks(units, tile_x, tile_y)) {
            return 0;
        }
        const world::Tile tile = world::sample(seed, tile_x, tile_y);
        return terrain_speed_factor(tile.terrain).raw;
    }
};

/// The ground as the mover sees it now, with `WorldGround::buildings` read off the units.
[[nodiscard]] WorldGround ground_of(std::uint64_t seed, std::span<const Unit> units) noexcept {
    const bool buildings = std::any_of(units.begin(), units.end(), [](const Unit& unit) {
        return unit.alive && has_footprint(unit.kind);
    });
    return WorldGround{seed, units, buildings};
}

/// Where a position-bearing order sends the unit; unused points are the origin, as they always
/// were.
/// The middle of a whole tile: where a scavenger stands on its pool.
[[nodiscard]] Fixed tile_centre_of(int tile) noexcept {
    return fixed_add(Fixed::from_int(tile), Fixed::from_raw(Fixed::kOne / 2));
}

void apply_posted(Unit& unit, Stance stance, Fixed x, Fixed y, Fixed x2, Fixed y2) noexcept {
    unit.stance = stance;
    unit.guard_x = clamp_pos(x);
    unit.guard_y = clamp_pos(y);
    unit.patrol_x = clamp_pos(x2);
    unit.patrol_y = clamp_pos(y2);
    unit.target = UnitId{};
    unit.route.age = kRouteAgeFresh; // a new goal re-plans at once; the same goal keeps its route
    unit.fetch_beats = 0; // a new order is a new trip: nothing carries over from the old one
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
void apply_strafe(Unit& unit, const orders::Strafe& strafe, const WorldGround& ground) noexcept {
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
    const StepPlan plan = plan_step(request, ground);
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
                 std::vector<Sim::Shot>& shots, UnitId& driven, std::uint32_t& lone_batches,
                 std::array<std::uint16_t, 2>& ore, const std::vector<Pool>& pools,
                 const Order& order, std::uint64_t seed) noexcept {
    // Re-validated here: the unit may have died between enqueue and the tick it takes effect.
    if (!order_is_valid(units, pools, order, seed)) {
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
            if (scoots(unit.kind) && unit.stance != Stance::Attack) {
                // No spot is pending when an engagement begins: it holds the ground it stands on
                // until its first shot picks the next. (A re-issued Attack keeps its spot.)
                unit.guard_x = unit.x;
                unit.guard_y = unit.y;
            }
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
            apply_strafe(unit, *o, ground_of(seed, units));
        }
        break;
    case OrderKind::Recycle:
        if (const auto* o = std::get_if<orders::Recycle>(&order.payload)) {
            // Drive to the recycler's CURRENT position; `recycle_step` re-aims every tick, so a
            // recycler still on wheels is followed rather than chased to where it used to be.
            const Unit* target = find_const(units, o->recycler);
            if (target != nullptr) {
                unit.stance = Stance::Recycle;
                unit.target = o->recycler;
                unit.guard_x = target->x;
                unit.guard_y = target->y;
                unit.route.age = kRouteAgeFresh;
            }
        }
        break;
    case OrderKind::Deploy:
        if (const auto* o = std::get_if<orders::Deploy>(&order.payload)) {
            // Off to the site's centre, as a MoveTo would go; the countdown starts once it is there
            // (`deploy_step`). A new deploy order starts the count again from nothing.
            if (is_turret(unit.kind)) {
                // A turret goes to the middle of the tile it was pointed at. A DEPLOYED one keeps
                // its `deploy_ticks`, which count its un-deploy: the order is "take it down, go
                // there, put it up", and the first leg must not lose its progress.
                apply_posted(unit, Stance::Deploy, tile_centre_of(o->tile_x),
                             tile_centre_of(o->tile_y), Fixed{}, Fixed{});
                if (unit.kind == UnitKind::Turret) {
                    unit.deploy_ticks = 0;
                }
                break;
            }
            if (unit.kind == UnitKind::Scavenger) {
                // A scavenger goes to the middle of its pool's tile.
                apply_posted(unit, Stance::Deploy, tile_centre_of(o->tile_x),
                             tile_centre_of(o->tile_y), Fixed{}, Fixed{});
                unit.deploy_ticks = 0;
                break;
            }
            const Footprint site = footprint_at(o->tile_x, o->tile_y);
            apply_posted(unit, Stance::Deploy, Fixed::from_int(site.centre_x()),
                         Fixed::from_int(site.centre_y()), Fixed{}, Fixed{});
            unit.deploy_ticks = 0;
        }
        break;
    case OrderKind::FireLob:
        if (const auto* o = std::get_if<orders::FireLob>(&order.payload)) {
            // The player's mortar: FireTowards' rules (armed, reloaded, mounted, no naming
            // oneself, no Fired event) with the MORTAR's shell, damage and reload.
            const UnitProfile prof = profile(unit.kind);
            const UnitProfile shell = profile(UnitKind::MortarBike);
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
            // direction and nowhere else. `aimed_at` only says whose miss the bolt measures.
            // A unit may not name itself: its own bolt starts on top of it.
            const UnitProfile prof = profile(unit.kind);
            if (prof.weapon_range.raw > 0 && prof.damage > 0 && unit.cooldown == 0
                && is_mounted(unit)) {
                const UnitId aimed_at = o->aimed_at == unit.id ? UnitId{} : o->aimed_at;
                fan_out(
                    launch_towards(unit, o->dir_x, o->dir_y, o->dir_z, aimed_at, prof.damage, seed),
                    spec_of(unit.kind).pellets, spec_of(unit.kind).cone_half_tan, projectiles);
                // The rule: the sim reports `Fired` for the shots it decided to make (gun fire); a
                // player's manual shot is the app's to announce, with its own sound. So the Shot is
                // recorded with NO target whatever `aimed_at` says -- a valid target here is what
                // the observer turns into a Fired event, and the player would hear it twice.
                shots.push_back(Sim::Shot{unit.id, UnitId{}});
                unit.cooldown = prof.cooldown_ticks;
            }
        }
        break;
    case OrderKind::FireFan:
        if (const auto* o = std::get_if<orders::FireFan>(&order.payload)) {
            // THE SHOTGUN'S TRIGGER (A): FireTowards' rules -- armed, mounted, reloaded, no naming
            // oneself, no Fired event -- with the WEAPON's own numbers from `kShotgunSpec`: its
            // per-pellet damage, its pellet count and cone, its flat flight and its tripled reload.
            // The order carries the aim only; the balance numbers are the spec row's, so retuning
            // the gun does not change what a recorded battle meant (as with FireLob and the
            // mortar).
            const UnitSpec& gun = kShotgunSpec;
            if (gun.damage > 0 && gun.pellets > 1 && unit.cooldown == 0 && is_mounted(unit)) {
                const UnitId aimed_at = o->aimed_at == unit.id ? UnitId{} : o->aimed_at;
                fan_out(launch_towards(unit, o->dir_x, o->dir_y, o->dir_z, aimed_at, gun.damage,
                                       seed, gun),
                        gun.pellets, gun.cone_half_tan, projectiles);
                shots.push_back(Sim::Shot{unit.id, UnitId{}});
                unit.cooldown = gun.cooldown;
            }
        }
        break;
    case OrderKind::FireRocket:
        if (const auto* o = std::get_if<orders::FireRocket>(&order.payload)) {
            // THE ROCKET'S TRIGGER (B), FireFan's rule for a single bolt: armed, mounted, reloaded,
            // no naming oneself, no Fired event, and the WEAPON's own numbers from `kRocketSpec` --
            // its long reach, its triple damage, its quintuple reload and its three-quarter bolt
            // speed. ONE bolt: `pellets` is zero, so there is no fan to spread (the hull's gun
            // fires through FireTowards, which still reads the shooter's kind).
            const UnitSpec& gun = kRocketSpec;
            if (gun.range().raw > 0 && gun.damage > 0 && unit.cooldown == 0 && is_mounted(unit)) {
                const UnitId aimed_at = o->aimed_at == unit.id ? UnitId{} : o->aimed_at;
                projectiles.push_back(launch_towards(unit, o->dir_x, o->dir_y, o->dir_z, aimed_at,
                                                     gun.damage, seed, gun));
                shots.push_back(Sim::Shot{unit.id, UnitId{}});
                unit.cooldown = gun.cooldown;
            }
        }
        break;
    case OrderKind::Build:
        if (const auto* o = std::get_if<orders::Build>(&order.payload)) {
            // Joins the back of the producer's queue. A lone build (batch 0) takes the next id of
            // the sim's own, above every token an order can carry; a shift run keeps its token.
            const std::uint32_t batch = o->batch != 0 ? o->batch : kFirstLoneBatch + lone_batches++;
            unit.queue.entries.at(unit.queue.count) = BuildEntry{o->kind, batch};
            ++unit.queue.count;
        }
        break;
    case OrderKind::CancelBuild:
        // The rest of the queue goes, the entry under way included. That entry paid its whole cost
        // on the tick it started (`production_step`), and nothing of it was built, so the cost
        // comes back; an entry still waiting for scrap paid nothing and returns nothing. The
        // units the queue already delivered are units like any other and stay. Step 6b clamps a
        // bank the refund pushed past its store.
        if (unit.queue.count > 0 && unit.queue.left > 0) {
            std::uint16_t& bank = ore.at(static_cast<std::size_t>(unit.side));
            bank = static_cast<std::uint16_t>(bank + spec_of(unit.queue.entries.at(0).kind).cost);
        }
        unit.queue = BuildQueue{};
        break;
    case OrderKind::Undeploy:
        // The count itself is `turret_deploy_step`'s; the order only says "take it down".
        unit.stance = Stance::Undeploy;
        unit.target = UnitId{};
        break;
    case OrderKind::Defend:
        if (const auto* o = std::get_if<orders::Defend>(&order.payload)) {
            unit.stance = Stance::Defend;
            unit.target = o->target;
            unit.route.age = kRouteAgeFresh;
            if (unit.kind == UnitKind::Turret) {
                unit.deploy_ticks = 0; // a deployed turret's count is its un-deploy: keep it
            }
        }
        break;
    case OrderKind::Repair:
        if (const auto* o = std::get_if<orders::Repair>(&order.payload)) {
            unit.stance = Stance::Repair;
            unit.target = o->target;
            unit.route.age = kRouteAgeFresh;
        }
        break;
    case OrderKind::TakeControl:
        // THE PLAYER TAKES THE HELM: one slot, so this unit becomes the driven one and the last
        // one (if any) is released. Nothing else about the unit changes -- no stance, no
        // destination, no velocity -- because the player steers it every tick.
        driven = order.unit;
        break;
    }
}

struct Coord {
    int x;
    int y;
};

// The tile itself when `ok` accepts it; otherwise the first tile `ok` accepts scanning square rings
// of growing radius in row-major order. The wish itself when nothing anywhere is acceptable.
template <class Ok> Coord nearest_where(int x, int y, Ok ok) noexcept {
    if (ok(x, y)) {
        return Coord{x, y};
    }
    for (int r = 1; r < kMapTiles; ++r) {
        for (int dy = -r; dy <= r; ++dy) {
            const int step = (dy == -r || dy == r) ? 1 : 2 * r;
            for (int dx = -r; dx <= r; dx += step) {
                if (ok(x + dx, y + dy)) {
                    return Coord{x + dx, y + dy};
                }
            }
        }
    }
    return Coord{x, y};
}

// A tile a unit may stand on: passable terrain (`tile_passable`, which is on the map and not a
// crater) and no building's footprint (`building_blocks`).
bool standable(std::uint64_t seed, std::span<const Unit> units, int tx, int ty) noexcept {
    return tile_passable(seed, tx, ty) && !building_blocks(units, tx, ty);
}

/// The sim's own RNG stream for a scoot's first-tried hop: world 0, sim 1, ai 2, scenario 3 and the
/// pool's 4 to 6 are taken elsewhere.
constexpr std::uint64_t kScootStream = 7;

/// cos and sin, Q16.16, of every sector's angle (`kScootSectors` of them, 22.5 degrees apart). A
/// table, because the sim has no trigonometry.
constexpr std::array<std::array<std::int32_t, 2>, kScootSectors> kSectorDirections{{
    {65536, 0},
    {60547, 25080},
    {46341, 46341},
    {25080, 60547},
    {0, 65536},
    {-25080, 60547},
    {-46341, 46341},
    {-60547, 25080},
    {-65536, 0},
    {-60547, -25080},
    {-46341, -46341},
    {-25080, -60547},
    {0, -65536},
    {25080, -60547},
    {46341, -46341},
    {60547, -25080},
}};

// The nearest tile a unit may stand on (`standable`), so a unit never starts inside a crater or a
// building.
Coord nearest_passable(std::uint64_t seed, std::span<const Unit> units, int x, int y) noexcept {
    return nearest_where(x, y,
                         [seed, units](int tx, int ty) { return standable(seed, units, tx, ty); });
}

// Whether a building may be PLACED with its north-west tile here: the site is one a deploy could
// use (`deploy_site_ok`), and no living unit is standing inside it -- a building is never spawned
// on top of a unit, as a unit is never spawned inside a building.
bool building_fits(std::uint64_t seed, std::span<const Unit> units, int tx, int ty) noexcept {
    if (!deploy_site_ok(seed, units, tx, ty)) {
        return false;
    }
    const Footprint site = footprint_at(tx, ty);
    return std::none_of(units.begin(), units.end(), [&site](const Unit& unit) {
        return unit.alive && site.covers(unit.x.floor_int(), unit.y.floor_int());
    });
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

} // namespace

std::optional<ScootSpot> scoot_spot(std::uint64_t seed, std::span<const Unit> units,
                                    const Unit& bike, const Unit& target, Tick tick) noexcept {
    const std::int64_t range = spec_of(bike.kind).range().raw;
    const std::int64_t reach = range - kScootMarginRaw;
    if (reach <= 0) {
        return std::nullopt;
    }
    // The sector the bike stands in, seen from the target: the direction its offset points most
    // nearly along, the lower sector on a tie.
    const std::int64_t ox = static_cast<std::int64_t>(bike.x.raw) - target.x.raw;
    const std::int64_t oy = static_cast<std::int64_t>(bike.y.raw) - target.y.raw;
    std::size_t here = 0;
    std::int64_t best_dot = 0;
    for (std::size_t k = 0; k < kSectorDirections.size(); ++k) {
        const std::int64_t dot
            = ox * kSectorDirections.at(k).at(0) + oy * kSectorDirections.at(k).at(1);
        if (k == 0 || dot > best_dot) {
            here = k;
            best_dot = dot;
        }
    }
    rng::SplitMix64 mix(rng::stream_seed(seed, kScootStream)
                        ^ (static_cast<std::uint64_t>(bike.id.value) << 32)
                        ^ static_cast<std::uint64_t>(tick));
    const std::size_t first = static_cast<std::size_t>(mix.next() % kScootSectorSteps.size());
    for (std::size_t i = 0; i < kScootSectorSteps.size(); ++i) {
        const int step = kScootSectorSteps.at((first + i) % kScootSectorSteps.size());
        const std::size_t sector
            = (here + static_cast<std::size_t>(kScootSectors + step)) % kScootSectors;
        const std::int64_t sx
            = target.x.raw + (reach * kSectorDirections.at(sector).at(0) >> Fixed::kShift);
        const std::int64_t sy
            = target.y.raw + (reach * kSectorDirections.at(sector).at(1) >> Fixed::kShift);
        const int tx = static_cast<int>(sx >> Fixed::kShift);
        const int ty = static_cast<int>(sy >> Fixed::kShift);
        if (!standable(seed, units, tx, ty)) {
            continue; // off the map, a crater or a building: the next hop
        }
        const Fixed cx = tile_center(static_cast<std::uint8_t>(tx));
        const Fixed cy = tile_center(static_cast<std::uint8_t>(ty));
        if (dist2(cx, cy, target.x, target.y) > range * range) {
            continue; // snapped to the tile's middle it would be out of range: never relocate there
        }
        return ScootSpot{cx, cy};
    }
    return std::nullopt;
}

std::optional<ScootSpot> firing_spot(std::uint64_t seed, std::span<const Unit> units,
                                     const Unit& shooter, const Unit& target) noexcept {
    const std::int64_t range = profile(shooter.kind).weapon_range.raw;
    if (!profile(shooter.kind).mobile || range <= 0) {
        return std::nullopt;
    }
    const int home_x = shooter.x.raw >> Fixed::kShift;
    const int home_y = shooter.y.raw >> Fixed::kShift;
    std::optional<ScootSpot> best{};
    std::int64_t best_d2 = 0;
    for (int ty = home_y - kFiringSpotReachTiles; ty <= home_y + kFiringSpotReachTiles; ++ty) {
        for (int tx = home_x - kFiringSpotReachTiles; tx <= home_x + kFiringSpotReachTiles; ++tx) {
            if (!standable(seed, units, tx, ty)) {
                continue;
            }
            const Fixed cx = tile_center(static_cast<std::uint8_t>(tx));
            const Fixed cy = tile_center(static_cast<std::uint8_t>(ty));
            const std::int64_t d2 = dist2(shooter.x, shooter.y, cx, cy);
            // Strict: ties keep the first found. The line is the dear question, so it is asked
            // last, and only of a tile that would beat the best so far.
            if ((best && d2 >= best_d2) || dist2(cx, cy, target.x, target.y) > range * range
                || !has_line_from(shooter, cx, cy, target, seed)) {
                continue;
            }
            best = ScootSpot{cx, cy};
            best_d2 = d2;
        }
    }
    return best;
}

namespace {

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

/// Where a factory's new tank appears: the factory's own tile when nothing is standing on it,
/// otherwise the first free passable tile on growing square rings, to `kBuildSpawnRadiusTiles`.
/// `{-1, -1}` means the yard is full: no build, and no ore spent.
Coord builder_spot(std::uint64_t seed, const std::vector<Unit>& units, Fixed fx,
                   Fixed fy) noexcept {
    const TileXY home = tile_of(fx, fy);
    const auto free_tile = [seed, &units](int tx, int ty) {
        if (!standable(seed, units, tx, ty)) {
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
/// a producer's finished build. The tile is a whole tile, as a spawn's is.
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

/// THE QUEUE (BUILD-QUEUE). One producer's line for one tick: the front entry waits for its scrap,
/// pays the whole cost on the tick it starts, counts its build time down, and on the tick the count
/// ends its unit appears beside the producer and the entry leaves the queue.
///
/// SCRAP SHORT PAUSES; it never cancels. A front entry the bank cannot pay for holds exactly where
/// it is -- nothing spent, nothing counted -- and starts on the first tick the bank can. A producer
/// that has died builds nothing and charges nothing (what its current entry paid is gone with it).
/// The spot is `builder_spot`'s, the factory's own placer, so a unit is never put inside a
/// building's footprint; a yard with no room holds the finished unit and tries again next tick.
///
/// Takes the producer by INDEX: the arrival appends to `units`, which invalidates a reference.
void production_step(std::vector<Unit>& units, std::array<std::uint16_t, 2>& ore,
                     std::uint32_t& next_id, std::uint64_t seed, std::size_t index) noexcept {
    BuildQueue& queue = units.at(index).queue;
    if (!units.at(index).alive || queue.count == 0) {
        return;
    }
    const BuildEntry front = queue.entries.at(0);
    const UnitSpec& spec = spec_of(front.kind);
    const Side side = units.at(index).side;
    std::uint16_t& bank = ore.at(static_cast<std::size_t>(side));
    if (queue.left == 0) {
        if (bank < spec.cost) {
            return; // paused: the bank is short, so nothing is spent and nothing runs
        }
        bank = static_cast<std::uint16_t>(bank - spec.cost);
        queue.left = spec.build_ticks;
        return;
    }
    if (--queue.left > 0) {
        return;
    }
    const Coord spot = builder_spot(seed, units, units.at(index).x, units.at(index).y);
    if (spot.x < 0) {
        queue.left = 1; // no room to put it: it is finished, and arrives the first tick there is
        return;
    }
    for (std::size_t i = 1; i < queue.count; ++i) {
        queue.entries.at(i - 1) = queue.entries.at(i);
    }
    --queue.count;
    queue.entries.at(queue.count) = BuildEntry{};
    Unit made = fresh_unit(next_id++, front.kind, side, spot);
    made.batch = front.batch;
    units.push_back(made);
}

/// Whether `unit` has given up on this pile (`give_up_pile`): it is not offered it again.
bool gave_up_on(const Unit& unit, const Scrap& piece) noexcept {
    const std::size_t known
        = std::min(static_cast<std::size_t>(unit.shunned_count), unit.shunned.size());
    const TileXY tile = tile_of(piece.x, piece.y);
    return std::any_of(unit.shunned.begin(),
                       unit.shunned.begin() + static_cast<std::ptrdiff_t>(known),
                       [&](const TileXY& shunned) { return shunned == tile; });
}

/// The unit gives up the pile at `piece`: it remembers its tile (the ring overwrites the oldest)
/// and the trip is over.
void give_up_pile(Unit& unit, const Scrap& piece) noexcept {
    unit.shunned.at(static_cast<std::size_t>(unit.shunned_count) % unit.shunned.size())
        = tile_of(piece.x, piece.y);
    if (unit.shunned_count < 0xFF) {
        ++unit.shunned_count;
    }
    unit.stance = Stance::Idle;
    unit.fetch_beats = 0;
}

/// The nearest piece of wreckage, ties broken by the order the pieces were made in -- which is the
/// order `scraps_` itself is in, so "the nearest" is a function of the litter and not of the walk.
std::size_t nearest_scrap(const std::vector<Scrap>& scraps, const Unit& unit) noexcept {
    std::size_t best = kNoScrap;
    std::int64_t best_d = 0;
    for (std::size_t i = 0; i < scraps.size(); ++i) {
        if (gave_up_on(unit, scraps.at(i))) {
            continue;
        }
        const std::int64_t d = dist2(unit.x, unit.y, scraps.at(i).x, scraps.at(i).y);
        if (best == kNoScrap || d < best_d) {
            best = i;
            best_d = d;
        }
    }
    return best;
}

/// Whether a PEER of `unit` -- another living unit of the same side -- is already on its way to
/// this pile: `Stance::Fetch` with the pile's spot in `guard_x/y`. The claim is the commitment, so
/// it stands from the beat the peer sets off and dies with the `Fetch` that held it (arrival,
/// give-up, an order). Same side only: an enemy's errand is not a sensor for this side.
bool claimed_by_peer(const std::vector<Unit>& units, const Unit& unit,
                     const Scrap& piece) noexcept {
    return std::any_of(units.begin(), units.end(), [&](const Unit& other) {
        return other.alive && other.id != unit.id && other.side == unit.side
               && other.stance == Stance::Fetch && other.guard_x == piece.x
               && other.guard_y == piece.y;
    });
}

/// THE SCAVENGERS SPREAD OUT: the nearest pile inside `reach_sq` that `unit` has not given up on
/// and that no peer is already fetching; ties by litter order. With every pile in reach claimed it
/// is `nearest_scrap`, so nobody stands idle. Order, not chance: whoever commits first this tick
/// (the beat is phased by unit id) holds the nearer pile and the next one sees the claim.
std::size_t nearest_free_scrap(const std::vector<Scrap>& scraps, const std::vector<Unit>& units,
                               const Unit& unit, std::int64_t reach_sq) noexcept {
    std::size_t best = kNoScrap;
    std::int64_t best_d = 0;
    for (std::size_t i = 0; i < scraps.size(); ++i) {
        const Scrap& piece = scraps.at(i);
        const std::int64_t d = dist2(unit.x, unit.y, piece.x, piece.y);
        if (d > reach_sq || gave_up_on(unit, piece) || claimed_by_peer(units, unit, piece)) {
            continue;
        }
        if (best == kNoScrap || d < best_d) {
            best = i;
            best_d = d;
        }
    }
    return best != kNoScrap ? best : nearest_scrap(scraps, unit);
}

/// How far from a wreck its litter may settle, in tiles: the scatter's reach plus the walk east
/// that finds a free tile. This bounds the connectivity check below to a (2R+1)^2 window of tiles.
constexpr int kLitterRegionRadius = kScrapScatterRadius + kScrapSpan;
constexpr int kLitterRegionSide = (2 * kLitterRegionRadius) + 1;
constexpr std::size_t kLitterRegionCells
    = static_cast<std::size_t>(kLitterRegionSide) * static_cast<std::size_t>(kLitterRegionSide);

/// THE TILES A COLLECTOR COULD DRIVE TO FROM A WRECK: a flood fill, over the window of
/// `kLitterRegionRadius` tiles about `home`, of the standable tiles joined to `home` the way the
/// route planner joins them (eight ways, no corner cut). Integer, deterministic, and bounded: at
/// most 15 x 15 = 225 tiles, once per wreck. A tile outside the window is "not in the region" --
/// the safe answer, since a pile placed only where this says yes is always reachable.
class LitterRegion {
public:
    LitterRegion(std::uint64_t seed, std::span<const Unit> units, Coord home) noexcept
        : seed_(seed), units_(units), left_(home.x - kLitterRegionRadius),
          top_(home.y - kLitterRegionRadius) {
        std::array<Coord, kLitterRegionCells> queue{};
        std::size_t head = 0;
        std::size_t tail = 0;
        mark(home.x, home.y);
        queue.at(tail++) = home;
        while (head < tail) {
            const Coord at = queue.at(head++);
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const bool diagonal = dx != 0 && dy != 0;
                    if ((dx == 0 && dy == 0) || !open(at.x + dx, at.y + dy)
                        || (diagonal
                            && (!standable(seed_, units_, at.x + dx, at.y)
                                || !standable(seed_, units_, at.x, at.y + dy)))) {
                        continue;
                    }
                    mark(at.x + dx, at.y + dy);
                    queue.at(tail++) = Coord{at.x + dx, at.y + dy};
                }
            }
        }
    }
    [[nodiscard]] bool contains(int x, int y) const noexcept {
        return inside(x, y) && seen_.at(index(x, y));
    }

private:
    [[nodiscard]] bool inside(int x, int y) const noexcept {
        return x >= left_ && y >= top_ && x < left_ + kLitterRegionSide
               && y < top_ + kLitterRegionSide;
    }
    [[nodiscard]] std::size_t index(int x, int y) const noexcept {
        return (static_cast<std::size_t>(y - top_) * static_cast<std::size_t>(kLitterRegionSide))
               + static_cast<std::size_t>(x - left_);
    }
    /// A tile the fill may enter: in the window, standable, and not yet visited.
    [[nodiscard]] bool open(int x, int y) const noexcept {
        return inside(x, y) && !seen_.at(index(x, y)) && standable(seed_, units_, x, y);
    }
    void mark(int x, int y) noexcept { seen_.at(index(x, y)) = true; }

    std::uint64_t seed_;
    std::span<const Unit> units_;
    int left_;
    int top_;
    std::array<bool, kLitterRegionCells> seen_{};
};

} // namespace

/// Wreckage settles where a hauler can reach it: a piece the hash dropped on ground no vehicle may
/// enter -- a crater -- moves to the nearest tile one can, and never onto a tile another piece of
/// the same wreck already holds. Deterministic, like the scatter itself: the same battle settles
/// the same way. Without this a piece can lie in a crater where the only way to it is a tile the
/// hauler cannot stand on, and a scavenger drives at the crater rim forever.
///
/// AND WHERE A HAULER CAN GET TO: a wreck's pieces settle only in the region the wreck itself lay
/// in (`LitterRegion`), so the scatter can no longer drop a piece on a tile ringed by craters --
/// an island nothing can drive onto. `home_x/y` is the wreck's tile; with `home_x < 0` (a
/// scenario's own pile, which its author put where he meant it) there is no check, as before.
void settle_litter(std::vector<Scrap>& scraps, std::size_t first, std::uint64_t seed,
                   std::span<const Unit> units, int home_x, int home_y) noexcept {
    std::optional<LitterRegion> region;
    if (home_x >= 0 && first < scraps.size()) {
        region.emplace(seed, units, nearest_where(home_x, home_y, [seed, units](int tx, int ty) {
                           return standable(seed, units, tx, ty);
                       }));
    }
    const auto lands = [&](int tx, int ty) {
        return standable(seed, units, tx, ty) && (!region || region->contains(tx, ty));
    };
    for (std::size_t i = first; i < scraps.size(); ++i) {
        TileXY at = tile_of(scraps.at(i).x, scraps.at(i).y);
        // A building's footprint is no more reachable than a crater: a piece never settles in one.
        if (!lands(at.x, at.y)) {
            const Coord spot = nearest_where(at.x, at.y, lands);
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
            const Coord next = nearest_where(static_cast<int>(at.x) + 1, at.y, lands);
            at = TileXY{static_cast<std::uint8_t>(next.x), static_cast<std::uint8_t>(next.y)};
        }
        scraps.at(i) = Scrap{tile_center(at.x), tile_center(at.y)};
    }
}

namespace {

/// SCAVENGING BY A TRIP (C2, AR). Called for every living scavenger before it is steered, in
/// ascending unit order, so a piece a lower id took this tick is already gone when a higher id
/// looks.
///
/// Nothing is carried and nothing is delivered (Harri: scavengers "just collect, and scrap
/// magically appears in your stores. It's immediately available!"): a piece taken goes straight
/// into its side's bank in the same tick. But the scavenger must be AT the pile
/// (`kScavengeCollectRadiusRaw`, one tile) to take it (Harri, 2026-11-08: "it has to actually go to
/// the spot where the scraps are to pick them up. They shouldn't just magically disappear"):
///   - Idle, on the beat (`kScavengeIntervalTicks`, phased by unit id so a group does not all act
///   on
///     the same tick), it looks at the NEAREST pile inside its own radar (`radar_radius_tiles`).
///     One it is standing at is taken there and then; a farther one sends it off: stance `Fetch`,
///     the pile's spot in `guard_x/y`, driven there by `steer` like a MoveTo.
///   - Under `Fetch` it is committed to that pile (no flip between two equally near ones). On
///     the first beat after it arrives it takes the pile and is Idle again (so it may stand a
///     moment, under a second, at the pile: the pickup keeps the beat, which is what keeps the
///     bank rising one piece per scavenger per second and never two on a tick); the beat after
///     picks the next pile. If the pile is gone on the way (another scavenger took it) it re-aims
///     at the nearest in its radar, and with none it stands down. A full store also stands it down.
///   - A pile it CANNOT REACH is given up, not waited for: if the route planner found no route to
///     the pile's tile, or the trip has taken `kFetchGiveUpBeats` beats, it goes Idle and does not
///     choose that pile again (`give_up_pile`), so the next beat takes another pile or none.
/// Only an IDLE or FETCHING scavenger collects: any order the player gives (a move, a follow, a
/// hold) replaces `Fetch` and is a stance of its own that the sim never argues with; `Stop` -- the
/// `3 Scavenge` row -- sets it scavenging again. A DEPLOYED scavenger is another kind altogether
/// and never comes here: its pool is drawn in by `store_step`, with no pile and no trip.
void scavenge_step(std::vector<Scrap>& scraps, const std::vector<Unit>& units,
                   std::array<std::uint16_t, 2>& ore, std::array<std::uint32_t, 2>& collected,
                   Unit& unit, std::uint32_t tick, std::uint16_t capacity) noexcept {
    const bool fetching = unit.stance == Stance::Fetch;
    if (unit.stance != Stance::Idle && !fetching) {
        return;
    }
    if ((tick + unit.id.value) % static_cast<std::uint32_t>(kScavengeIntervalTicks) != 0) {
        return; // the beat: everything below, driving off included, happens once a second
    }
    std::uint16_t& bank = ore.at(static_cast<std::size_t>(unit.side));
    // A FULL STORE STOPS COLLECTION: the piece stays where it lies, not taken and not destroyed.
    if (bank >= capacity) {
        if (fetching) {
            unit.stance = Stance::Idle;
            unit.fetch_beats = 0;
        }
        return;
    }
    const std::int64_t reach = static_cast<std::int64_t>(radar_radius_tiles(unit.kind))
                               << Fixed::kShift;
    const std::int64_t arrive = kScavengeCollectRadiusRaw;
    // A trip in hand keeps its pile while the pile is there; otherwise the nearest in the radar.
    std::size_t pick = nearest_free_scrap(scraps, units, unit, reach * reach);
    if (fetching) {
        const auto held = std::find_if(scraps.begin(), scraps.end(), [&](const Scrap& piece) {
            return piece.x == unit.guard_x && piece.y == unit.guard_y;
        });
        if (held != scraps.end()) {
            pick = static_cast<std::size_t>(held - scraps.begin());
        }
    }
    if (pick == kNoScrap
        || dist2(unit.x, unit.y, scraps.at(pick).x, scraps.at(pick).y) > reach * reach) {
        if (fetching) {
            unit.stance = Stance::Idle; // nothing left in sight: the errand is over
            unit.fetch_beats = 0;
        }
        return;
    }
    if (dist2(unit.x, unit.y, scraps.at(pick).x, scraps.at(pick).y) > arrive * arrive) {
        // Not at it: drive there. Re-aiming at an unchanged pile keeps the route (steer re-plans
        // only on a new goal), so this costs nothing each tick.
        if (!fetching || unit.guard_x != scraps.at(pick).x || unit.guard_y != scraps.at(pick).y) {
            apply_posted(unit, Stance::Fetch, scraps.at(pick).x, scraps.at(pick).y, Fixed{},
                         Fixed{});
            return;
        }
        // A TRIP IT CANNOT FINISH IS GIVEN UP (the stalled-fetch fix). Held on a pile for a beat or
        // more, the route planner has had its say: a plan that found no route to the pile's tile
        // means no drive ends there (`steer` would only press on the rim), and `kFetchGiveUpBeats`
        // on one trip is the backstop for any wedge the plan did not foresee.
        if (unit.fetch_beats < 0xFF) {
            ++unit.fetch_beats;
        }
        const TileXY pile_tile = tile_of(scraps.at(pick).x, scraps.at(pick).y);
        const bool no_route = unit.route.has_goal && unit.route.count == 0
                              && unit.route.goal == pile_tile
                              && !(tile_of(unit.x, unit.y) == pile_tile);
        if (no_route || unit.fetch_beats >= kFetchGiveUpBeats) {
            give_up_pile(unit, scraps.at(pick));
        }
        return;
    }
    scraps.erase(scraps.begin() + static_cast<std::ptrdiff_t>(pick));
    unit.stance = Stance::Idle;
    unit.fetch_beats = 0;
    bank = static_cast<std::uint16_t>(std::min(static_cast<int>(bank) + 1, 0xFFFF));
    ++collected.at(static_cast<std::size_t>(unit.side));
}

/// A SCAVENGER WITH NO ORDER DEPLOYS ITSELF (Harri, 2026-11-01, watched in BattleZone 2: pointed at
/// the ground near a pool, "Go To", and "it waited there for a few seconds. Then, ON ITS OWN, it
/// went to the pool and deployed itself"). Called for every living scavenger after its deploy step
/// and before it collects, on the same once-a-second beat, ONLY WHEN THE SCAVENGER HAS NO ORDER
/// (`Stance::Idle`): a Follow Me, a Go To or anything else suspends it for as long as that order
/// stands and going idle resumes it, which is how a player leads a scavenger past a pool without it
/// stopping. Its own radar gates it, the number that is also its collection range, so it never acts
/// on a pool it cannot see; and it deploys only when it sees NO litter there (litter is the faster
/// income, and `scavenge_step` has first call on it). The pool is the nearest free one in reach,
/// and one another scavenger is already driving to is not free, so a second idle scavenger takes
/// the next pool and does not make a wasted trip after the first. It sets the very stance a
/// `Deploy` order sets, so everything after is `scavenger_deploy_step`'s. The enemy's
/// any-vehicle-points-at-it order is a separate path and is untouched; this is the scavenger's own
/// idle decision, and it applies to both sides.
void auto_deploy_step(const std::vector<Unit>& units, const std::vector<Pool>& pools,
                      const std::vector<Scrap>& scraps, Unit& unit, std::uint32_t tick) noexcept {
    if (unit.stance != Stance::Idle
        || (tick + unit.id.value) % static_cast<std::uint32_t>(kScavengeIntervalTicks) != 0) {
        return;
    }
    const std::int64_t reach = static_cast<std::int64_t>(radar_radius_tiles(unit.kind))
                               << Fixed::kShift;
    const std::size_t litter = nearest_scrap(scraps, unit);
    if (litter != kNoScrap
        && dist2(unit.x, unit.y, scraps.at(litter).x, scraps.at(litter).y) <= reach * reach) {
        return; // it sees litter: collecting comes first
    }
    const Pool* best = nullptr;
    std::int64_t best_d = 0;
    for (const Pool& pool : pools) {
        const Fixed px = tile_centre_of(pool.tile_x);
        const Fixed py = tile_centre_of(pool.tile_y);
        const std::int64_t d = dist2(unit.x, unit.y, px, py);
        if (d > reach * reach || pool_taken(units, pool) || (best != nullptr && d >= best_d)) {
            continue;
        }
        const bool claimed = std::any_of(units.begin(), units.end(), [&](const Unit& other) {
            return other.alive && other.id != unit.id && other.kind == UnitKind::Scavenger
                   && other.stance == Stance::Deploy && other.guard_x.floor_int() == pool.tile_x
                   && other.guard_y.floor_int() == pool.tile_y;
        });
        if (!claimed) {
            best = &pool;
            best_d = d;
        }
    }
    if (best == nullptr) {
        return;
    }
    apply_posted(unit, Stance::Deploy, tile_centre_of(best->tile_x), tile_centre_of(best->tile_y),
                 Fixed{}, Fixed{});
    unit.deploy_ticks = 0;
}

/// DEPLOYING A SCAVENGER ON A POOL. Called for every living scavenger before it is steered; returns
/// true when it is not to be steered this tick (counting at its pool, just deployed, or its order
/// has failed). The shape is `deploy_step`'s: away from the pool it drives there like a MoveTo with
/// the count at zero, at it the count runs to `scavenger_deploy_ticks`, and the site is re-checked
/// every tick -- a pool another scavenger has finished on ends the order whole, the loser standing
/// idle, still a scavenger. The finished unit is the same unit, settled on the pool's middle, as a
/// `DeployedScavenger`: its store (+20) exists from this tick and the order can never be undone.
bool scavenger_deploy_step(const std::vector<Unit>& units, const std::vector<Pool>& pools,
                           Unit& unit) noexcept {
    if (unit.stance != Stance::Deploy) {
        unit.deploy_ticks = 0;
        return false;
    }
    const Pool* pool = pool_at(pools, unit.guard_x.floor_int(), unit.guard_y.floor_int());
    if (pool == nullptr || pool_taken(units, *pool)) {
        unit.stance = Stance::Idle;
        unit.deploy_ticks = 0;
        unit.route = Route{};
        unit.vx = Fixed::zero();
        unit.vy = Fixed::zero();
        return true;
    }
    constexpr std::int64_t kArrive = kArriveRadiusRaw;
    if (dist2(unit.x, unit.y, unit.guard_x, unit.guard_y) >= kArrive * kArrive) {
        unit.deploy_ticks = 0;
        return false;
    }
    unit.vx = Fixed::zero();
    unit.vy = Fixed::zero();
    ++unit.deploy_ticks;
    if (unit.deploy_ticks < scavenger_deploy_ticks(unit.side)) {
        return true;
    }
    unit.kind = UnitKind::DeployedScavenger;
    unit.x = unit.guard_x;
    unit.y = unit.guard_y;
    unit.stance = Stance::Idle;
    unit.deploy_ticks = 0;
    unit.route = Route{};
    unit.detour = Detour{};
    return true;
}

/// THE STORE, once a tick (after the shooting, so a deployed scavenger killed this tick has already
/// taken its 20 with it). First the bank is CLAMPED to what each side can hold -- "you can't have
/// more scrap than capacity to hold it": a lost deployed scavenger destroys the scrap above the new
/// capacity. Then each side that holds a deployed scavenger extracts ONE unit on the global beat,
/// whatever number of pools it holds (the rate does not scale), into whatever room is left. The
/// beat is the fast one inside the gold band and the slow one above it.
void store_step(const std::vector<Unit>& units, std::array<std::uint16_t, 2>& ore,
                std::array<std::uint32_t, 2>& collected, std::uint16_t base,
                std::uint32_t tick) noexcept {
    for (const Side side : {Side::Player, Side::Enemy}) {
        std::uint16_t& bank = ore.at(static_cast<std::size_t>(side));
        const std::uint16_t capacity = store_capacity(units, side, base);
        bank = std::min(bank, capacity);
        // THE TWO RATES: the bank fills at the fast beat while its level is inside the gold band
        // (the bottom `capacity - base` of the store) and at the slow one once it climbs into the
        // green. The band is the only thing a deployed scavenger changes -- see
        // `kPoolFastIntervalTicks` -- so no rate here depends on how many scavengers there are.
        const int beat = bank < capacity - base ? kPoolFastIntervalTicks : kPoolSlowIntervalTicks;
        if (tick % static_cast<std::uint32_t>(beat) != 0 || bank >= capacity) {
            continue;
        }
        const bool extracting = std::any_of(units.begin(), units.end(), [side](const Unit& unit) {
            return unit.alive && unit.side == side && unit.kind == UnitKind::DeployedScavenger;
        });
        if (extracting) {
            ++bank;
            ++collected.at(static_cast<std::size_t>(side));
        }
    }
}

/// DEPLOY (R2). Called for every living recycler before it is steered. Returns true when the
/// recycler is not to be steered this tick: it is standing at its site counting, it has just become
/// a building, or its order has just failed.
///
/// The countdown is a matter of being AT the site: inside the arrive radius of its centre, the
/// radius a MoveTo stops in. Away from it the recycler is steered there like a MoveTo and the count
/// is zero. The site is re-checked every tick, so one that stops being usable (another building has
/// been deployed over it) ends the order whole: the recycler stands, idle, still a `Recycler` --
/// there is no half-deployed state to be left in.
bool deploy_step(const std::vector<Unit>& units, Unit& unit, std::uint64_t seed) noexcept {
    if (unit.stance != Stance::Deploy) {
        unit.deploy_ticks = 0; // a countdown never outlives the order that started it
        return false;
    }
    const Footprint site = footprint_around(unit.guard_x, unit.guard_y);
    if (!deploy_site_ok(seed, units, site.x, site.y)) {
        unit.stance = Stance::Idle;
        unit.deploy_ticks = 0;
        unit.route = Route{};
        unit.vx = Fixed::zero();
        unit.vy = Fixed::zero();
        return true;
    }
    constexpr std::int64_t kArrive = kArriveRadiusRaw; // the radius a MoveTo stops in
    if (dist2(unit.x, unit.y, unit.guard_x, unit.guard_y) >= kArrive * kArrive) {
        unit.deploy_ticks = 0;
        return false; // on its way: steered like a MoveTo
    }
    unit.vx = Fixed::zero();
    unit.vy = Fixed::zero();
    ++unit.deploy_ticks;
    if (unit.deploy_ticks < kDeployTicks) {
        return true;
    }
    // The transformation, IN PLACE: the same entry in the table, so the same id, and the hit points
    // it has now -- deploying neither heals nor wounds it. It settles on the site's centre, which
    // is what puts its footprint on the site's four tiles.
    unit.kind = UnitKind::DeployedRecycler;
    unit.x = unit.guard_x;
    unit.y = unit.guard_y;
    unit.stance = Stance::Idle;
    unit.deploy_ticks = 0;
    unit.route = Route{};
    unit.detour = Detour{};
    return true;
}

/// Whether an order a `DeployedTurret` holds asks it to leave its spot: an un-deploy by name, or
/// any stance that goes somewhere -- a move, a patrol, a follow, a redeploy, a defend, a recycle,
/// or a post it is not standing at. Hold (a post under its own feet), Attack and Idle keep it up.
bool wants_to_leave(const Unit& unit) noexcept {
    switch (unit.stance) {
    case Stance::Undeploy:
    case Stance::MoveTo:
    case Stance::Patrol:
    case Stance::Follow:
    case Stance::Deploy:
    case Stance::Defend:
    case Stance::Recycle:
    case Stance::Repair:
        return true;
    case Stance::GuardAt: {
        constexpr std::int64_t kArrive = kArriveRadiusRaw;
        return dist2(unit.x, unit.y, unit.guard_x, unit.guard_y) >= kArrive * kArrive;
    }
    case Stance::Idle:
    case Stance::Attack:
    case Stance::Dead:
    case Stance::Fetch:
        break;
    }
    return false;
}

/// THE TURRET'S DEPLOY AND UN-DEPLOY (TURRET-CMD). Called for every living turret, plain or
/// deployed, before it is steered; returns true when it is not to be steered this tick.
///
/// A plain `Turret` under a `Deploy` order drives to its spot (the tile's middle, within the
/// MoveTo's arrive radius) and counts `kTurretDeployTicks` there; under a `Defend` order it drives
/// at its enemy until it is within its own WEAPON RANGE of it (`profile(Turret).weapon_range`,
/// `kTurretRangeRaw`: 16 tiles) and counts from there. On the last tick it is the SAME unit as a
/// `DeployedTurret`, where it stands: the gun is up and it takes no step.
///
/// A `DeployedTurret` counts `kTurretUndeployTicks` once it has been told to leave -- by an
/// `Undeploy` order or by any order that goes somewhere (`wants_to_leave`) -- and then is a plain
/// `Turret` again with its stance as the order left it, so a move or a follow carries straight on.
/// A count once begun runs to the end: Stop does not put the gun back. Both ways go through
/// `deploy_ticks`, which on a deployed turret is the un-deploy's count.
bool turret_deploy_step(const std::vector<Unit>& units, Unit& unit, std::uint64_t seed) noexcept {
    if (unit.kind == UnitKind::DeployedTurret) {
        unit.vx = Fixed::zero();
        unit.vy = Fixed::zero();
        if (unit.deploy_ticks == 0 && unit.stance == Stance::Defend) {
            // Already up, and the enemy is already in range: there is nothing to take down.
            const Unit* enemy = find_const(units, unit.target);
            const std::int64_t range = profile(UnitKind::Turret).weapon_range.raw;
            if (enemy != nullptr && enemy->alive
                && dist2(unit.x, unit.y, enemy->x, enemy->y) < range * range) {
                unit.stance = Stance::Idle;
                unit.target = UnitId{};
                return true;
            }
        }
        if (unit.deploy_ticks == 0 && !wants_to_leave(unit)) {
            return true;
        }
        if (++unit.deploy_ticks < kTurretUndeployTicks) {
            return true;
        }
        unit.kind = UnitKind::Turret;
        unit.deploy_ticks = 0;
        unit.still_ticks = 0; // off its mount: it crawls, and mounts again as any turret does
        unit.unmount_ticks = 0;
        if (unit.stance == Stance::Undeploy) {
            unit.stance = Stance::Idle;
        }
        return true;
    }
    if (unit.kind != UnitKind::Turret) {
        return false;
    }
    if (unit.stance != Stance::Deploy && unit.stance != Stance::Defend) {
        unit.deploy_ticks = 0;
        return false;
    }
    std::int64_t radius = kArriveRadiusRaw; // a spot: the radius a MoveTo stops in
    if (unit.stance == Stance::Defend) {
        const Unit* enemy = find_const(units, unit.target);
        if (enemy == nullptr || !enemy->alive) {
            unit.stance = Stance::Idle; // moot: nobody left to defend against
            unit.target = UnitId{};
            unit.deploy_ticks = 0;
            return false;
        }
        unit.guard_x = enemy->x; // steered at like a MoveTo, re-aimed every tick
        unit.guard_y = enemy->y;
        radius = profile(UnitKind::Turret).weapon_range.raw;
    } else if (!tile_passable(seed, unit.guard_x.floor_int(), unit.guard_y.floor_int())
               || building_blocks(units, unit.guard_x.floor_int(), unit.guard_y.floor_int())) {
        unit.stance = Stance::Idle; // the spot stopped being ground: the order ends whole
        unit.deploy_ticks = 0;
        unit.route = Route{};
        return false;
    }
    if (dist2(unit.x, unit.y, unit.guard_x, unit.guard_y) >= radius * radius) {
        unit.deploy_ticks = 0;
        return false; // on its way
    }
    unit.vx = Fixed::zero();
    unit.vy = Fixed::zero();
    if (++unit.deploy_ticks < kTurretDeployTicks) {
        return true;
    }
    unit.kind = UnitKind::DeployedTurret;
    unit.stance = Stance::Idle;
    unit.target = UnitId{};
    unit.deploy_ticks = 0;
    unit.route = Route{};
    unit.detour = Detour{};
    return true;
}

/// RECYCLE. Called for every living unit before it is steered. Returns true when the unit is not to
/// be steered this tick: it is standing at its recycler and has just been consumed into the ledger.
/// Away from it the unit chases the recycler's CURRENT position -- an undeployed recycler can drive
/// -- so the goal is re-written every tick and a moving base is followed, not missed. The unit is
/// set !alive WITHOUT an Impact, which is what keeps step 6 from scattering a wreck: its value goes
/// to the ledger, and the ground gets nothing.
bool recycle_step(std::vector<Unit>& units, std::vector<Scrap>& scraps, Unit& unit,
                  std::array<std::uint16_t, 2>& ore, std::uint16_t base,
                  std::uint64_t seed) noexcept {
    if (unit.stance != Stance::Recycle) {
        return false;
    }
    const Unit* target = find_const(units, unit.target);
    if (target == nullptr || !target->alive) {
        unit.stance = Stance::Idle; // the recycler is gone: stand down, still a unit
        unit.target = UnitId{};
        return true;
    }
    constexpr std::int64_t kArrive = kArriveRadiusRaw; // the radius a MoveTo stops in
    if (dist2(unit.x, unit.y, target->x, target->y) >= kArrive * kArrive) {
        unit.guard_x = target->x;
        unit.guard_y = target->y;
        return false; // on its way: steered like a MoveTo
    }
    unit.vx = Fixed::zero();
    unit.vy = Fixed::zero();
    unit.alive = false;
    unit.stance = Stance::Dead;
    unit.target = UnitId{};
    const auto side = static_cast<std::size_t>(unit.side);
    // What a recycle returns goes into the store as far as the store has room. THE REMAINDER LANDS
    // ON THE GROUND AS COLLECTABLE LITTER where the unit stood (Harri, 2026-11-01): nothing is
    // destroyed and nothing is blocked, and it is the rule a destroyed building already follows --
    // `scatter` -- one piece to the unit of refund. Do not turn this back into a clamp: a silent
    // clamp is invisible, and the capacity design depends on the economy staying legible.
    const int room = std::max(0, static_cast<int>(store_capacity(units, unit.side, base))
                                     - static_cast<int>(ore.at(side)));
    const int banked = std::min(room, static_cast<int>(kRecycleOre));
    ore.at(side) = static_cast<std::uint16_t>(ore.at(side) + banked);
    if (banked < static_cast<int>(kRecycleOre)) {
        const std::size_t first = scraps.size();
        scatter(scraps, seed, unit.id, unit.x, unit.y, static_cast<int>(kRecycleOre) - banked);
        settle_litter(scraps, first, seed, units, unit.x.floor_int(), unit.y.floor_int());
    }
    return true;
}

// THE TURN RATE. A machine whose spec limits it (`turn_decidegrees`, the Walker) carries a heading
// (`Unit::heading_x/y`) and `steer` turns it toward the way it wants to go by at most one step a
// tick; until it faces that way it stands and turns, then it sets off. Integers only: the step's
// sine and cosine come from a Taylor series in Q28, no float, no table, no clock.
constexpr int kTrigBits = 28;
constexpr std::int64_t kTrigOne = std::int64_t{1} << kTrigBits;

struct Rotation {
    std::int64_t sin{};
    std::int64_t cos{};
};

/// sin and cos of `decidegrees` tenths of a degree, Q28. The angle in radians is
/// decidegrees * pi / 1800; pi is taken as 3.14159265358979 in Q28 (843314857). Terms to x^9 are
/// far below one part in 2^28 for the few degrees a tick this is used with.
[[nodiscard]] constexpr Rotation rotation_of(std::int64_t decidegrees) noexcept {
    constexpr std::int64_t kPiQ28 = 843314857;
    const std::int64_t x = decidegrees * kPiQ28 / 1800;
    const std::int64_t x2 = x * x >> kTrigBits;
    std::int64_t sin = x;
    std::int64_t cos = kTrigOne;
    std::int64_t term = x;        // x^(2k+1) / (2k+1)!
    std::int64_t even = kTrigOne; // x^(2k) / (2k)!
    for (std::int64_t k = 1; k <= 4; ++k) {
        even = even * x2 / (kTrigOne * (2 * k - 1) * (2 * k)) * -1;
        term = term * x2 / (kTrigOne * (2 * k) * (2 * k + 1)) * -1;
        cos += even;
        sin += term;
    }
    return Rotation{sin, cos};
}

/// (vx, vy) scaled so the larger component is `kHeadingScale`; a zero vector stays zero.
void scale_heading(std::int64_t vx, std::int64_t vy, std::int32_t& out_x,
                   std::int32_t& out_y) noexcept {
    const std::int64_t major = std::max(std::abs(vx), std::abs(vy));
    if (major == 0) {
        out_x = 0;
        out_y = 0;
        return;
    }
    out_x = static_cast<std::int32_t>(vx * kHeadingScale / major);
    out_y = static_cast<std::int32_t>(vy * kHeadingScale / major);
}

/// Turns `unit`'s heading toward (dx, dy) by at most `decidegrees` tenths of a degree. True when it
/// now faces that way exactly (it was within one step and snapped), so it may move this tick.
[[nodiscard]] bool turn_toward(Unit& unit, std::int64_t dx, std::int64_t dy,
                               std::uint16_t decidegrees) noexcept {
    std::int32_t wx = 0;
    std::int32_t wy = 0;
    scale_heading(dx, dy, wx, wy);
    // Never turned: +Y, the way the presentation draws a hull that has not moved.
    const bool unset = unit.heading_x == 0 && unit.heading_y == 0;
    const std::int64_t hx = unset ? 0 : unit.heading_x;
    const std::int64_t hy = unset ? kHeadingScale : unit.heading_y;
    const std::int64_t cross = hx * wy - hy * wx;
    const std::int64_t dot = hx * wx + hy * wy;
    const Rotation step = rotation_of(decidegrees);
    // Within one step when tan(angle) <= tan(step) with the angle acute: |cross| * cos <= dot *
    // sin.
    if (dot > 0 && std::abs(cross) * step.cos <= dot * step.sin) {
        unit.heading_x = wx;
        unit.heading_y = wy;
        return true;
    }
    // Otherwise one full step toward it (the long way round is never taken: the sign of the cross
    // product picks the nearer side; exactly behind turns the same way every time).
    const std::int64_t s = cross >= 0 ? step.sin : -step.sin;
    const std::int64_t nx = (hx * step.cos - hy * s) >> kTrigBits;
    const std::int64_t ny = (hx * s + hy * step.cos) >> kTrigBits;
    scale_heading(nx, ny, unit.heading_x, unit.heading_y);
    return false;
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
    // A scavenger on a trip to a pile of litter (`scavenge_step`) drives at the pile's spot, which
    // it wrote into guard_x/y.
    case Stance::Fetch:
    // A deploy drives to its site's centre, which the order wrote into guard_x/guard_y.
    case Stance::Deploy:
    // A recycle drives at its recycler, whose CURRENT position `recycle_step` writes into
    // guard_x/y.
    case Stance::Recycle:
    // A defend drives at its enemy's CURRENT position, which `turret_deploy_step` writes into
    // guard_x/y every tick; an undeploy has nowhere to go, and a turret in it never steers.
    case Stance::Defend:
    case Stance::Undeploy:
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
        // Shoot and scoot: while it reloads, with the target still in range, the bike drives to
        // the spot its last shot chose (`guard_x/y`) instead of standing where it fired. Out of
        // range, or loaded, it is the plain engagement above: close until in range, hold, fire.
        const std::int64_t reach = prof.weapon_range.raw;
        if (scoots(unit.kind) && unit.cooldown > 0
            && dist2(unit.x, unit.y, target->x, target->y) <= reach * reach) {
            goal_x = unit.guard_x;
            goal_y = unit.guard_y;
            stop_radius = kArriveRadiusRaw;
        }
        // An attacker in range with no line to the target (a hill between) would stand there for
        // ever: it goes to the nearest tile that has one. Loaded or not, it keeps the target.
        if (dist2(unit.x, unit.y, target->x, target->y) < reach * reach
            && !has_line(unit, *target, ground.seed)) {
            if (const std::optional<ScootSpot> spot
                = firing_spot(ground.seed, units, unit, *target)) {
                goal_x = spot->x;
                goal_y = spot->y;
                // Not the arrival radius: the nearest tile with a line can be inside it, and a
                // unit "arrived" there would stand in the hill's shadow. Having a line ends this.
                stop_radius = 0;
            }
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
    case Stance::Repair: {
        // Close on the patient's CURRENT position to the approach radius and hold; the mending is
        // `service_repair_step`'s. A patient that is whole or gone ends the errand.
        const Unit* target = find_const(units, unit.target);
        if (target == nullptr || !target->alive || target->hp >= profile(target->kind).hp) {
            unit.stance = Stance::Idle;
            unit.target = UnitId{};
            return;
        }
        goal_x = target->x;
        goal_y = target->y;
        stop_radius = static_cast<std::int64_t>(kServiceRepairApproachTiles) << Fixed::kShift;
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
    if (prof.turn_decidegrees != 0 && !turn_toward(unit, dx, dy, prof.turn_decidegrees)) {
        return; // still coming round: it stands (the velocity is zero from the top of `steer`)
    }
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

/// The vehicle a step would run into, or null: the first living MOBILE unit (by id) that the step
/// brings inside `kBodyRadiusRaw` of `unit` AND closer than it was, so two vehicles already
/// overlapping can always drive apart. A building is not a vehicle -- the ground already refuses
/// it -- and a unit in the middle of deploying is a site, not a body: it is not moved.
[[nodiscard]] Unit* vehicle_in_the_way(std::vector<Unit>& units, const Unit& unit, std::int64_t dx,
                                       std::int64_t dy) noexcept {
    constexpr std::int64_t kRadius = kBodyRadiusRaw;
    for (Unit& other : units) {
        if (!other.alive || other.id == unit.id || other.deploy_ticks != 0
            || !profile(other.kind).mobile) {
            continue;
        }
        const std::int64_t ox = other.x.raw - unit.x.raw;
        const std::int64_t oy = other.y.raw - unit.y.raw;
        const std::int64_t before = (ox * ox) + (oy * oy);
        const std::int64_t after = ((ox - dx) * (ox - dx)) + ((oy - dy) * (oy - dy));
        if (after < kRadius * kRadius && after < before) {
            return &other;
        }
    }
    return nullptr;
}

/// Whether a vehicle may be carried to (x, y): on the map and onto ground it can enter.
[[nodiscard]] bool can_stand(const WorldGround& ground, std::int64_t x, std::int64_t y) noexcept {
    if (x < 0 || y < 0 || x > kMaxPosRaw || y > kMaxPosRaw) {
        return false;
    }
    const Fixed fx = Fixed::from_raw(static_cast<std::int32_t>(x));
    const Fixed fy = Fixed::from_raw(static_cast<std::int32_t>(y));
    return ground.factor(fx.floor_int(), fy.floor_int()) != 0;
}

/// THE SHOVE. `step` is where `unit` is about to go. If that runs it into another vehicle, the
/// unit goes at `kShoveSpeedNumerator/kShoveSpeedDenominator` of the step and the vehicle is
/// carried the same distance, with half as much again to the side it already lies on so a head-on
/// pair slides past instead of cancelling out. Never a wall, three ways over:
///   * a vehicle that cannot be carried (the map's edge, a crater, a building) is simply driven
///     through, at the same reduced speed;
///   * a unit that has shoved for `kShoveYieldTicks` in a row stops colliding until it has been
///     clear of every vehicle for one tick;
///   * nothing is ever refused, so the step always lands.
/// `step` is rewritten to the step to take. Only the pusher moves a vehicle -- a shoved vehicle
/// does not shove in turn, so a convoy is not a chain reaction.
void shove_step(std::vector<Unit>& units, const WorldGround& ground, Unit& unit,
                Step& step) noexcept {
    Unit* other = vehicle_in_the_way(units, unit, step.dx, step.dy);
    if (other == nullptr) {
        unit.shove_ticks = 0;
        return;
    }
    if (unit.shove_ticks >= kShoveYieldTicks) {
        return; // yielding: through, at full speed, until clear
    }
    ++unit.shove_ticks;
    step.dx = step.dx * kShoveSpeedNumerator / kShoveSpeedDenominator;
    step.dy = step.dy * kShoveSpeedNumerator / kShoveSpeedDenominator;
    if (!can_stand(ground, unit.x.raw + step.dx, unit.y.raw + step.dy)) {
        step = Step{}; // the thinner step is not enterable; stand this tick, the next differs
        return;
    }
    // Which side the vehicle lies on, from the cross product of the step and the offset to it;
    // dead ahead (zero) is settled by id, which both vehicles agree on.
    const std::int64_t cross
        = (step.dx * (other->y.raw - unit.y.raw)) - (step.dy * (other->x.raw - unit.x.raw));
    const std::int64_t side = cross > 0 ? 1 : (cross < 0 ? -1 : (unit.id < other->id ? 1 : -1));
    const std::int64_t lx = -step.dy * side / 2;
    const std::int64_t ly = step.dx * side / 2;
    const std::array<Step, 3> carry{Step{step.dx + lx, step.dy + ly}, step, Step{lx, ly}};
    for (const Step& c : carry) {
        if (can_stand(ground, other->x.raw + c.dx, other->y.raw + c.dy)) {
            other->x = Fixed::from_raw(static_cast<std::int32_t>(other->x.raw + c.dx));
            other->y = Fixed::from_raw(static_cast<std::int32_t>(other->y.raw + c.dy));
            other->still_ticks = 0; // carried: a turret is unmounted
            return;
        }
    }
}

// Moves a unit by the step `plan_step` chose: straight on where it can, along a rim where it must,
// and around an obstacle it cannot pass. The choice itself lives in sim/steering.hpp, which is the
// seam a planned route will plug into. Another vehicle in the way is shoved, not walked through
// (`shove_step`).
void move(std::vector<Unit>& units, const WorldGround& ground, const UnitProfile& prof,
          Unit& unit) noexcept {
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
    Step step = plan.step;
    shove_step(units, ground, unit, step);
    unit.x = Fixed::from_raw(
        static_cast<std::int32_t>(std::clamp<std::int64_t>(unit.x.raw + step.dx, 0, kMaxPosRaw)));
    unit.y = Fixed::from_raw(
        static_cast<std::int32_t>(std::clamp<std::int64_t>(unit.y.raw + step.dy, 0, kMaxPosRaw)));
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

/// How long the friendly-fire probe flies a shot before it gives up: ten seconds, far past the
/// longest flight (a mortar lob lands in a couple), so a probe always ends with the shot.
constexpr int kLaneProbeTicks = 10 * kTicksPerSecond;

/// WOULD THIS SHOT HURT A UNIT ON THE SHOOTER'S OWN SIDE? (friendly fire: everybody should TRY to
/// avoid it. The damage is untouched -- a round that hits a friend still hurts it; this only lets a
/// gun decline the shot.) The shot is the very one `launch` builds for `target`, flown to its end
/// on a copy of the nearby units by the real flight (`advance_projectiles`), so there is one
/// geometry: a flat bolt is stopped by the first body in its lane, whichever side it is on (a
/// hostile in front shields the friend behind it), and a lobbed shell flies OVER the friends under
/// its arc and counts only the body it comes down on and the splash around the landing. The lane
/// is the bolt's own hit radius, not a wider one, so a gun does not refuse a shot that would have
/// missed. The shooter itself counts: a shell that comes down on its own gun is a friend hit.
/// A target that is itself on the shooter's side is a DELIBERATE shot (an explicit order on a
/// friendly): it is not second-guessed. Positions are as they are now; a friend who moves into the
/// lane afterwards is not foreseen.
[[nodiscard]] bool shot_hurts_friend(const std::vector<Unit>& units, const Unit& shooter,
                                     const Unit& target, std::uint16_t damage, std::uint64_t seed) {
    if (target.side == shooter.side) {
        return false;
    }
    const UnitSpec& weapon = spec_of(shooter.kind);
    // The aim estimate is about one target: judge the shot the gun would make at THIS one, not
    // the belief it holds about another. A copy, so the real gun's belief is not thrown away.
    Unit gun = shooter;
    retarget_aim(gun, target.id);
    Projectile bolt = launch(gun, target, damage, seed);
    bolt.learns = false; // a probe teaches nobody
    std::vector<Projectile> flight;
    flight.reserve(std::max<std::size_t>(1, weapon.pellets));
    fan_out(bolt, weapon.pellets, weapon.cone_half_tan, flight);

    // Only what the shot can reach: within its travel and its splash of the gun.
    const std::int64_t reach = static_cast<std::int64_t>(weapon.max_travel.raw)
                               + static_cast<std::int64_t>(weapon.splash_radius.raw);
    std::vector<Unit> near;
    near.reserve(units.size());
    for (const Unit& unit : units) {
        if (unit.alive && dist2(shooter.x, shooter.y, unit.x, unit.y) <= reach * reach) {
            near.push_back(unit);
        }
    }
    std::vector<Impact> impacts;
    for (int tick = 0; tick < kLaneProbeTicks && !flight.empty(); ++tick) {
        advance_projectiles(flight, near, impacts, seed);
    }
    return std::any_of(impacts.begin(), impacts.end(), [&](const Impact& impact) {
        return std::any_of(near.begin(), near.end(), [&](const Unit& unit) {
            return unit.id == impact.victim && unit.side == shooter.side;
        });
    });
}

} // namespace

std::int64_t service_repair_weight(const Unit& truck, const Unit& patient) noexcept {
    const std::int64_t radius_raw
        = static_cast<std::int64_t>(kServiceRepairRadiusTiles) * Fixed::kOne;
    const std::int64_t radius2 = radius_raw * radius_raw;
    const std::int64_t d2 = dist2(patient.x, patient.y, truck.x, truck.y);
    if (d2 > radius2) {
        return 0;
    }
    const std::int64_t distance
        = kServiceRepairFull - (kServiceRepairFull - kServiceRepairEdge) * d2 / radius2;
    // THE CONE. cos^2 of the angle between the truck's facing and the line to the patient, in
    // 65536ths, from the squared dot product over the squared lengths: no root, no float. The
    // line is read a 256th of a tile at a time so the squares fit; a patient nearer than that is
    // on the truck and takes the whole share.
    const bool moved = truck.face_x != 0 || truck.face_y != 0;
    const std::int64_t face_x = moved ? truck.face_x : 0;
    const std::int64_t face_y = moved ? truck.face_y : kServiceFacingScale; // +Y until it moves
    const std::int64_t to_x
        = (static_cast<std::int64_t>(patient.x.raw) - truck.x.raw) / kServiceConeStep;
    const std::int64_t to_y
        = (static_cast<std::int64_t>(patient.y.raw) - truck.y.raw) / kServiceConeStep;
    if (to_x == 0 && to_y == 0) {
        return distance;
    }
    const std::int64_t dot = (to_x * face_x) + (to_y * face_y);
    if (dot <= 0) {
        return 0; // abeam or behind: his "90 degrees, no effect" and "nothing BEHIND"
    }
    const std::int64_t cos2
        = (dot * dot * kServiceRepairFull)
          / (((to_x * to_x) + (to_y * to_y)) * ((face_x * face_x) + (face_y * face_y)));
    std::int64_t share = cos2;
    for (int power = 1; power < kServiceRepairConePower; ++power) {
        share = share * cos2 / kServiceRepairFull;
    }
    return distance * share / kServiceRepairFull;
}

namespace {

/// Points `truck` along (`vx`, `vy`), scaled so the larger component is `kServiceFacingScale`. No
/// motion leaves the facing as it was.
void face_along(Unit& truck, std::int64_t vx, std::int64_t vy) noexcept {
    const std::int64_t major = std::max(std::abs(vx), std::abs(vy));
    if (major == 0) {
        return;
    }
    truck.face_x = static_cast<std::int16_t>(vx * kServiceFacingScale / major);
    truck.face_y = static_cast<std::int16_t>(vy * kServiceFacingScale / major);
}

/// SERVICE TRUCKS. For each living, hurt unit, the sum over every OTHER living friendly truck of
/// `service_repair_weight`: the weight (`kServiceRepairFull` at the truck's own spot, falling
/// linearly in the squared distance to `kServiceRepairEdge` at `kServiceRepairRadiusTiles`) times
/// the cone's share. The
/// unit gains `max_hp * weight / (kServiceRepairTicks * kServiceRepairFull)` hit points that tick,
/// carried as a remainder (`repair_frac`) so a fraction of a hit point is not lost: a tank's
/// 100 / 600 is a sixth of one. One rule for every kind -- a building, a vehicle, a turret
/// mounted or not -- and the trucks add, so two mend twice as fast.
///
/// Ascending id and integers throughout, so the order of units is the only order there is. It runs
/// after the shooting: a unit shot dead this tick is already !alive and is skipped, and the hit
/// points land on what the tick's damage left. Last, each truck takes the direction it moved this
/// tick as its facing, so the cone a tick mends by is the one the tick before left, which is what
/// `game::repair_links` reads between steps.
void service_repair_step(std::vector<Unit>& units) {
    for (Unit& unit : units) {
        if (!unit.alive) {
            continue;
        }
        const std::uint16_t max_hp = profile(unit.kind).hp;
        if (unit.hp >= max_hp) {
            unit.repair_frac = 0;
            continue;
        }
        std::int64_t weight = 0;
        for (const Unit& truck : units) {
            if (!truck.alive || truck.kind != UnitKind::ServiceTruck || truck.side != unit.side
                || truck.id == unit.id) {
                continue;
            }
            weight += service_repair_weight(truck, unit);
        }
        if (weight == 0) {
            continue;
        }
        std::int64_t frac = unit.repair_frac + static_cast<std::int64_t>(max_hp) * weight;
        while (frac >= kServiceRepairHitPoint && unit.hp < max_hp) {
            frac -= kServiceRepairHitPoint;
            ++unit.hp;
        }
        unit.repair_frac = unit.hp >= max_hp ? 0 : static_cast<std::uint32_t>(frac);
    }
    for (Unit& truck : units) {
        if (truck.alive && truck.kind == UnitKind::ServiceTruck) {
            face_along(truck, truck.vx.raw, truck.vy.raw);
        }
    }
}

} // namespace

Sim Sim::create(const Scenario& scenario) {
    Sim sim;
    sim.scenario_ = scenario;
    sim.ore_.at(static_cast<std::size_t>(Side::Player)) = scenario.start_ore;
    sim.units_.reserve(scenario.spawns.size());
    for (const Spawn& spawn : scenario.spawns) {
        // Out-of-range spawns are clamped onto the map rather than rejected: total, not UB.
        const int x = std::clamp(spawn.tile_x, 0, kMapTiles - 1);
        const int y = std::clamp(spawn.tile_y, 0, kMapTiles - 1);
        const std::span<const Unit> placed{sim.units_};
        Coord tile{};
        if (has_footprint(spawn.kind)) {
            // A building is placed whole: its north-west tile is the wish, or the nearest one whose
            // square is clear, and it stands at that square's centre.
            const Coord corner = nearest_where(x, y, [&scenario, placed](int tx, int ty) {
                return building_fits(scenario.seed, placed, tx, ty);
            });
            const Footprint site = footprint_at(corner.x, corner.y);
            tile = Coord{site.centre_x(), site.centre_y()};
        } else {
            tile = nearest_passable(scenario.seed, placed, x, y);
        }
        sim.units_.push_back(fresh_unit(sim.next_id_++, spawn.kind, spawn.side, tile));
        if (spawn.hp != 0) {
            sim.units_.back().hp = std::min(spawn.hp, sim.units_.back().hp);
        }
        if (spawn.kind == UnitKind::ServiceTruck) {
            face_along(sim.units_.back(), spawn.face_x, spawn.face_y);
        }
    }
    // C0: the ground the battle starts on. The scenario placed it as a pure function of its seed;
    // it settles here because settling needs the units that now stand on the map (a piece under a
    // hull is invisible and unfetchable at once).
    sim.scraps_ = scenario.scrap;
    settle_litter(sim.scraps_, 0, scenario.seed, sim.units_, -1, -1);
    return sim;
}

void Sim::enqueue(const Order& order) {
    // The generic command channel: the human's input layer, a replay being replayed, and an AI all
    // arrive here. The sim has no idea WHO is issuing, so it must not filter by side -- ownership
    // is the commander's responsibility. Filtering here silently killed the enemy commander.
    if (!order_is_valid(units_, scenario_.pools, order, scenario_.seed)) {
        return;
    }
    // A build is judged against the queue AS IT WILL BE when this order is applied: the builds
    // already pending for the same producer are laid onto a copy first. Without that, the second
    // of two conflicting orders enqueued in one tick passed here, was dropped when applied, and
    // had already reached the log -- and so the replay -- as an order the sim never took.
    if (const auto* build = std::get_if<orders::Build>(&order.payload)) {
        BuildQueue queue = find_const(units_, order.unit)->queue;
        for (const Order& earlier : pending_) {
            const auto* before = std::get_if<orders::Build>(&earlier.payload);
            if (before != nullptr && earlier.unit == order.unit
                && build_fits(queue, before->kind, before->batch)) {
                // A lone build's id is above every token, so any stand-in above them matches none.
                queue.entries.at(queue.count) = BuildEntry{
                    before->kind, before->batch != 0 ? before->batch : kFirstLoneBatch};
                ++queue.count;
            }
        }
        if (!build_fits(queue, build->kind, build->batch)) {
            return;
        }
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
        apply_order(units_, projectiles_, shots_, driven_, lone_batches_, ore_, scenario_.pools,
                    order, scenario_.seed);
    }
    pending_.clear();

    // 2. movement, ascending id (units_ is stored in id order).
    const std::uint64_t seed = scenario_.seed;
    for (Unit& unit : units_) {
        if (!unit.alive) {
            continue;
        }
        // The deploy, before the mover: it may stand the recycler still, or turn it into a
        // building that does not move at all.
        if (unit.kind == UnitKind::Recycler && deploy_step(units_, unit, seed)) {
            continue;
        }
        if (unit.kind == UnitKind::Scavenger
            && scavenger_deploy_step(units_, scenario_.pools, unit)) {
            continue;
        }
        if (is_turret(unit.kind) && turret_deploy_step(units_, unit, seed)) {
            continue;
        }
        if (recycle_step(units_, scraps_, unit, ore_, scenario_.store_capacity, seed)) {
            continue;
        }
        // Asked afresh for each unit, so a recycler that deployed earlier in this very loop is
        // already in the way of every unit after it.
        const WorldGround ground = ground_of(seed, units_);
        const UnitProfile prof = profile(unit.kind);
        // Scavenging, before the mover: it takes from where the unit stands and sets no goal (C2).
        if (unit.kind == UnitKind::Scavenger) {
            auto_deploy_step(units_, scenario_.pools, scraps_, unit, tick_);
            scavenge_step(scraps_, units_, ore_, collected_, unit, tick_,
                          store_capacity(units_, unit.side, scenario_.store_capacity));
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
        move(units_, ground, prof, unit);
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
                && dist2(unit.x, unit.y, focused->x, focused->y) <= range * range
                && !shot_hurts_friend(units_, unit, *focused, prof.damage, seed)) {
                best = focused;
            }
        }
        // An explicit Attack order outranks auto-engagement: while the named target is alive, in
        // range and in sight the gun engages it, even when a closer hostile is in range too.
        if (unit.stance == Stance::Attack && unit.target.valid()) {
            Unit* named = find_mut(units_, unit.target);
            if (named != nullptr && named->alive
                && dist2(unit.x, unit.y, named->x, named->y) <= range * range
                && has_line(unit, *named, seed)
                && !shot_hurts_friend(units_, unit, *named, prof.damage, seed)) {
                best = named;
            }
        }
        if (best == nullptr && unit.id != driven_) {
            std::int64_t best_d2 = range * range + 1;
            for (Unit& other : units_) {
                if (!other.alive || other.side == unit.side) {
                    continue;
                }
                const std::int64_t d2 = dist2(unit.x, unit.y, other.x, other.y);
                // A gun does not shoot a hill: without a line the enemy is not a target, and a
                // farther one it can see is.
                // Nor does it shoot through a friend: a candidate whose shot would hurt one is
                // passed over for the next (a gun TRIES not to; the damage itself is unchanged).
                if (d2 <= range * range && d2 < best_d2 // strict: ties keep the lowest id
                    && has_line(unit, other, seed)
                    && !shot_hurts_friend(units_, unit, other, prof.damage, seed)) {
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
        fan_out(launch(unit, *best, prof.damage, seed), spec_of(unit.kind).pellets,
                spec_of(unit.kind).cone_half_tan, projectiles_);
        shots_.push_back(Shot{unit.id, best->id});
        unit.cooldown = prof.cooldown_ticks;
        // The bike scoots the moment it has fired: the spot it will reload at, for `steer`. Only
        // an engaging bike (an Attack stance) does; one on a guard post fires from it.
        if (scoots(unit.kind) && unit.stance == Stance::Attack) {
            if (const std::optional<ScootSpot> spot
                = scoot_spot(seed, units_, unit, *best, tick_)) {
                unit.guard_x = spot->x;
                unit.guard_y = spot->y;
            }
        }
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

    // 5b. THE TRUCKS. Every friendly truck in reach mends what is hurt, on every tick.
    service_repair_step(units_);

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
                settle_litter(scraps_, first, scenario_.seed, units_, dead->x.floor_int(),
                              dead->y.floor_int());
            }
        }
    }
    // 6b. THE STORE: the bank is clamped to what the side can hold, then the side's deployed
    //     scavengers extract on the global beat (see `store_step`).
    store_step(units_, ore_, collected_, scenario_.store_capacity, tick_);
    // 7. THE QUEUES (BUILD-QUEUE). Every producer's line runs, in ascending id, over the producers
    //    that existed when the tick began, because an arrival appends to the table and a new unit
    //    is nobody's producer yet.
    for (std::size_t i = 0, n = units_.size(); i < n; ++i) {
        production_step(units_, ore_, next_id_, scenario_.seed, i);
    }
    ++tick_;
}

std::uint32_t Sim::tick() const noexcept { return tick_; }

std::uint32_t Sim::route_plans() const noexcept { return route_plans_; }

namespace {

/// `kind_wiped_out` for every kind `is_one` accepts: the side had at least one, and none is alive.
template <class Is> bool wiped_out(const std::vector<Unit>& units, Side side, Is is_one) noexcept {
    bool had_one = false;
    for (const Unit& unit : units) {
        if (unit.side != side || !is_one(unit.kind)) {
            continue;
        }
        if (unit.alive) {
            return false;
        }
        had_one = true; // a dead unit stays in the table, so "had one" needs no second record
    }
    return had_one;
}

/// ONE UNIT, TWO FORMS. Deploying changes the KIND of the same unit: the recycler becomes the
/// deployed recycler. A condition that named the vehicle alone would therefore forget a deployed
/// one, and that unit's death would decide nothing -- so a condition on the recycler accepts both
/// forms, which is the whole of this rule and the only place it lives.
bool same_unit(const UnitKind asked, const UnitKind actual) noexcept {
    return actual == asked || (asked == UnitKind::Recycler && actual == UnitKind::DeployedRecycler);
}

} // namespace

bool Sim::kind_wiped_out(Side side, UnitKind kind) const noexcept {
    return wiped_out(units_, side, [kind](UnitKind k) { return same_unit(kind, k); });
}

WarOutcome Sim::war_outcome() const noexcept {
    // The scenario's own list, asked in order: the first condition that fires hands down the
    // verdict and the rest are never reached. The built-in list puts the player's loss first, so a
    // recycler pair that dies together reads as a loss. A scenario with no conditions is a sandbox:
    // it is never decided, which is a scenario rather than an error.
    for (const Objective& objective : scenario_.objectives) {
        if (kind_wiped_out(objective.side, objective.kind)) {
            return objective.verdict;
        }
    }
    return WarOutcome::Undecided;
}

std::uint64_t Sim::seed() const noexcept { return scenario_.seed; }

bool tile_passable(std::uint64_t seed, int tile_x, int tile_y) noexcept {
    return tile_x >= 0 && tile_y >= 0 && tile_x < kMapTiles && tile_y < kMapTiles
           && terrain_speed_factor(world::sample(seed, tile_x, tile_y).terrain).raw != 0;
}

bool building_blocks(std::span<const Unit> units, int tile_x, int tile_y) noexcept {
    return std::any_of(units.begin(), units.end(), [tile_x, tile_y](const Unit& unit) {
        return unit.alive && has_footprint(unit.kind)
               && footprint_around(unit.x, unit.y).covers(tile_x, tile_y);
    });
}

bool deploy_site_ok(std::uint64_t seed, std::span<const Unit> units, int tile_x,
                    int tile_y) noexcept {
    if (tile_x < 0 || tile_y < 0 || tile_x >= kMapTiles || tile_y >= kMapTiles) {
        return false; // aimed off the map: refused, never clipped back onto it
    }
    const Footprint site = footprint_at(tile_x, tile_y);
    for (int dy = 0; dy < kBuildingFootprintTiles; ++dy) {
        for (int dx = 0; dx < kBuildingFootprintTiles; ++dx) {
            if (!standable(seed, units, site.x + dx, site.y + dy)) {
                return false;
            }
        }
    }
    return true;
}

const std::vector<Unit>& Sim::units() const noexcept { return units_; }

const Unit* Sim::find(UnitId id) const noexcept { return find_const(units_, id); }

const std::vector<Projectile>& Sim::projectiles() const noexcept { return projectiles_; }

const std::vector<Scrap>& Sim::scraps() const noexcept { return scraps_; }

std::uint16_t Sim::ore(Side side) const noexcept { return ore_.at(static_cast<std::size_t>(side)); }

std::uint32_t Sim::collected(Side side) const noexcept {
    return collected_.at(static_cast<std::size_t>(side));
}

std::uint16_t Sim::capacity(Side side) const noexcept {
    return store_capacity(units_, side, scenario_.store_capacity);
}

std::uint16_t Sim::fast_band(Side side) const noexcept {
    return store_fast_band(units_, side, scenario_.store_capacity);
}

const std::vector<Pool>& Sim::pools() const noexcept { return scenario_.pools; }

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
        if (u.fetch_beats != 0) {
            h.u64(u.fetch_beats); // only while on a trip: a battle with none hashes as before
        }
        if (u.shunned_count != 0) {
            h.u64(u.shunned_count); // only once it has given up a pile
            for (const TileXY& tile : u.shunned) {
                h.u64(tile.x);
                h.u64(tile.y);
            }
        }
        if (u.shove_ticks != 0) {
            h.u64(u.shove_ticks); // only when set: a battle that never shoves hashes as before
        }
        if (u.repair_frac != 0) {
            h.u64(u.repair_frac); // only when set: a battle no truck mends in hashes as before
        }
        if (u.face_x != 0 || u.face_y != 0) {
            h.i32(u.face_x); // a truck that has moved: no other unit's is ever set
            h.i32(u.face_y);
        }
        if (u.heading_x != 0 || u.heading_y != 0) {
            h.i32(u.heading_x); // a turn-limited machine that has turned: no other unit's is set
            h.i32(u.heading_y);
        }
        if (u.deploy_ticks != 0) {
            h.u64(
                u.deploy_ticks); // only when set too: a battle that never deploys hashes as before
        }
        // The batch a unit was built in, and a producer's queue: only for a unit that has either,
        // so a battle that queues nothing hashes exactly as it did before the queue existed.
        if (u.batch != 0 || u.queue.count != 0) {
            h.u64(u.batch);
            h.u64(u.queue.count);
            h.u64(u.queue.left);
            for (std::size_t i = 0; i < u.queue.count; ++i) {
                h.u64(static_cast<std::uint64_t>(u.queue.entries.at(i).kind));
                h.u64(u.queue.entries.at(i).batch);
            }
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
                h.i32(p.flown);
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
    // How many lone builds have been numbered: the next one's id depends on it. Only once one has.
    if (lone_batches_ != 0) {
        h.u64(lone_batches_);
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
