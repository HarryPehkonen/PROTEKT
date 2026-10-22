#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "protekt/game/scene.hpp"
#include "protekt/sim/sim.hpp"

/// NAVIGATIONAL MARKS (N1): the player's own pins on the map, so he can find his way back.
///
/// Ten slots, numbered 0-9. N drops a mark on the ground tile under the crosshair and takes the
/// LOWEST FREE number; N aimed at a tile that already holds a mark clears it instead. A drop with
/// every slot in use is refused, never an overwrite, and a ray with no ground in range is refused,
/// never a guess. The marks are the player's annotation, held by the app like `HitMark` and
/// `LandingMarks`: nothing here enters the sim, so a mark changes no tick and no hash.
namespace protekt::game {

inline constexpr int kNavSlots = 10;

/// The colour a mark is drawn in, on the radar's plate and as the world beacon: a saturated cyan.
/// Neither team's camo has it, the landing marker is magenta, and no terrain tile is that
/// blue-green.
inline constexpr math::Vec3 kNavColour{0.0F, 1.0F, 1.0F};

class NavMarks {
public:
    struct Mark {
        int number{}; ///< 0-9
        int tile_x{};
        int tile_y{};
    };

    enum class Outcome : std::uint8_t {
        Dropped,  ///< a new mark, numbered `number`
        Cleared,  ///< the mark on that tile, `number`, is gone and its slot is free
        NoGround, ///< the ray met no ground (tile -1): nothing was created
        Full      ///< all `kNavSlots` are in use: nothing was created
    };
    struct Result {
        Outcome outcome{Outcome::NoGround};
        int number{-1};
    };

    /// N on a ground tile (`tile_x`/`tile_y` negative means the ray met none).
    Result press(int tile_x, int tile_y);

    /// The marks that exist, in ascending number. A number never dropped, or cleared, is not here.
    [[nodiscard]] std::vector<Mark> marks() const;

private:
    struct Slot {
        bool used{false};
        int tile_x{};
        int tile_y{};
    };
    std::array<Slot, kNavSlots> slots_{};
};

/// What N does with the crosshair as it is now: the ground tile under it comes from `pick`, the
/// same ray the trigger and X use. Returns the sentence to tell the player.
[[nodiscard]] std::string nav_press(NavMarks& marks, const sim::Sim& sim,
                                    const SceneOptions& options);

} // namespace protekt::game
