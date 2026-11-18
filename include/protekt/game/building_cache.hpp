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

/// N10: everything that determines a vehicle's vertices. Where a building is keyed by its place and
/// its facing, a vehicle is keyed by its place and the angles it is drawn at, BIT FOR BIT: the
/// angles are floats that `rotate_offset` feeds to `sin` and `cos`, and a template turned by a
/// matrix afterwards is not the same bytes (see the N10 note below).
struct VehicleMeshKey {
    std::uint64_t seed{};
    std::int32_t x{};
    std::int32_t y{};
    std::uint32_t variant{};
    std::uint32_t heading{}; ///< `std::bit_cast` of the hull's yaw
    std::uint32_t turret{};  ///< of the turret's yaw from the hull's
    std::uint32_t stride{};  ///< of the walk phase, for the two kinds that have legs; else 0
    std::uint8_t kind{};
    std::uint8_t side{};
    bool alien{}; ///< the faction's silhouette (a Scavenger, Tug or turret is drawn another way)
    bool has_stride{}; ///< a legged machine that has never moved has NO phase, which is not phase 0

    [[nodiscard]] bool operator<(const VehicleMeshKey& other) const noexcept {
        return std::tie(seed, x, y, variant, heading, turret, stride, kind, side, alien, has_stride)
               < std::tie(other.seed, other.x, other.y, other.variant, other.heading, other.turret,
                          other.stride, other.kind, other.side, other.alien, other.has_stride);
    }
};

// N10: THE SAME MEMO, FOR VEHICLES. A battle's scene is mostly vehicles, and
// `smooth_vertex_normals` re-averaged every one of them every frame. A vehicle that holds still --
// the same place, the same hull heading, the same turret bearing, the same stride -- is drawn from
// here exactly as a standing building is: its vertices as drawn (built, tilted to the ground,
// smoothed alone), copied out, its range stepped over by the pass. The table is the building
// table's own machinery
// (`MeshTable`), not a second one.
//
// WHY THE KEY CARRIES THE POSE AND THE PLACE AND NOT A TEMPLATE PLUS A TRANSFORM. The honest way to
// smooth a vehicle once per (kind, faction, side) and move it by a translation, a yaw and a tilt
// is not byte-exact: the live path rotates each part about its own pivot with `sin`/`cos` in
// world coordinates and then tilts the lot about the ground point, and `rotate(translate(v))` is
// not `translate(rotate(v))` in floats -- the same reason the building key carries the place. The
// smoothed NORMALS are a weighted sum over faces, which would also differ in the last bits once
// the faces are turned. This project compares frames byte for byte, so the entry is the live
// path's own output for exactly these inputs. The price: a vehicle that MOVES, or whose gun
// turns, has a new key each frame and is a miss each frame (it is built, smoothed alone and held
// for that one frame, the same work the pass did, so it does not cost more than before); the
// saving is every vehicle that is standing still.
//
// WHAT THE BUILDERS READ is `Unit::id` (through the hull variant), `kind`, `side`, `x`, `y`, the
// world seed (the ground), and the arguments heading, turret, pitch, stride, mark and lift: no
// health, no damage state, no carried load, no tick. So no vehicle kind has per-unit vertices that
// the key does not name, and none is excluded for that reason. NOT CACHED, and drawn live: a
// marked vehicle (its colour), a lifted one, the DRIVEN one (its pitch and its heading come from
// the player, every frame), and a unit that is a building or a site.
class BuildingMeshCache final {
public:
    /// The remembered vertices for `key`, marked as used this frame; null when there are none.
    [[nodiscard]] const std::vector<SceneVertex>* find(const BuildingMeshKey& key) const {
        return buildings_.find(key);
    }
    [[nodiscard]] const std::vector<SceneVertex>* find(const VehicleMeshKey& key) const {
        return vehicles_.find(key);
    }

    /// Remembers `vertices` for `key` (replacing any), marked as used this frame.
    const std::vector<SceneVertex>& insert(const BuildingMeshKey& key,
                                           std::vector<SceneVertex> vertices) const {
        return buildings_.insert(key, std::move(vertices));
    }
    const std::vector<SceneVertex>& insert(const VehicleMeshKey& key,
                                           std::vector<SceneVertex> vertices) const {
        return vehicles_.insert(key, std::move(vertices));
    }

    /// Drops every entry no frame has used since the last call.
    void end_frame() const {
        buildings_.end_frame();
        vehicles_.end_frame();
    }

    /// How many buildings are held right now.
    [[nodiscard]] std::size_t size() const noexcept { return buildings_.size(); }
    /// How many times a building's mesh has been built (a miss), since the cache was made.
    [[nodiscard]] std::size_t built() const noexcept { return buildings_.built(); }
    /// How many times one has been copied out instead (a hit), since the cache was made.
    [[nodiscard]] std::size_t reused() const noexcept { return buildings_.reused(); }
    /// The same three for vehicles (N10).
    [[nodiscard]] std::size_t vehicle_size() const noexcept { return vehicles_.size(); }
    [[nodiscard]] std::size_t vehicle_built() const noexcept { return vehicles_.built(); }
    [[nodiscard]] std::size_t vehicle_reused() const noexcept { return vehicles_.reused(); }

private:
    template <typename Key> class MeshTable {
    public:
        [[nodiscard]] const std::vector<SceneVertex>* find(const Key& key) const {
            const auto found = entries_.find(key);
            if (found == entries_.end()) {
                return nullptr;
            }
            found->second.used = true;
            ++reused_;
            return &found->second.vertices;
        }
        const std::vector<SceneVertex>& insert(const Key& key,
                                               std::vector<SceneVertex> vertices) const {
            Entry& entry = entries_[key];
            entry.vertices = std::move(vertices);
            entry.used = true;
            ++built_;
            return entry.vertices;
        }
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
        [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
        [[nodiscard]] std::size_t built() const noexcept { return built_; }
        [[nodiscard]] std::size_t reused() const noexcept { return reused_; }

    private:
        struct Entry {
            std::vector<SceneVertex> vertices;
            bool used{};
        };
        mutable std::map<Key, Entry> entries_;
        mutable std::size_t built_{};
        mutable std::size_t reused_{};
    };
    MeshTable<BuildingMeshKey> buildings_;
    MeshTable<VehicleMeshKey> vehicles_;
};

} // namespace protekt::game
