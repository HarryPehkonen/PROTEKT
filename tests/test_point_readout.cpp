// POINT AT A THING AND READ WHAT IT IS (Harri, 2026-11-03): a unit the crosshair is on, or within a
// few degrees of, shows its NAME above it with the health bar right under, and its DISTANCE in
// metres below it, as far under its middle as the bar is over it. The pair rides the unit's
// projected top (higher the nearer he gets) and is the SAME NUMBER OF PIXELS at any range. The ink
// says whose it is -- green his, red theirs. `T` locks the readout on the pointed asset, any side,
// until `T` again or it dies. It is a further consumer of the one shared aim (attack_aim.hpp), and
// the assist -- its dot, its line, its lead -- is exactly what it was.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "protekt/game/assist.hpp"
#include "protekt/game/attack_aim.hpp"
#include "protekt/game/interaction.hpp"
#include "protekt/game/pick.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/input/input.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/world/world.hpp"

namespace protekt::game {
namespace {

using input::InputFrame;
using input::KeyEvent;
using sim::Side;
using sim::UnitId;
using sim::UnitKind;

constexpr std::uint64_t kSeed = 42;
constexpr Extent kExtent{1280, 720};
constexpr float kRadiansPerDegree = 3.14159265358979F / 180.0F;
constexpr std::array<float, 3> kOwnInk{0.45F, 1.0F, 0.45F};
constexpr std::array<float, 3> kHostileInk{1.0F, 0.25F, 0.25F};
constexpr float kCellPixels = 2.0F; ///< the radar's N S E W letters: 2 px cells, 10 px tall
constexpr float kLetterHeight = 10.0F;

[[nodiscard]] InputFrame key_frame(std::int32_t scancode) {
    InputFrame frame{};
    frame.keys.push_back(KeyEvent{scancode, true, false});
    return frame;
}

struct Field {
    sim::Sim sim;
    SceneOptions options;
    UnitId driven;
    UnitId enemy;
    UnitId friendly;
    UnitId turret;
};

/// The camera stands `back` tiles short of the enemy tank, level with its middle, looking straight
/// at it (yaw 0 is +Z): a yaw of `d` puts it exactly `d` off the crosshair.
[[nodiscard]] Field make_field(float back = 20.0F) {
    Field f{};
    f.sim = sim::Sim::create(sim::Scenario{kSeed,
                                           {{UnitKind::Tank, Side::Player, 34, 54},
                                            {UnitKind::Tank, Side::Enemy, 40, 60},
                                            {UnitKind::Tank, Side::Player, 60, 52},
                                            {UnitKind::Turret, Side::Enemy, 52, 60}}});
    f.driven = f.sim.units()[0].id;
    f.enemy = f.sim.units()[1].id;
    f.friendly = f.sim.units()[2].id;
    f.turret = f.sim.units()[3].id;
    const sim::Unit* tank = f.sim.find(f.enemy);
    f.options.seed = kSeed;
    f.options.extent = kExtent;
    f.options.viewer_unit = f.driven;
    f.options.viewer_side = Side::Player;
    f.options.driven_unit = f.driven;
    f.options.show_ledger = false;
    f.options.radius_tiles = 60;
    f.options.camera = Camera{40.0F * kMetresPerTile, ground_metres(kSeed, tank->x, tank->y) + 0.9F,
                              (60.0F - back) * kMetresPerTile, 0.0F, 0.0F};
    f.options.aim = Aim{640.0F, 360.0F};
    return f;
}

/// Turn the camera onto `unit`'s middle and then `degrees` round, the crosshair at the centre.
void look_at(Field& f, UnitId id, float degrees) {
    const sim::Unit* unit = f.sim.find(id);
    const float one = static_cast<float>(sim::Fixed::kOne);
    const float x = static_cast<float>(unit->x.raw) / one * kMetresPerTile;
    const float z = static_cast<float>(unit->y.raw) / one * kMetresPerTile;
    f.options.camera.y
        = ground_metres(kSeed, unit->x, unit->y) + (unit_box_height_metres(unit->kind) * 0.5F);
    f.options.camera.pitch = 0.0F;
    f.options.camera.yaw = std::atan2(x - f.options.camera.x, z - f.options.camera.z)
                           + (degrees * kRadiansPerDegree);
    f.options.aim = Aim{640.0F, 360.0F};
}

struct Cell {
    float left{};
    float top{};
};

/// Every 2x2 px quad of `ink` in the HUD half of the frame: one cell of one letter.
[[nodiscard]] std::vector<Cell> cells_of(const SceneOptions& options, const Scene& scene,
                                         const std::array<float, 3>& ink) {
    std::vector<Cell> cells;
    for (std::size_t first = scene.world_vertex_count; first + 6 <= scene.vertices.size();
         ++first) {
        // A quad is a, b, c, a, c, d: the shape that tells one from two neighbours' halves.
        const auto& q = scene.vertices;
        if (q.at(first).position != q.at(first + 3).position
            || q.at(first + 2).position != q.at(first + 4).position) {
            continue;
        }
        float left = 1e9F;
        float right = -1e9F;
        float top = 1e9F;
        float bottom = -1e9F;
        bool same = true;
        for (std::size_t i = 0; i < 6; ++i) {
            const SceneVertex& v = scene.vertices.at(first + i);
            same = same && v.colour == ink;
            const Projected p = project(options.camera, options.extent, v.position[0],
                                        v.position[1], v.position[2]);
            left = std::min(left, p.x);
            right = std::max(right, p.x);
            top = std::min(top, p.y);
            bottom = std::max(bottom, p.y);
        }
        if (same && std::abs(right - left - kCellPixels) < 0.05F
            && std::abs(bottom - top - kCellPixels) < 0.05F) {
            cells.push_back(Cell{left, top});
            first += 5;
        }
    }
    return cells;
}

struct Block {
    std::size_t cells{};
    float left{};
    float right{};
    float top{};
    float bottom{};
};

[[nodiscard]] Block block_of(const std::vector<Cell>& cells, bool above, float centre_y) {
    Block b{0, 1e9F, -1e9F, 1e9F, -1e9F};
    for (const Cell& c : cells) {
        if ((c.top < centre_y) != above) {
            continue;
        }
        ++b.cells;
        b.left = std::min(b.left, c.left);
        b.right = std::max(b.right, c.left + kCellPixels);
        b.top = std::min(b.top, c.top);
        b.bottom = std::max(b.bottom, c.top + kCellPixels);
    }
    return b;
}

/// The unit's health bar in the frame: the outline quad's pixel box, the one whose fill is `ink`.
/// Found as the black quad directly above `centre_x`.
struct BarBox {
    float top{};
    float bottom{};
    bool found{};
};

[[nodiscard]] BarBox bar_over(const SceneOptions& options, const Scene& scene, float centre_x,
                              float centre_y) {
    BarBox best{};
    for (std::size_t first = scene.world_vertex_count; first + 18 <= scene.vertices.size();
         ++first) {
        const auto& v = scene.vertices;
        if (!(v.at(first).colour == std::array<float, 3>{0.0F, 0.0F, 0.0F}
              && v.at(first + 6).colour == std::array<float, 3>{0.10F, 0.10F, 0.12F})) {
            continue;
        }
        float left = 1e9F;
        float right = -1e9F;
        float top = 1e9F;
        float bottom = -1e9F;
        for (std::size_t i = 0; i < 6; ++i) {
            const Projected p = project(options.camera, options.extent, v.at(first + i).position[0],
                                        v.at(first + i).position[1], v.at(first + i).position[2]);
            left = std::min(left, p.x);
            right = std::max(right, p.x);
            top = std::min(top, p.y);
            bottom = std::max(bottom, p.y);
        }
        if (std::abs(((left + right) * 0.5F) - centre_x) < 3.0F && bottom < centre_y
            && (!best.found || bottom > best.bottom)) {
            best = BarBox{top, bottom, true};
        }
    }
    return best;
}

[[nodiscard]] float eye_to_middle(const SceneOptions& options, const sim::Unit& unit) {
    const float one = static_cast<float>(sim::Fixed::kOne);
    const float dx = (static_cast<float>(unit.x.raw) / one * kMetresPerTile) - options.camera.x;
    const float dy = ground_metres(options.seed, unit.x, unit.y)
                     + (unit_box_height_metres(unit.kind) * 0.5F) - options.camera.y;
    const float dz = (static_cast<float>(unit.y.raw) / one * kMetresPerTile) - options.camera.z;
    return std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
}

/// What a label of `text` measures: six cells a letter less the last gap.
[[nodiscard]] float text_width(const std::string& text) {
    return ((static_cast<float>(text.size()) * 6.0F) - 1.0F) * kCellPixels;
}

} // namespace

// ------------------------------------------------------------------------------ the shared aim

TEST(PointReadout, TheDistanceIsTheEyeToTheUnitsMiddleInMetres) {
    const Field f = make_field();
    const sim::Unit* enemy = f.sim.find(f.enemy);
    EXPECT_NEAR(distance_metres(f.options, *enemy), eye_to_middle(f.options, *enemy), 0.01F);
    EXPECT_GT(distance_metres(f.options, *enemy), 30.0F) << "20 tiles back is tens of metres";
}

TEST(PointReadout, PointingIsTheHealthBarsOwnConeAndNoSecondOne) {
    for (const float sign : {1.0F, -1.0F}) {
        Field inside = make_field();
        look_at(inside, inside.enemy, sign * (kAssistConeDegrees - 0.5F));
        EXPECT_EQ(pointed_asset(inside.sim, inside.options, Pick{}), inside.enemy);
        Field outside = make_field();
        look_at(outside, outside.enemy, sign * (kAssistConeDegrees + 0.5F));
        EXPECT_FALSE(pointed_asset(outside.sim, outside.options, Pick{}).valid());
    }
}

// ------------------------------------------------------------------------------ the layout

TEST(PointReadout, TheNameIsAboveTheBarAndTheDistanceBelowTheUnit) {
    Field f = make_field();
    look_at(f, f.enemy, 0.0F);
    const Scene scene = build_scene(f.sim, f.options);
    const std::vector<Cell> cells = cells_of(f.options, scene, kHostileInk);
    const Projected middle = attack_point(f.options, *f.sim.find(f.enemy));
    const Block name = block_of(cells, true, middle.y);
    const Block distance = block_of(cells, false, middle.y);
    ASSERT_GT(name.cells, 0U) << "the name is drawn";
    ASSERT_GT(distance.cells, 0U) << "and the distance";
    EXPECT_LT(name.bottom, middle.y);
    EXPECT_GT(distance.top, middle.y);
    const BarBox bar = bar_over(f.options, scene, middle.x, middle.y);
    ASSERT_TRUE(bar.found);
    EXPECT_LT(name.bottom, bar.top) << "the name is over the bar";
    EXPECT_GT(name.bottom, bar.top - 8.0F) << "and the bar is right under it";
    // "TANK": four letters.
    EXPECT_NEAR(name.right - name.left, text_width("TANK"), 0.1F);
    EXPECT_NEAR(name.bottom - name.top, kLetterHeight, 0.1F);
}

TEST(PointReadout, TheDistanceIsAsFarUnderTheMiddleAsTheBarIsOverIt) {
    for (const float back : {20.0F, 8.0F}) {
        Field f = make_field(back);
        look_at(f, f.enemy, 0.0F);
        const Scene scene = build_scene(f.sim, f.options);
        const Projected middle = attack_point(f.options, *f.sim.find(f.enemy));
        const BarBox bar = bar_over(f.options, scene, middle.x, middle.y);
        ASSERT_TRUE(bar.found) << back;
        const Block distance = block_of(cells_of(f.options, scene, kHostileInk), false, middle.y);
        ASSERT_GT(distance.cells, 0U) << back;
        // The plate round the letters takes the pad; the plate's top edge mirrors the bar's.
        EXPECT_NEAR(distance.top - kLetterPlatePadPixels - middle.y, middle.y - bar.bottom, 0.3F)
            << "back " << back;
    }
}

TEST(PointReadout, TheDistanceReadsInMetresAndIsLabelledM) {
    Field f = make_field();
    look_at(f, f.enemy, 0.0F);
    const Scene scene = build_scene(f.sim, f.options);
    const Projected middle = attack_point(f.options, *f.sim.find(f.enemy));
    const Block distance = block_of(cells_of(f.options, scene, kHostileInk), false, middle.y);
    const long metres = std::lround(distance_metres(f.options, *f.sim.find(f.enemy)));
    EXPECT_NEAR(distance.right - distance.left, text_width(std::to_string(metres) + " M"), 0.1F);
    EXPECT_NEAR(distance.bottom - distance.top, kLetterHeight, 0.1F)
        << "the radar letters' size: 5 cells of 2 px";
}

// ------------------------------------------------------------------------------ closer = higher

TEST(PointReadout, NearerRidesHigherAndTheLettersNeverChangeSize) {
    float previous_rise = 0.0F;
    float width = 0.0F;
    for (const float back : {30.0F, 16.0F, 8.0F}) {
        Field f = make_field(back);
        look_at(f, f.enemy, 0.0F);
        const Scene scene = build_scene(f.sim, f.options);
        const Projected middle = attack_point(f.options, *f.sim.find(f.enemy));
        const std::vector<Cell> cells = cells_of(f.options, scene, kHostileInk);
        const Block name = block_of(cells, true, middle.y);
        ASSERT_GT(name.cells, 0U) << back;
        const float rise = middle.y - name.bottom;
        EXPECT_GT(rise, previous_rise) << "back " << back << ": nearer, so higher over the middle";
        previous_rise = rise;
        // Not scaled: the same box, in pixels, whatever the range.
        if (width != 0.0F) {
            EXPECT_NEAR(name.right - name.left, width, 0.1F);
        }
        width = name.right - name.left;
        EXPECT_NEAR(name.bottom - name.top, kLetterHeight, 0.1F);
    }
}

// ------------------------------------------------------------------------------ whose it is

TEST(PointReadout, TheInkSaysWhoseItIs) {
    Field theirs = make_field();
    look_at(theirs, theirs.enemy, 0.0F);
    const Scene red = build_scene(theirs.sim, theirs.options);
    const Projected at_enemy = attack_point(theirs.options, *theirs.sim.find(theirs.enemy));
    EXPECT_GT(block_of(cells_of(theirs.options, red, kHostileInk), true, at_enemy.y).cells, 0U);
    EXPECT_EQ(block_of(cells_of(theirs.options, red, kOwnInk), true, at_enemy.y).cells, 0U);

    Field mine = make_field();
    look_at(mine, mine.friendly, 0.0F);
    const Scene green = build_scene(mine.sim, mine.options);
    const Projected at_friend = attack_point(mine.options, *mine.sim.find(mine.friendly));
    EXPECT_GT(block_of(cells_of(mine.options, green, kOwnInk), true, at_friend.y).cells, 0U);
    EXPECT_GT(block_of(cells_of(mine.options, green, kOwnInk), false, at_friend.y).cells, 0U);
    EXPECT_EQ(block_of(cells_of(mine.options, green, kHostileInk), true, at_friend.y).cells, 0U);
}

TEST(PointReadout, ABuildingIsReadToo) {
    Field f = make_field();
    look_at(f, f.turret, 0.0F);
    const Scene scene = build_scene(f.sim, f.options);
    const Projected middle = attack_point(f.options, *f.sim.find(f.turret));
    const std::vector<Cell> cells = cells_of(f.options, scene, kHostileInk);
    const Block name = block_of(cells, true, middle.y);
    // The turret is the ENEMY's, and the enemy plays the aliens by default: their name for it.
    EXPECT_NEAR(name.right - name.left, text_width("GUARDIAN"), 0.1F);
    EXPECT_GT(block_of(cells, false, middle.y).cells, 0U);
}

// ------------------------------------------------------------------------------ transient / locked

TEST(PointReadout, PointingShowsItAndTurningAwayTakesItDown) {
    Field f = make_field();
    look_at(f, f.enemy, 0.0F);
    const Projected middle = attack_point(f.options, *f.sim.find(f.enemy));
    EXPECT_GT(cells_of(f.options, build_scene(f.sim, f.options), kHostileInk).size(), 0U);
    look_at(f, f.enemy, kAssistConeDegrees + 1.0F);
    EXPECT_EQ(cells_of(f.options, build_scene(f.sim, f.options), kHostileInk).size(), 0U)
        << "no pointing, no readout (" << middle.y << ")";
}

TEST(PointReadout, ALockedUnitKeepsItWhileHeLooksElsewhere) {
    Field f = make_field();
    look_at(f, f.enemy, kAssistConeDegrees + 6.0F);
    f.options.readout_lock = f.enemy;
    const Scene scene = build_scene(f.sim, f.options);
    const Projected middle = attack_point(f.options, *f.sim.find(f.enemy));
    const std::vector<Cell> cells = cells_of(f.options, scene, kHostileInk);
    EXPECT_GT(block_of(cells, true, middle.y).cells, 0U);
    EXPECT_GT(block_of(cells, false, middle.y).cells, 0U);
}

TEST(PointReadoutKey, TLocksTheReadoutOnAnyAssetAndTAgainLetsGo) {
    for (const bool own : {false, true}) {
        Field f = make_field();
        const UnitId who = own ? f.friendly : f.enemy;
        look_at(f, who, 1.0F);
        Interaction player{f.driven};
        player.handle(key_frame(input::kKeyT), f.sim, f.options);
        EXPECT_EQ(player.readout_lock(), who) << (own ? "his own" : "an enemy");
        player.handle(InputFrame{}, f.sim, f.options);
        EXPECT_EQ(player.readout_lock(), who) << "it stays with no key pressed";
        look_at(f, who, kAssistConeDegrees + 10.0F);
        player.handle(InputFrame{}, f.sim, f.options);
        EXPECT_EQ(player.readout_lock(), who) << "and while he looks elsewhere";
        look_at(f, who, 0.5F);
        player.handle(key_frame(input::kKeyT), f.sim, f.options);
        EXPECT_FALSE(player.readout_lock().valid()) << "T again";
    }
}

TEST(PointReadoutKey, ALockGoesWhenTheThingDies) {
    Field f = make_field();
    look_at(f, f.enemy, 0.0F);
    Interaction player{f.driven};
    player.handle(key_frame(input::kKeyT), f.sim, f.options);
    ASSERT_EQ(player.readout_lock(), f.enemy);
    const sim::Sim empty
        = sim::Sim::create(sim::Scenario{kSeed, {{UnitKind::Tank, Side::Player, 36, 44}}});
    player.handle(InputFrame{}, empty, f.options);
    EXPECT_FALSE(player.readout_lock().valid());
}

TEST(PointReadoutKey, TOnAnotherThingMovesTheLockAndOnNothingReleasesIt) {
    Field f = make_field();
    Interaction player{f.driven};
    look_at(f, f.enemy, 0.0F);
    player.handle(key_frame(input::kKeyT), f.sim, f.options);
    look_at(f, f.turret, 0.0F);
    player.handle(key_frame(input::kKeyT), f.sim, f.options);
    EXPECT_EQ(player.readout_lock(), f.turret);
    look_at(f, f.turret, kAssistConeDegrees + 20.0F);
    player.handle(key_frame(input::kKeyT), f.sim, f.options);
    EXPECT_FALSE(player.readout_lock().valid());
}

// ------------------------------------------------------------------------------ the assist is as
// it was

TEST(PointReadoutAssist, TStillLatchesTheHostileAndNeverTheFriend) {
    Field f = make_field();
    Interaction player{f.driven};
    look_at(f, f.enemy, 0.0F);
    player.handle(key_frame(input::kKeyT), f.sim, f.options);
    EXPECT_EQ(player.assist_target(), f.enemy);
    EXPECT_EQ(player.readout_lock(), f.enemy);
    Interaction friend_only{f.driven};
    look_at(f, f.friendly, 0.0F);
    friend_only.handle(key_frame(input::kKeyT), f.sim, f.options);
    EXPECT_FALSE(friend_only.assist_target().valid()) << "a friend is not the assist's to take";
    EXPECT_EQ(friend_only.readout_lock(), f.friendly);
}

TEST(PointReadoutAssist, TheLockAddsNothingToTheAssistsLineAndDot) {
    Field f = make_field();
    look_at(f, f.enemy, 0.0F);
    f.options.weapon = Weapon::Gun;
    f.options.assist_target = f.enemy;
    const auto ink_count = [&](const SceneOptions& options) {
        const Scene scene = build_scene(f.sim, options);
        return std::count_if(scene.vertices.begin(), scene.vertices.end(),
                             [](const SceneVertex& v) { return v.colour == kAssistInk; });
    };
    const auto before = ink_count(f.options);
    SceneOptions locked = f.options;
    locked.readout_lock = f.enemy;
    EXPECT_EQ(ink_count(locked), before) << "one quad and one fan, with or without the readout";
    ASSERT_EQ(before, static_cast<long>(6U + (3U * kAssistDotSlices)));
}

TEST(PointReadoutAssist, TheAssistStillLeadsAMovingTarget) {
    // The lead is the assist's own solve, untouched: a target crossing at speed has its aim point
    // AHEAD of where it stands, in its direction of travel. (The rig is test_assist.cpp's Duel.)
    constexpr int kTolerance = 40;
    int bx = 0;
    int by = 0;
    for (int y = 8; y < sim::kMapTiles - 32 && bx == 0; y += 2) {
        for (int x = 8; x < sim::kMapTiles - 32 && bx == 0; x += 2) {
            const int h0 = world::sample(kSeed, x, y).height;
            bool level = true;
            for (int j = 0; j <= 16 && level; ++j) {
                for (int i = 0; i <= 16 && level; ++i) {
                    const auto t = world::sample(kSeed, x + i, y + j);
                    level = t.terrain != world::Terrain::Crater
                            && std::abs(t.height - h0) <= kTolerance;
                }
            }
            if (level) {
                bx = x;
                by = y;
            }
        }
    }
    ASSERT_NE(bx, 0) << "a level block";
    const int sx = bx + 2;
    const int sy = by + 8;
    sim::Sim sim = sim::Sim::create(sim::Scenario{
        kSeed,
        {{UnitKind::Tank, Side::Player, sx, sy}, {UnitKind::Tank, Side::Enemy, sx + 10, sy - 5}}});
    const UnitId shooter = sim.units()[0].id;
    const UnitId target = sim.units()[1].id;
    sim.enqueue(sim::Order{shooter, sim::orders::TakeControl{}});
    for (int tick = 0; tick < sim::profile(UnitKind::Tank).cooldown_ticks; ++tick) {
        sim.step();
    }
    sim.enqueue(sim::Order{
        target, sim::orders::MoveTo{sim::Fixed::from_int(sx + 10), sim::Fixed::from_int(sy + 25)}});
    for (int tick = 0; tick < 4; ++tick) {
        sim.step();
    }
    const sim::Unit* gun = sim.find(shooter);
    const sim::Unit* mover = sim.find(target);
    const float one = static_cast<float>(sim::Fixed::kOne);
    SceneOptions options;
    options.seed = kSeed;
    options.extent = kExtent;
    options.camera = Camera{static_cast<float>(gun->x.raw) / one * kMetresPerTile,
                            ground_metres(kSeed, gun->x, gun->y) + 1.4F,
                            static_cast<float>(gun->y.raw) / one * kMetresPerTile, 0.0F, 0.0F};
    const auto shot = assist_shot(options, Weapon::Gun, *gun, *mover);
    ASSERT_TRUE(shot.has_value());
    if (!shot.has_value()) {
        return; // for the analyzer, not the test: gtest asserts do not stop it
    }
    const float stands = static_cast<float>(mover->y.raw) / one * kMetresPerTile;
    EXPECT_GT(shot->aim_metres.at(2) - stands, 4.0F) << "the dot leads a target moving +y";
    EXPECT_GT(shot->passes, 2);
}

} // namespace protekt::game
