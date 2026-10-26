#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "protekt/game/menu.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/input/input.hpp"
#include "protekt/sim/sim.hpp"

/// NAVIGATIONAL MARKS (N1, corrected by MENU-NAV): the player's own pins on the map, so he can find
/// his way back.
///
/// TEN SLOTS, ROUND-ROBIN. A new mark takes the NEXT slot in the cycle, drawn and pressed as
/// `1 2 3 4 5 6 7 8 9 0` and then `1` again: placement number p (1-based) lands in slot
/// (p - 1) mod 10. Writing into a slot that already holds a mark DESTROYS that mark irrecoverably,
/// with no prompt, so a full table is never a refusal -- the eleventh mark simply overwrites slot
/// `1`. That wrap is the ONLY way a mark ever goes: there is no deletion. A ray with no ground in
/// range never places a mark.
///
/// Every slot carries a HIGHLIGHT, a per-slot toggle and nothing else. A freshly placed mark is
/// always plain, whatever its slot held before; several slots may be highlighted at once.
///
/// The marks are the player's annotation, held by the app like `HitMark` and `LandingMarks`:
/// nothing here enters the sim, so a mark changes no tick and no hash.
namespace protekt::game {

inline constexpr int kNavSlots = 10;

/// The digit slot `slot` (0-9, cycle order) is drawn and pressed as: 1..9, then 0 for the tenth.
[[nodiscard]] constexpr int nav_slot_digit(int slot) noexcept { return (slot + 1) % kNavSlots; }

/// The slot a digit key (0-9) names: the inverse of `nav_slot_digit`.
[[nodiscard]] constexpr int nav_digit_slot(int digit) noexcept {
    return (digit + kNavSlots - 1) % kNavSlots;
}

/// The colours a mark is drawn in, the same on the radar, on the world map and as its beacon in
/// the world (his ruling, 2026-10-26: "EXACTLY the same as in the radar"): a green plus when the
/// slot is plain, a yellow one inside a yellow diamond when it is highlighted. Neither is a radar
/// tone, a reticle colour or the notice's amber.
inline constexpr math::Vec3 kNavPlainColour{0.0F, 1.0F, 0.0F};
inline constexpr math::Vec3 kNavHighlightColour{1.0F, 1.0F, 0.0F};

[[nodiscard]] constexpr const math::Vec3& nav_colour(bool highlighted) noexcept {
    return highlighted ? kNavHighlightColour : kNavPlainColour;
}

class NavMarks {
public:
    struct Mark {
        int slot{}; ///< 0-9 in cycle order; drawn and pressed as `nav_slot_digit(slot)`
        int tile_x{};
        int tile_y{};
        bool highlighted{false};
    };

    enum class Outcome : std::uint8_t {
        Placed,  ///< a new, plain mark in `slot`; whatever the slot held is gone for good
        NoGround ///< the ray met no ground (tile -1): nothing was placed
    };
    struct Result {
        Outcome outcome{Outcome::NoGround};
        int slot{-1};
    };

    /// Place a mark on a ground tile (`tile_x`/`tile_y` negative means the ray met none), in the
    /// next slot of the cycle. A tile that already holds a mark gets a second one: aiming at a mark
    /// never removes it.
    Result place(int tile_x, int tile_y);

    /// Flip the highlight of the mark in `slot`, and nothing else. False, and nothing changes, when
    /// the slot holds no mark.
    bool toggle(int slot);

    /// The slot of the mark on that tile -- the first in slot order if several share it -- or -1.
    [[nodiscard]] int slot_at(int tile_x, int tile_y) const;

    /// The marks that exist, in slot order (drawn `1`..`9`, then `0`). A slot never written is not
    /// here.
    [[nodiscard]] std::vector<Mark> marks() const;

private:
    struct Slot {
        bool used{false};
        int tile_x{};
        int tile_y{};
        bool highlighted{false};
    };
    std::array<Slot, kNavSlots> slots_{};
    int next_{0}; ///< the slot the next placement writes
};

/// Where the nav chain stands. One state at a time, and the game never pauses in any of them.
enum class NavState : std::uint8_t {
    Closed,  ///< the top menu: `n` arms placing, and `~` opens the nav menu once a mark exists
    Placing, ///< `top menu -> n`: SPACE places a mark on the ground under the reticle
    Menu,    ///< `top menu -> ~`: SPACE and the digits toggle a highlight, TAB closes
};

/// What the reticle is over, for the nav chain: the ground tile its ray meets (-1 when none) and
/// how far that tile is from the camera, in whole metres.
struct NavAim {
    int tile_x{-1};
    int tile_y{-1};
    int metres{};
    [[nodiscard]] bool on_ground() const noexcept { return tile_x >= 0 && tile_y >= 0; }
};

/// The horizontal distance from the camera to the middle of a tile, in whole metres, rounded half
/// up -- the attack list's rule. One rule for the `GET NAV` readout and the `NAV` rows, so a mark
/// placed at `17m` is listed at `17m`.
[[nodiscard]] int nav_metres(const Camera& from, int tile_x, int tile_y) noexcept;

/// The reticle as `pick` sees it now: the same ray the trigger and X use.
[[nodiscard]] NavAim nav_aim(const sim::Sim& sim, const SceneOptions& options);

/// THE NAV CHAIN's keys (MENU-NAV), as a pure function of the keys, the marks and the reticle.
///
///  * Closed: `n` ARMS placing and `~` opens the nav menu (once a mark exists), but only from the
///    top menu -- with another left menu open, neither does anything.
///  * Placing: SPACE places a mark on the ground under the reticle and closes. With the reticle
///    off the ground the state is drawn as nothing at all (`nav_menu_rows`), and SPACE places
///    nothing, but the state survives and is back when the reticle is.
///  * Menu: SPACE toggles the highlight of the mark under the reticle, a digit toggles that slot's;
///    the same verb, one mark per press. The menu stays open.
///  * Placing and Menu: TAB (or Escape) closes the left menu. The top menu is visible but
///    inaccessible meanwhile, so `n`, `~`, the digits, F1-F10 (shifted or not), and `G` and `T`
///    -- which open prompts of their own, and one panel at a time is the rule -- do nothing.
///
/// Only presses are taken; a release always passes through. Everything not taken goes to the
/// session in the order it came, so driving, looking, `R`, `X` and `M` work in every state.
class NavMenu {
public:
    /// One frame's keys. `top_menu` is true when NO other left menu is open (no selection menu,
    /// no `G` prompt, no attack list). Returns the keys it did not take, in order.
    [[nodiscard]] std::vector<input::KeyEvent> handle(std::span<const input::KeyEvent> keys,
                                                      NavMarks& marks, const NavAim& aim,
                                                      bool top_menu);

    [[nodiscard]] NavState state() const noexcept;

    /// What the last `handle` did, for the console ("Nav 3 placed."). Empty when it did nothing.
    [[nodiscard]] std::string_view message() const noexcept;

private:
    /// Whether this press is the chain's while a nav left menu is open: its own keys and the top
    /// menu's, which it shuts out.
    [[nodiscard]] static bool owned_while_open(const input::KeyEvent& key) noexcept;

    NavState state_{NavState::Closed};
    std::string message_;
};

/// The left menu the chain draws, top to bottom. Closed: none. Placing: exactly one row,
/// `SPACE GET NAV` with the reticle's distance at the right -- and NONE with the reticle off the
/// ground. Menu: `SPACE` (`SPACE SELECT` with the reticle over a mark), `TAB CLOSE MENU`, then one
/// `<digit> NAV` row per mark in slot order with its distance from `from` at the right.
[[nodiscard]] std::vector<MenuRow> nav_menu_rows(NavState state, const NavMarks& marks,
                                                 const NavAim& aim, const Camera& from);

} // namespace protekt::game
