#include "protekt/cmd/controller.hpp"

#include <algorithm>

#include "protekt/cmd/command.hpp"

namespace protekt::cmd {

namespace {

/// Squared distance in raw Q16.16 units between a tile and a fixed-point point. 64-bit, so it
/// cannot overflow.
[[nodiscard]] std::int64_t distance_sq(const ai::UnitView& unit, sim::Fixed x,
                                       sim::Fixed y) noexcept {
    const std::int64_t dx = (static_cast<std::int64_t>(unit.x) * sim::Fixed::kOne) - x.raw;
    const std::int64_t dy = (static_cast<std::int64_t>(unit.y) * sim::Fixed::kOne) - y.raw;
    return (dx * dx) + (dy * dy);
}

[[nodiscard]] const ai::UnitView* find_view(const std::vector<ai::UnitView>& views,
                                            sim::UnitId id) noexcept {
    const auto at = std::find_if(views.begin(), views.end(),
                                 [id](const ai::UnitView& view) { return view.id == id; });
    return at == views.end() ? nullptr : &*at;
}

void append(std::vector<sim::Order>& out, const std::vector<sim::Order>& orders) {
    out.insert(out.end(), orders.begin(), orders.end());
}

} // namespace

std::optional<Controller> Controller::create(const Policy& policy,
                                             std::vector<sim::UnitId> roster) {
    if (policy.cadence_ticks == 0) {
        return std::nullopt;
    }
    std::sort(roster.begin(), roster.end());
    roster.erase(std::unique(roster.begin(), roster.end()), roster.end());
    roster.erase(roster.begin(), std::find_if(roster.begin(), roster.end(),
                                              [](sim::UnitId id) { return id.valid(); }));
    if (roster.empty()) {
        return std::nullopt;
    }
    Controller controller;
    controller.policy_ = policy;
    controller.roster_ = std::move(roster);
    return controller;
}

bool Controller::due(sim::Tick tick) const noexcept {
    return tick != 0 && tick % policy_.cadence_ticks == 0;
}

void Controller::think(const ai::Observation& observation, sim::Tick tick,
                       std::vector<sim::Order>& out) {
    if (!due(tick)) {
        return;
    }
    std::vector<sim::UnitId> living;
    for (const sim::UnitId id : roster_) {
        if (find_view(observation.own, id) != nullptr) {
            living.push_back(id);
        }
    }
    if (living.empty()) {
        return;
    }
    switch (policy_.kind) {
    case PolicyKind::Hold:
        append(out, guard_at(living, policy_.x, policy_.y));
        break;
    case PolicyKind::Deny: {
        // Squared Euclidean, as the contract names it: a radius is a circle.
        constexpr std::int64_t kRadiusRaw
            = static_cast<std::int64_t>(kDenyRadiusTiles) * sim::Fixed::kOne;
        constexpr std::int64_t kRadiusSq = kRadiusRaw * kRadiusRaw;
        for (const sim::UnitId id : living) {
            const ai::UnitView* view = find_view(observation.own, id);
            const std::vector<sim::UnitId> one(1, id);
            if (distance_sq(*view, policy_.x, policy_.y) <= kRadiusSq) {
                append(out,
                       guard_at(one, sim::Fixed::from_int(view->x), sim::Fixed::from_int(view->y)));
            } else {
                append(out, move_to(one, policy_.x, policy_.y));
            }
        }
        break;
    }
    case PolicyKind::Focus: {
        const ai::UnitView* nearest = nullptr;
        std::int64_t nearest_d2 = 0;
        for (const ai::UnitView& enemy : observation.enemy) {
            const std::int64_t d2 = distance_sq(enemy, policy_.x, policy_.y);
            if (nearest == nullptr || d2 < nearest_d2
                || (d2 == nearest_d2 && enemy.id < nearest->id)) {
                nearest = &enemy;
                nearest_d2 = d2;
            }
        }
        if (nearest != nullptr) {
            append(out, attack(living, nearest->id));
        }
        break;
    }
    }
}

const Policy& Controller::policy() const noexcept { return policy_; }
std::span<const sim::UnitId> Controller::roster() const noexcept { return roster_; }

std::vector<sim::UnitId> ControllerSet::add(Controller controller) {
    std::vector<sim::UnitId> refused;
    std::vector<sim::UnitId> kept;
    for (const sim::UnitId id : controller.roster()) {
        (owner_of(id).has_value() ? refused : kept).push_back(id);
    }
    if (refused.empty()) {
        controllers_.push_back(std::move(controller)); // nothing taken away: keep it as given
    } else if (auto filtered = Controller::create(controller.policy(), std::move(kept))) {
        controllers_.push_back(std::move(*filtered));
    }
    return refused;
}

bool ControllerSet::empty() const noexcept { return controllers_.empty(); }
std::size_t ControllerSet::size() const noexcept { return controllers_.size(); }
std::span<const Controller> ControllerSet::controllers() const noexcept { return controllers_; }

std::optional<std::size_t> ControllerSet::owner_of(sim::UnitId id) const noexcept {
    for (std::size_t i = 0; i < controllers_.size(); ++i) {
        const auto roster = controllers_[i].roster();
        if (std::binary_search(roster.begin(), roster.end(), id)) {
            return i;
        }
    }
    return std::nullopt;
}

void ControllerSet::think_due(const ai::Observation& observation, sim::Tick tick,
                              std::vector<sim::Order>& out) {
    for (Controller& controller : controllers_) {
        controller.think(observation, tick, out);
    }
}

} // namespace protekt::cmd
