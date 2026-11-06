#include "protekt/game/repair_aim.hpp"

#include "protekt/game/attack_aim.hpp"
#include "protekt/sim/spec.hpp"

namespace protekt::game {

sim::UnitId repair_target(const sim::Sim& sim, const SceneOptions& options,
                          std::span<const sim::UnitId> selected, sim::UnitId driven) noexcept {
    if (!options.aim.has_value()) {
        return sim::UnitId{};
    }
    // The trucks that could be sent, kept as ids so the patient can be taken out of them.
    const auto truck_other_than = [&](sim::UnitId patient) {
        for (const sim::UnitId id : selected) {
            const sim::Unit* unit = sim.find(id);
            if (unit != nullptr && unit->alive && unit->side == sim::Side::Player && id != driven
                && id != patient && unit->kind == sim::UnitKind::ServiceTruck) {
                return true;
            }
        }
        return false;
    };
    const Pick picked = pick(sim, options, options.aim->x, options.aim->y);
    sim::UnitId candidate{};
    if (picked.has_unit()) {
        const sim::Unit* hit = sim.find(picked.unit);
        if (hit != nullptr && hit->alive) {
            candidate = hit->side == sim::Side::Player ? hit->id : sim::UnitId{};
        }
    } else {
        candidate = friendly_nearest_ray(sim, options, kAttackAimToleranceDegrees);
    }
    const sim::Unit* patient = sim.find(candidate);
    if (patient == nullptr || !patient->alive || patient->id == driven
        || patient->hp >= sim::spec_of(patient->kind).hp || !truck_other_than(patient->id)) {
        return sim::UnitId{};
    }
    return patient->id;
}

} // namespace protekt::game
