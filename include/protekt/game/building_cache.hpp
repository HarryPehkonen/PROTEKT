#pragma once

// N8: THE STANDING BUILDINGS' MESHES, REMEMBERED.
//
// A building that stands never changes shape: `docs/FRAMECOST.md` measured `smooth_vertex_normals`
// at 62.7% of `build_scene` at the x2 base, re-averaging every vertex of every building every frame
// for geometry that cannot move. This is the memo that stops it: the building's vertices, as drawn
// (shape, scale, and smoothed normals), kept from the frame it was first seen and copied out on
// every frame after.
//
// A MEMO, never a second source of truth. An entry is written only by the SAME two calls the live
// path makes (`build_building_model`, then `scale_model`) followed by the SAME smoothing, and read
// back unchanged, so a frame drawn from it is byte-identical to one drawn without it
// (BuildingCache.* pin exactly that, for every kind that qualifies).
//
// WHAT IT IS KEYED BY, and why that is enough. Every input the building builders read: the kind,
// the side (its colour and its facing), `turned` (the quarter turn), the unit's place (the model is
// built in WORLD coordinates, its ground height read at that place), the hull variant (the alien
// shells' camo, a hash of the unit's id) and the world seed (the ground). Nothing else reaches the
// vertices: no health, no tick, no faction (the faction only picks the builder of a Tug, a
// Scavenger or a turret, none of which is cached). The place is in the key and not applied as a
// translation afterwards BECAUSE a translated template is not the same bytes: `scale_model`
// scales about the building's centre and its lowest point, and `x + ((x + r) - x) * 4` is not
// `x + r * 4` in floats. Two buildings of one kind therefore have two entries; each is built once.
//
// WHAT IS NOT CACHED: a building that is marked (selected, aimed at -- the mark is its colour), one
// that is being built (cut each frame), one that is lifted, and every kind that tilts with the
// ground or turns a part (`building_is_cacheable`). Those come from the live path.
//
// EVICTION: an entry not used by a frame is dropped at that frame's end (`end_frame`), so a
// building that has been destroyed stops being held.
//
// OWNED BY THE CALLER, one thread: the app holds one for its session and hands it to `build_scene`
// through `SceneOptions::buildings` as a non-owned pointer, the way the terrain cache is. The table
// is `mutable` because a memo IS a write behind a read.
#include <cstddef>
#include <cstdint>
#include <map>
#include <tuple>
#include <vector>

#include "protekt/game/scene_data.hpp"

namespace protekt::game {

/// Everything that determines a standing building's vertices.
struct BuildingMeshKey {
    std::uint64_t seed{};
    std::int32_t x{}; ///< the unit's `x.raw`
    std::int32_t y{}; ///< the unit's `y.raw`
    std::uint32_t variant{};
    std::uint8_t kind{};
    std::uint8_t side{};
    bool turned{};

    [[nodiscard]] bool operator<(const BuildingMeshKey& other) const noexcept {
        return std::tie(seed, x, y, variant, kind, side, turned)
               < std::tie(other.seed, other.x, other.y, other.variant, other.kind, other.side,
                          other.turned);
    }
};

class BuildingMeshCache final {
public:
    /// The remembered vertices for `key`, marked as used this frame; null when there are none.
    [[nodiscard]] const std::vector<SceneVertex>* find(const BuildingMeshKey& key) const {
        const auto found = entries_.find(key);
        if (found == entries_.end()) {
            return nullptr;
        }
        found->second.used = true;
        ++reused_;
        return &found->second.vertices;
    }

    /// Remembers `vertices` for `key` (replacing any), marked as used this frame.
    const std::vector<SceneVertex>& insert(const BuildingMeshKey& key,
                                           std::vector<SceneVertex> vertices) const {
        Entry& entry = entries_[key];
        entry.vertices = std::move(vertices);
        entry.used = true;
        ++built_;
        return entry.vertices;
    }

    /// Drops every entry no frame has used since the last call.
    void end_frame() const {
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (it->second.used) {
                it->second.used = false;
                ++it;
            } else {
                it = entries_.erase(it);
            }
        }
    }

    /// How many buildings are held right now.
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    /// How many times a building's mesh has been built (a miss), since the cache was made.
    [[nodiscard]] std::size_t built() const noexcept { return built_; }
    /// How many times one has been copied out instead (a hit), since the cache was made.
    [[nodiscard]] std::size_t reused() const noexcept { return reused_; }

private:
    struct Entry {
        std::vector<SceneVertex> vertices;
        bool used{};
    };
    mutable std::map<BuildingMeshKey, Entry> entries_;
    mutable std::size_t built_{};
    mutable std::size_t reused_{};
};

} // namespace protekt::game
