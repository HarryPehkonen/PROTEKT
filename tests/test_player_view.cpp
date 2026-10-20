// The app layer's eyes, radar and ears follow the tank the player is DRIVING, read fresh each
// frame. The bug these pin: the camera, radar and listener kept the id from startup, so after an
// X swap the throttle drove the new tank while the view stayed on the old one.
#include "protekt/app/player_view.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <vector>

#include "protekt/game/pick.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/game/session.hpp"
#include "protekt/input/input.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"

namespace protekt::app {
namespace {

using game::Session;
using sim::Side;
using sim::UnitKind;

constexpr std::uint64_t kSeed = 42;
constexpr game::Extent kExtent{1280, 720};

struct Rig {
    sim::Sim sim;
    game::SceneOptions options;
    sim::UnitId tank_a{};
    sim::UnitId tank_b{};
};

Rig make_rig() {
    Rig rig{};
    rig.sim = sim::Sim::create(sim::Scenario{kSeed,
                                             {{UnitKind::Tank, Side::Player, 40, 40},
                                              {UnitKind::Tank, Side::Player, 60, 40},
                                              {UnitKind::Tank, Side::Enemy, 40, 70}}});
    for (const sim::Unit& unit : rig.sim.units()) {
        if (unit.side == Side::Player && unit.x.floor_int() == 40) {
            rig.tank_a = unit.id;
        } else if (unit.side == Side::Player) {
            rig.tank_b = unit.id;
        }
    }
    rig.options.seed = kSeed;
    rig.options.extent = kExtent;
    return rig;
}

/// A session that started driving tank A and has swapped into tank B with X.
Session swapped_session(Rig& rig) {
    Session session{rig.tank_a};
    // Aim at tank B: put the camera behind A, looking along +x at B is what the swap's pick needs.
    const sim::Unit* b = rig.sim.find(rig.tank_b);
    rig.options.camera = camera_for(rig.sim, session, kSeed, std::nullopt);
    rig.options.camera.yaw = 1.5707964F; // +x
    const float bx = static_cast<float>(b->x.raw) / 65536.0F * game::kMetresPerTile;
    const float bz = static_cast<float>(b->y.raw) / 65536.0F * game::kMetresPerTile;
    const float ground = game::height_metres(kSeed, b->x.floor_int(), b->y.floor_int());
    const game::Projected p = game::project(rig.options.camera, kExtent, bx, ground + 0.9F, bz);
    rig.options.aim = game::Aim{p.x, p.y};
    input::InputFrame swap;
    swap.keys.push_back(input::KeyEvent{input::kKeyX, true, false});
    std::vector<sim::Order> out;
    session.frame(swap, rig.sim, rig.options, out);
    return session;
}

TEST(PlayerView, TheCameraStartsOnTheTankTheSessionDrives) {
    Rig rig = make_rig();
    const Session session{rig.tank_a};
    const game::Camera camera = camera_for(rig.sim, session, kSeed, std::nullopt);
    EXPECT_NEAR(camera.x, 40.0F * game::kMetresPerTile, game::kMetresPerTile);
}

TEST(PlayerView, AfterASwapTheCameraIsOnTheNewTank) {
    Rig rig = make_rig();
    const Session session = swapped_session(rig);
    ASSERT_EQ(session.driven(), rig.tank_b) << "the rig must have swapped";
    const game::Camera camera = camera_for(rig.sim, session, kSeed, std::nullopt);
    EXPECT_NEAR(camera.x, 60.0F * game::kMetresPerTile, game::kMetresPerTile)
        << "the eye stayed on the tank the player left";
}

TEST(PlayerView, AfterASwapTheRadarIsDrawnFromTheNewTank) {
    Rig rig = make_rig();
    const Session session = swapped_session(rig);
    ASSERT_EQ(session.driven(), rig.tank_b);
    EXPECT_EQ(radar_viewer(session, false), rig.tank_b);
    EXPECT_FALSE(radar_viewer(session, true).valid()) << "--no-radar means no viewer";
}

TEST(PlayerView, AfterASwapTheListenerAndTheHitFlashAreTheNewTank) {
    Rig rig = make_rig();
    const Session session = swapped_session(rig);
    ASSERT_EQ(session.driven(), rig.tank_b);
    const game::Camera camera = camera_for(rig.sim, session, kSeed, std::nullopt);
    const AudioFrame frame = audio_frame_for(rig.sim, session, camera, false, true);
    EXPECT_EQ(frame.player, rig.tank_b);
    EXPECT_FLOAT_EQ(frame.listener.x, camera.x);
    EXPECT_FLOAT_EQ(frame.listener.z, camera.z);
    EXPECT_TRUE(frame.player_alive_now);
}

} // namespace
} // namespace protekt::app
