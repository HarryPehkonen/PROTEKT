#include "protekt/cmd/selection.hpp"

#include <algorithm>

namespace protekt::cmd {

bool Selection::add(sim::UnitId id) noexcept {
    if (!id.valid() || full()) {
        return false;
    }
    const auto at = std::lower_bound(ids_.begin(), ids_.end(), id);
    if (at != ids_.end() && *at == id) {
        return false;
    }
    ids_.insert(at, id);
    return true;
}

bool Selection::toggle(sim::UnitId id) noexcept {
    if (remove(id)) {
        return false;
    }
    return add(id);
}

bool Selection::remove(sim::UnitId id) noexcept {
    const auto at = std::lower_bound(ids_.begin(), ids_.end(), id);
    if (at == ids_.end() || *at != id) {
        return false;
    }
    ids_.erase(at);
    return true;
}

void Selection::clear() noexcept { ids_.clear(); }

bool Selection::contains(sim::UnitId id) const noexcept {
    return std::binary_search(ids_.begin(), ids_.end(), id);
}

bool Selection::empty() const noexcept { return ids_.empty(); }
std::size_t Selection::size() const noexcept { return ids_.size(); }
std::span<const sim::UnitId> Selection::units() const noexcept { return ids_; }
bool Selection::full() const noexcept { return ids_.size() >= kMax; }

void Selection::retain(std::span<const sim::UnitId> living) noexcept {
    std::erase_if(ids_, [living](sim::UnitId id) {
        return std::find(living.begin(), living.end(), id) == living.end();
    });
}

} // namespace protekt::cmd
