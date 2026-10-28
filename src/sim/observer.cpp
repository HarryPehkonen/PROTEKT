#include "protekt/sim/observer.hpp"

#include "protekt/sim/spec.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <map>
#include <string>
#include <variant>

namespace protekt::sim {

namespace {

// BattleLog is frozen at {ticks, seed, events, final_hash}, so the unit states that the status
// lines print have to live in `events` too. Two private event kinds past the public enum carry
// them: a "status position" event (actor = unit, subject = raw x, subject_hp = hp) is always
// followed by a "status attributes" event (actor = unit, subject = raw y, subject_hp = kind |
// side << 6 | stance << 8). They are recorded only when a unit's state changed since its last
// record, and to_text() replays them to know every unit's state at any tick, whatever status
// period it is asked for.
constexpr EventKind kStatusPos = static_cast<EventKind>(0x80);
constexpr EventKind kStatusAttr = static_cast<EventKind>(0x81);

bool is_status(EventKind kind) noexcept { return kind == kStatusPos || kind == kStatusAttr; }

struct UnitState {
    std::int32_t x{};
    std::int32_t y{};
    std::uint16_t hp{};
    std::uint16_t attrs{};
};

UnitState state_of(const Unit& u) noexcept {
    return UnitState{u.x.raw, u.y.raw, u.hp, pack_unit_attrs(u.kind, u.side, u.stance)};
}

bool same(const UnitState& a, const UnitState& b) noexcept {
    return a.x == b.x && a.y == b.y && a.hp == b.hp && a.attrs == b.attrs;
}

UnitKind kind_of(std::uint16_t attrs) noexcept { return unpack_unit_kind(attrs); }
Side side_of(std::uint16_t attrs) noexcept { return unpack_unit_side(attrs); }
Stance stance_of(std::uint16_t attrs) noexcept { return unpack_unit_stance(attrs); }

/// Records the units whose state differs from the last record (everything, the first time).
void record_status(const Sim& sim, std::vector<UnitState>& last, std::vector<bool>& seen,
                   BattleLog& log) {
    const std::vector<Unit>& units = sim.units();
    last.resize(units.size());
    seen.resize(units.size(), false);
    for (std::size_t i = 0; i < units.size(); ++i) {
        const UnitState now = state_of(units[i]);
        if (seen[i] && same(last[i], now)) {
            continue;
        }
        seen[i] = true;
        last[i] = now;
        log.events.push_back(BattleEvent{sim.tick(), kStatusPos, units[i].id,
                                         UnitId{static_cast<std::uint32_t>(now.x)}, now.hp});
        log.events.push_back(BattleEvent{sim.tick(), kStatusAttr, units[i].id,
                                         UnitId{static_cast<std::uint32_t>(now.y)}, now.attrs});
    }
}

/// Whom an order shoots at; "no unit" for the kinds that name no victim.
UnitId ordered_target(const Order& order) {
    if (const auto* attack = std::get_if<orders::Attack>(&order.payload)) {
        return attack->target;
    }
    if (const auto* fire = std::get_if<orders::FocusFire>(&order.payload)) {
        return fire->target;
    }
    return UnitId{};
}

/// Appends accepted orders (Sim's command log grew) as Ordered events.
void record_orders(const Sim& sim, std::size_t& logged, BattleLog& log) {
    const std::vector<Command>& commands = sim.command_log();
    for (; logged < commands.size(); ++logged) {
        const Command& c = commands[logged];
        log.events.push_back(BattleEvent{c.tick, EventKind::Ordered, c.order.unit,
                                         ordered_target(c.order),
                                         static_cast<std::uint16_t>(c.order.kind())});
    }
}

void command_phase(Sim& sim, sim::ICommander* commander, Side side, std::vector<Order>& orders) {
    if (commander == nullptr) {
        return;
    }
    orders.clear();
    commander->think(sim::observe(sim, side), orders);
    for (const Order& order : orders) {
        sim.enqueue(order);
    }
}

void append_number(std::string& out, std::uint64_t value) { out += std::to_string(value); }

void append_status_lines(std::string& out, Tick tick,
                         const std::map<std::uint32_t, UnitState>& units) {
    for (const auto& [id, s] : units) {
        if (stance_of(s.attrs) == Stance::Dead || s.hp == 0) {
            continue;
        }
        out += "tick ";
        append_number(out, tick);
        out += " status unit ";
        append_number(out, id);
        out += ' ';
        out += to_string(kind_of(s.attrs));
        out += " side ";
        out += to_string(side_of(s.attrs));
        out += " pos ";
        append_tiles(out, s.x);
        out += ' ';
        append_tiles(out, s.y);
        out += " hp ";
        append_number(out, s.hp);
        out += " stance ";
        out += to_string(stance_of(s.attrs));
        out += '\n';
    }
}

} // namespace

void append_tiles(std::string& out, std::int32_t raw) {
    std::int64_t tenths = (static_cast<std::int64_t>(raw) * 10) >> Fixed::kShift;
    if (tenths < 0) {
        out += '-';
        tenths = -tenths;
    }
    append_number(out, static_cast<std::uint64_t>(tenths / 10));
    out += '.';
    out += static_cast<char>('0' + tenths % 10);
}

std::uint16_t pack_unit_attrs(UnitKind kind, Side side, Stance stance) noexcept {
    return static_cast<std::uint16_t>((static_cast<unsigned>(kind) & kKindMask)
                                      | ((static_cast<unsigned>(side) & kSideMask) << kSideShift)
                                      | (static_cast<unsigned>(stance) << kStanceShift));
}

UnitKind unpack_unit_kind(std::uint16_t attrs) noexcept {
    return static_cast<UnitKind>(attrs & kKindMask);
}
Side unpack_unit_side(std::uint16_t attrs) noexcept {
    return static_cast<Side>((attrs >> kSideShift) & kSideMask);
}
Stance unpack_unit_stance(std::uint16_t attrs) noexcept {
    return static_cast<Stance>((attrs >> kStanceShift) & 0xFFU);
}

std::vector<BattleEvent> events_between(const UnitSnapshot& before, const Sim& sim, Tick tick) {
    // Two things happened this tick that a diff of units cannot see on its own, and the sim reports
    // both itself: bolts that LANDED (Sim::impacts, because which of two bolts hit which unit is
    // not recoverable from hit points alone) and guns that FIRED (Sim::shots, because a cooldown
    // going up does not say whether the shot had a target). Landings come first because they run
    // first in a tick, and they are what killed anything that died, so the shots are read against
    // the same table.
    std::vector<BattleEvent> events;
    const std::vector<Unit>& after = sim.units();
    // A BUILD ADDS A UNIT (C4): the table GROWS by one on the production beat, so a size mismatch
    // is NOT proof of a foreign snapshot. Swallowing that tick's events (which is what this guard
    // did while the table was append-only by assumption) dropped every shot and kill of a build
    // tick out of the replay log and the sound. Only a table that SHRANK is foreign: the sim
    // appends and never removes a unit. Reading `before[index]` is still safe on a grown table --
    // the appended unit is the LAST entry, so every index below `before.size()` names the unit it
    // always did, and a unit built this tick cannot have been fired at by a shot resolved earlier
    // in it.
    if (after.size() < before.size()) {
        return events;
    }
    for (const Impact& impact : sim.impacts()) {
        events.push_back(BattleEvent{tick, EventKind::HitTaken, impact.shooter, impact.victim,
                                     impact.victim_hp});
        if (impact.victim_hp == 0) {
            events.push_back(
                BattleEvent{tick, EventKind::Killed, impact.shooter, impact.victim, 0});
        }
    }
    for (const Sim::Shot& shot : sim.shots()) {
        if (!shot.target.valid()) {
            continue; // a shot at nothing: the sim has nothing to attribute it to
        }
        const Unit* target = sim.find(shot.target);
        if (target == nullptr) {
            continue;
        }
        // The target's hit points as the shot leaves the gun, read from BEFORE the tick: a bolt
        // fired earlier may land on it in this very step, and that damage is HitTaken's to report,
        // not this shot's. This is the value the stream has always carried.
        const std::size_t index = static_cast<std::size_t>(target - after.data());
        events.push_back(
            BattleEvent{tick, EventKind::Fired, shot.shooter, shot.target, before[index].hp});
    }
    return events;
}

std::string BattleLog::to_text(Tick status_period) const {
    std::string out;
    std::map<std::uint32_t, UnitState> units;
    std::size_t next = 0;

    const auto apply_status = [&units](const BattleEvent& e) {
        UnitState& s = units[e.actor.value];
        if (e.kind == kStatusPos) {
            s.x = static_cast<std::int32_t>(e.subject.value);
            s.hp = e.subject_hp;
        } else {
            s.y = static_cast<std::int32_t>(e.subject.value);
            s.attrs = e.subject_hp;
        }
    };

    for (Tick t = 0; t <= ticks; ++t) {
        while (next < events.size() && events[next].tick <= t && is_status(events[next].kind)) {
            apply_status(events[next]);
            ++next;
        }
        if (t == 0 || (status_period != 0 && t % status_period == 0)) {
            append_status_lines(out, t, units);
        }
        while (next < events.size() && events[next].tick <= t) {
            const BattleEvent& e = events[next++];
            if (is_status(e.kind)) {
                apply_status(e);
                continue;
            }
            out += "tick ";
            append_number(out, e.tick);
            switch (e.kind) {
            case EventKind::Ordered:
                out += " ordered unit ";
                append_number(out, e.actor.value);
                out += ' ';
                out += to_string(static_cast<OrderKind>(e.subject_hp));
                break;
            case EventKind::Fired:
                out += " fired unit ";
                append_number(out, e.actor.value);
                out += " -> unit ";
                append_number(out, e.subject.value);
                out += " hp ";
                append_number(out, e.subject_hp);
                break;
            case EventKind::HitTaken:
                out += " hit unit ";
                append_number(out, e.subject.value);
                out += " by unit ";
                append_number(out, e.actor.value);
                out += " hp ";
                append_number(out, e.subject_hp);
                break;
            case EventKind::Killed:
                out += " killed unit ";
                append_number(out, e.subject.value);
                out += " by unit ";
                append_number(out, e.actor.value);
                break;
            }
            out += '\n';
        }
        if (t == std::numeric_limits<Tick>::max()) {
            break;
        }
    }

    std::uint64_t living = 0;
    for (const auto& [id, s] : units) {
        if (stance_of(s.attrs) != Stance::Dead && s.hp != 0) {
            ++living;
        }
    }
    out += "end tick ";
    append_number(out, ticks);
    out += " seed ";
    append_number(out, seed);
    out += " units ";
    append_number(out, living);
    out += " hash ";
    append_number(out, final_hash);
    out += '\n';
    return out;
}

BattleLog run_battle(const Scenario& scenario, std::span<const Command> script, Tick ticks,
                     sim::ICommander* player, sim::ICommander* enemy) {
    std::vector<Command> commands(script.begin(), script.end());
    std::stable_sort(commands.begin(), commands.end(),
                     [](const Command& a, const Command& b) { return a.tick < b.tick; });

    Sim sim = Sim::create(scenario);
    BattleLog log;
    log.ticks = ticks;
    log.seed = scenario.seed;

    std::vector<UnitState> last;
    std::vector<bool> seen;
    std::vector<Order> orders;
    std::size_t next_command = 0;
    std::size_t logged = 0;

    for (Tick t = 0; t < ticks; ++t) {
        record_status(sim, last, seen, log);
        while (next_command < commands.size() && commands[next_command].tick <= t) {
            sim.enqueue(commands[next_command].order);
            ++next_command;
        }
        command_phase(sim, player, Side::Player, orders);
        command_phase(sim, enemy, Side::Enemy, orders);
        record_orders(sim, logged, log);

        const UnitSnapshot before = sim.units();
        sim.step();
        for (const BattleEvent& event : events_between(before, sim, t)) {
            log.events.push_back(event);
        }
    }
    record_status(sim, last, seen, log);
    log.final_hash = sim.state_hash();
    return log;
}

const char* to_string(UnitKind kind) noexcept {
    if (static_cast<std::size_t>(kind) >= kUnitSpecs.size()) {
        return "unknown";
    }
    return spec_of(kind).name;
}

const char* to_string(Side side) noexcept {
    switch (side) {
    case Side::Player:
        return "player";
    case Side::Enemy:
        return "enemy";
    }
    return "unknown";
}

const char* to_string(Stance stance) noexcept {
    switch (stance) {
    case Stance::Idle:
        return "idle";
    case Stance::MoveTo:
        return "move";
    case Stance::GuardAt:
        return "guard";
    case Stance::Patrol:
        return "patrol";
    case Stance::Attack:
        return "attack";
    case Stance::Dead:
        return "dead";
    case Stance::Follow:
        return "follow";
    case Stance::Fetch:
        return "fetch";
    case Stance::Deploy:
        return "deploy";
    case Stance::Recycle:
        return "recycle";
    }
    return "unknown";
}

const char* to_string(OrderKind kind) noexcept {
    switch (kind) {
    case OrderKind::MoveTo:
        return "move";
    case OrderKind::GuardAt:
        return "guard";
    case OrderKind::Patrol:
        return "patrol";
    case OrderKind::Attack:
        return "attack";
    case OrderKind::FocusFire:
        return "focus";
    case OrderKind::FireTowards:
        return "firetowards";
    case OrderKind::Strafe:
        return "strafe";
    case OrderKind::Stop:
        return "stop";
    case OrderKind::Follow:
        return "follow";
    case OrderKind::FireLob:
        return "firelob";
    case OrderKind::Deploy:
        return "deploy";
    case OrderKind::Recycle:
        return "recycle";
    case OrderKind::FireFan:
        return "firefan";
    case OrderKind::FireRocket:
        return "firerocket";
    }
    return "unknown";
}

/// What a commander is allowed to see. It is the SIM that decides this, which is why it is built
/// here rather than in a commander's translation unit: a commander gets a snapshot of the living
/// units it may know about, and nothing else.
Observation observe(const Sim& sim, Side side) {
    Observation observation{};
    observation.tick = sim.tick();
    observation.terrain = sim.seed();
    for (const Unit& unit : sim.units()) {
        if (!unit.alive) {
            continue;
        }
        const UnitView view{unit.id, unit.kind,  unit.side,  unit.x.floor_int(), unit.y.floor_int(),
                            unit.hp, unit.x.raw, unit.y.raw, is_mounted(unit)};
        (unit.side == side ? observation.own : observation.enemy).push_back(view);
    }
    return observation;
}

} // namespace protekt::sim
