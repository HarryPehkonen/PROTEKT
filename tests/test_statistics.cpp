// AF: ESCAPE pauses the game and shows a statistics screen.
//
// Two halves. The COUNTS (`game::BattleStats`, plus the sim's own `collected`) are read off what
// the sim already reports each step, per side, and are never part of the battle: counting must not
// move a hash. The PAUSE is view state: a paused game steps nothing, hears nothing and takes no
// orders, and the battle that comes out of a pause is the battle that would have come out without
// one.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "protekt/app/audio_pipeline.hpp"
#include "protekt/app/game.hpp"
#include "protekt/game/radar.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/game/session.hpp"
#include "protekt/game/stats.hpp"
#include "protekt/input/bindings.hpp"
#include "protekt/input/input.hpp"
#include "protekt/sim/orders.hpp"
#include "protekt/sim/pool.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/spec.hpp"

namespace {

namespace sim = protekt::sim;
using protekt::game::BattleStats;
using sim::Scenario;
using sim::Scrap;
using sim::Side;
using sim::Sim;
using sim::Spawn;
using sim::UnitKind;

constexpr std::uint64_t kSeed = 20261023;
/// Ticks for a freshly spawned hull's gun to finish reloading.
constexpr int kReloaded = 30;

/// Three player tanks against one enemy tank on open ground, a hauler beside them: the enemy tank
/// dies, the hauler banks its wreck.
Scenario fight() {
    Scenario s;
    s.seed = kSeed;
    s.spawns = {Spawn{UnitKind::Factory, Side::Player, 52, 45},
                Spawn{UnitKind::Scavenger, Side::Player, 58, 40},
                Spawn{UnitKind::Tank, Side::Player, 57, 41},
                Spawn{UnitKind::Tank, Side::Player, 58, 41},
                Spawn{UnitKind::Tank, Side::Player, 59, 41},
                Spawn{UnitKind::Tank, Side::Enemy, 57, 43}};
    return s;
}

/// The same fight with the sides swapped: the ENEMY has the three tanks and the hauler.
Scenario fight_mirrored() {
    Scenario s = fight();
    for (Spawn& spawn : s.spawns) {
        spawn.side = spawn.side == Side::Player ? Side::Enemy : Side::Player;
    }
    return s;
}

/// A factory of `side` with three haulers and thirty loads of wreckage west of it, and nothing else
/// to shoot at: the only way for the bank to rise is the haulers' work.
Scenario factory_with_litter(Side side) {
    Scenario s;
    s.seed = kSeed;
    s.objectives = {};
    s.store_capacity = 400;
    s.spawns = {Spawn{UnitKind::Factory, side, 52, 45}, Spawn{UnitKind::Scavenger, side, 48, 47},
                Spawn{UnitKind::Scavenger, side, 48, 48}, Spawn{UnitKind::Scavenger, side, 48, 49}};
    const sim::Fixed half = sim::Fixed::from_raw(sim::Fixed::kOne / 2);
    for (int i = 0; i < 30; ++i) {
        s.scrap.push_back(Scrap{sim::fixed_add(sim::Fixed::from_int(42 + (i % 10)), half),
                                sim::fixed_add(sim::Fixed::from_int(36 + (i / 10)), half)});
    }
    return s;
}

/// Steps `ticks`, counting every one.
void run(Sim& sim, BattleStats& stats, int ticks) {
    for (int i = 0; i < ticks; ++i) {
        sim.step();
        stats.observe(sim);
    }
}

TEST(Statistics, ABattleThatNobodyHasCountedStartsAtZeroForBothSides) {
    const Sim sim = Sim::create(fight());
    const BattleStats stats{sim};
    for (const Side side : {Side::Player, Side::Enemy}) {
        EXPECT_EQ(stats.of(side).built, 0U) << "the units it opened with were not built in it";
        EXPECT_EQ(stats.of(side).destroyed, 0U);
        EXPECT_EQ(stats.of(side).fired, 0U);
        EXPECT_EQ(stats.of(side).hit, 0U);
        EXPECT_EQ(stats.missed(side), 0U);
        EXPECT_EQ(sim.collected(side), 0U);
    }
}

TEST(Statistics, VehiclesDestroyedAreCountedAgainstTheSideThatLostThem) {
    Sim sim = Sim::create(fight());
    BattleStats stats{sim};
    run(sim, stats, 900);
    ASSERT_FALSE(sim.find(sim::UnitId{6})->alive) << "the rig must kill the enemy tank";
    EXPECT_EQ(stats.of(Side::Enemy).destroyed, 1U) << "the enemy lost one vehicle";
    EXPECT_EQ(stats.of(Side::Player).destroyed, 0U) << "and the player none";
}

TEST(Statistics, TheSameFightWithTheSidesSwappedSwapsTheCounts) {
    Sim sim = Sim::create(fight_mirrored());
    BattleStats stats{sim};
    run(sim, stats, 900);
    EXPECT_EQ(stats.of(Side::Player).destroyed, 1U);
    EXPECT_EQ(stats.of(Side::Enemy).destroyed, 0U);
    EXPECT_GT(stats.of(Side::Enemy).fired, 0U);
    EXPECT_GT(stats.of(Side::Enemy).hit, 0U);
    EXPECT_GT(stats.of(Side::Enemy).hit, stats.of(Side::Player).hit)
        << "three guns struck more than the lone one";
    EXPECT_EQ(sim.collected(Side::Player), 0U);
    EXPECT_GT(sim.collected(Side::Enemy), 0U) << "the enemy's hauler banked the wreck";
}

TEST(Statistics, ShotsFiredAndHitAreCountedForTheSideThatFiredThem) {
    Sim sim = Sim::create(fight());
    BattleStats stats{sim};
    std::uint32_t fired_by_player = 0;
    std::uint32_t fired_by_enemy = 0;
    std::uint32_t struck_by_player = 0;
    for (int i = 0; i < 900; ++i) {
        sim.step();
        stats.observe(sim);
        for (const Sim::Shot& shot : sim.shots()) {
            (sim.find(shot.shooter)->side == Side::Player ? fired_by_player : fired_by_enemy) += 1U;
        }
        for (const sim::Impact& impact : sim.impacts()) {
            struck_by_player += sim.find(impact.shooter)->side == Side::Player ? 1U : 0U;
        }
    }
    ASSERT_GT(fired_by_player, 0U);
    ASSERT_GT(fired_by_enemy, 0U);
    EXPECT_EQ(stats.of(Side::Player).fired, fired_by_player);
    EXPECT_EQ(stats.of(Side::Enemy).fired, fired_by_enemy);
    EXPECT_EQ(stats.of(Side::Player).hit, struck_by_player);
    EXPECT_GT(stats.of(Side::Player).hit, 0U);
}

TEST(Statistics, ShotsMissedIsFiredMinusHitAndIsNotCountedOnItsOwn) {
    // A fight, whatever it comes to: the sum holds for both sides at every moment.
    Sim sim = Sim::create(fight());
    BattleStats stats{sim};
    for (int i = 0; i < 900; ++i) {
        run(sim, stats, 1);
        for (const Side side : {Side::Player, Side::Enemy}) {
            const auto& tally = stats.of(side);
            ASSERT_EQ(stats.missed(side), tally.fired >= tally.hit ? tally.fired - tally.hit : 0U);
        }
    }
    // And a shot at nothing, which cannot hit: the player's tank fires into empty air, so its
    // missed is exactly its fired, and the other side's is untouched.
    Scenario lone;
    lone.seed = kSeed;
    lone.objectives = {};
    lone.spawns = {Spawn{UnitKind::Tank, Side::Player, 60, 60},
                   Spawn{UnitKind::Tank, Side::Enemy, 200, 200}};
    Sim shooting = Sim::create(lone);
    BattleStats counts{shooting};
    run(shooting, counts, kReloaded); // a new hull starts with its gun reloading
    shooting.enqueue(
        sim::Order{shooting.units().front().id,
                   sim::orders::FireTowards{
                       sim::Fixed::from_int(-1), sim::Fixed::zero(), {}, sim::Fixed::zero()}});
    run(shooting, counts, 60);
    EXPECT_EQ(counts.of(Side::Player).fired, 1U);
    EXPECT_EQ(counts.of(Side::Player).hit, 0U);
    EXPECT_EQ(counts.missed(Side::Player), 1U) << "one fired, none hit, one missed";
    EXPECT_EQ(counts.missed(Side::Enemy), 0U);
}

TEST(Statistics, MissedNeverGoesNegativeWhenPelletsStrikeMoreThanWasFired) {
    // One scatter-gun shot is one shot fired and several pellets striking: more strikes than shots.
    // The side has missed nothing, and the count must not wrap round to four billion.
    Scenario close;
    close.seed = kSeed;
    close.objectives = {};
    close.spawns = {Spawn{UnitKind::Tank, Side::Player, 60, 60},
                    Spawn{UnitKind::Recycler, Side::Enemy, 62,
                          60}}; // once a factory: 8 x 8 now, and the tank is 2 tiles off
    Sim sim = Sim::create(close);
    BattleStats stats{sim};
    // Under the player's hand the hull no longer fires on its own, so every shot counted below is
    // the trigger's.
    sim.enqueue(sim::Order{sim.units().front().id, sim::orders::TakeControl{}});
    run(sim, stats, kReloaded); // a new hull starts with its gun reloading
    const sim::UnitId enemy = sim.units().back().id;
    sim.enqueue(sim::Order{sim.units().front().id,
                           sim::orders::FireFan{sim::Fixed::from_int(1), sim::Fixed::zero(), enemy,
                                                sim::Fixed::zero()}});
    run(sim, stats, 1);
    ASSERT_EQ(stats.of(Side::Player).fired, 1U) << "one trigger pull, one shot";
    run(sim, stats, 20);
    ASSERT_GT(stats.of(Side::Player).hit, stats.of(Side::Player).fired)
        << "the rig must strike more often than it fires";
    EXPECT_EQ(stats.missed(Side::Player), 0U);
}

TEST(Statistics, ABuiltVehicleIsCountedForTheSideWhoseFactoryMadeIt) {
    for (const Side side : {Side::Player, Side::Enemy}) {
        Sim sim = Sim::create(factory_with_litter(side));
        BattleStats stats{sim};
        const sim::UnitId factory = sim.units().front().id;
        sim.enqueue(sim::Order{factory, sim::orders::Build{UnitKind::Scavenger, 0}});
        const Side other = side == Side::Player ? Side::Enemy : Side::Player;
        for (int i = 0; i < 20000 && stats.of(side).built == 0; ++i) {
            run(sim, stats, 1);
        }
        EXPECT_EQ(stats.of(side).built, 1U);
        EXPECT_EQ(stats.of(other).built, 0U);
        EXPECT_EQ(sim.units().size(), 5U) << "and it really was appended to the table";
    }
}

TEST(Statistics, ScrapCollectedIsTheSimsOwnTallyPerSideAndIsNotSpent) {
    for (const Side side : {Side::Player, Side::Enemy}) {
        Sim sim = Sim::create(factory_with_litter(side));
        const sim::UnitId factory = sim.units().front().id;
        const Side other = side == Side::Player ? Side::Enemy : Side::Player;
        for (int i = 0; i < 4000 && sim.ore(side) < 12; ++i) {
            sim.step();
        }
        ASSERT_GE(sim.ore(side), 12);
        EXPECT_EQ(sim.collected(side), sim.ore(side)) << "every load banked was collected";
        EXPECT_EQ(sim.collected(other), 0U);
        const auto cost = static_cast<std::uint16_t>(sim::spec_of(UnitKind::Scavenger).cost);
        for (int i = 0; i < 20000 && sim.ore(side) < cost; ++i) {
            sim.step();
        }
        ASSERT_GE(sim.ore(side), cost);
        const std::uint32_t before = sim.collected(side);
        const std::uint16_t banked = sim.ore(side);
        sim.enqueue(sim::Order{factory, sim::orders::Build{UnitKind::Scavenger, 0}});
        sim.step();
        sim.step();
        EXPECT_LT(sim.ore(side), banked + 1U) << "the build paid out of the bank";
        EXPECT_LT(sim.ore(side), sim.collected(side)) << "so the bank is below what was collected";
        EXPECT_GE(sim.collected(side), before) << "spending on a build does not un-collect";
    }
}

TEST(Statistics, ScrapExtractedFromAPoolIsCollectedForTheSideThatDeployedOnIt) {
    // The other way a load lands in a store: no litter anywhere, a scavenger deployed on a pool.
    for (const Side side : {Side::Player, Side::Enemy}) {
        Scenario scenario;
        scenario.seed = kSeed;
        scenario.objectives = {};
        sim::Pool pool{60, 40};
        for (int dx = 0; dx < 40 && !sim::tile_passable(kSeed, pool.tile_x, pool.tile_y); ++dx) {
            pool = sim::Pool{60 + dx, 40};
        }
        scenario.pools = {pool};
        scenario.spawns = {Spawn{UnitKind::Scavenger, side, pool.tile_x, pool.tile_y}};
        Sim sim = Sim::create(scenario);
        sim.enqueue(
            sim::Order{sim.units().front().id, sim::orders::Deploy{pool.tile_x, pool.tile_y}});
        for (int i = 0; i < 1500; ++i) {
            sim.step();
        }
        const Side other = side == Side::Player ? Side::Enemy : Side::Player;
        ASSERT_EQ(sim.units().front().kind, UnitKind::DeployedScavenger) << "the rig must deploy";
        ASSERT_GT(sim.ore(side), 0) << "and the pool must have paid out";
        EXPECT_EQ(sim.collected(side), sim.ore(side)) << "every load banked was collected";
        EXPECT_EQ(sim.collected(other), 0U);
    }
}

TEST(Statistics, CountingDoesNotMoveTheBattle) {
    Sim counted = Sim::create(fight());
    Sim plain = Sim::create(fight());
    BattleStats stats{counted};
    for (int i = 0; i < 600; ++i) {
        counted.step();
        stats.observe(counted);
        plain.step();
    }
    EXPECT_EQ(counted.state_hash(), plain.state_hash());
}

// ------------------------------------------------------------------------------------ the pause

class FakeSink final : public protekt::app::SampleSink {
public:
    int plays{0};
    [[nodiscard]] bool ok() const noexcept override { return true; }
    void play(std::span<const float> /*samples*/, int /*rate*/) noexcept override { ++plays; }
};

TEST(Pause, APausedPipelineStepsNothingAndMakesNoSoundEvenOnTheTickOfARadarSweep) {
    const Scenario scenario{
        1, {{UnitKind::Tank, Side::Player, 20, 20}, {UnitKind::Tank, Side::Enemy, 200, 200}}};
    Sim sim = Sim::create(scenario);
    Sim twin = Sim::create(scenario);
    protekt::game::HitFlash flash;
    protekt::game::HitMark mark;
    protekt::game::RadarActivity radar;
    FakeSink sink;
    const protekt::app::AudioPipeline pipeline{&sink};
    protekt::app::AudioFrame frame{protekt::game::Listener{}, false, true, true,
                                   sim.units().front().id};

    // Control: unpaused, a stretch that crosses a radar sweep IS heard. Without this the silence
    // below proves nothing.
    pipeline.advance(twin, protekt::game::kRadarSweepIntervalTicks, frame, flash, mark, radar);
    ASSERT_GT(sink.plays, 0) << "the rig must have a sound to silence";
    sink.plays = 0;

    frame.paused = true;
    pipeline.advance(sim, protekt::game::kRadarSweepIntervalTicks * 3, frame, flash, mark, radar);
    EXPECT_EQ(sim.tick(), 0U) << "a paused game does not advance the tick";
    EXPECT_EQ(sink.plays, 0) << "and nothing sounds while it is paused";
    EXPECT_EQ(sim.state_hash(), Sim::create(scenario).state_hash());

    frame.paused = false;
    pipeline.advance(sim, protekt::game::kRadarSweepIntervalTicks, frame, flash, mark, radar);
    EXPECT_EQ(sim.tick(), protekt::game::kRadarSweepIntervalTicks) << "and it resumes";
    EXPECT_GT(sink.plays, 0);
}

TEST(Pause, APausedPipelineCountsNothing) {
    Sim sim = Sim::create(fight());
    BattleStats stats{sim};
    protekt::game::HitFlash flash;
    protekt::game::HitMark mark;
    protekt::game::RadarActivity radar;
    const protekt::app::AudioPipeline pipeline{nullptr};
    protekt::app::AudioFrame frame{protekt::game::Listener{}, false, false, false, {}};
    frame.stats = &stats;
    frame.paused = true;
    pipeline.advance(sim, 600, frame, flash, mark, radar);
    EXPECT_EQ(stats.of(Side::Player).fired + stats.of(Side::Enemy).fired, 0U);
    frame.paused = false;
    pipeline.advance(sim, 600, frame, flash, mark, radar);
    EXPECT_GT(stats.of(Side::Player).fired, 0U) << "the pipeline feeds the tally while it runs";
}

TEST(Pause, EscapeIsABarePressOfTheEscapeKeyAndNothingElse) {
    using protekt::input::KeyEvent;
    EXPECT_TRUE(protekt::input::pause_pressed(KeyEvent{protekt::input::kKeyEscape, true}));
    EXPECT_FALSE(protekt::input::pause_pressed(KeyEvent{protekt::input::kKeyEscape, false}))
        << "the release is not a second toggle";
    EXPECT_FALSE(
        protekt::input::pause_pressed(KeyEvent{protekt::input::kKeyEscape, true, false, true}));
    EXPECT_FALSE(protekt::input::pause_pressed(KeyEvent{protekt::input::kKeyTab, true}));
}

TEST(Pause, AKeyLetGoDuringAPauseIsNotStillHeldOnResume) {
    protekt::game::Session session{sim::UnitId{1}};
    protekt::input::InputFrame down;
    down.keys.push_back(protekt::input::KeyEvent{protekt::input::kKeyW, true});
    session.track_keys(down);
    ASSERT_TRUE(session.keys().forward);
    protekt::input::InputFrame up;
    up.keys.push_back(protekt::input::KeyEvent{protekt::input::kKeyW, false});
    session.track_keys(up);
    EXPECT_FALSE(session.keys().forward);
}

// -------------------------------------------------------------------- the screen and the game loop

protekt::game::SceneOptions screen_options() {
    protekt::game::SceneOptions options;
    options.seed = kSeed;
    options.extent = protekt::game::Extent{320, 180};
    options.radius_tiles = 4;
    options.show_ledger = false;
    return options;
}

TEST(StatisticsScreen, EmitsNothingUnlessItIsAskedFor) {
    const Sim sim = Sim::create(fight());
    BattleStats stats{sim};
    protekt::game::SceneOptions options = screen_options();
    const std::size_t without = protekt::game::build_scene(sim, options).vertices.size();
    options.statistics = &stats;
    const std::size_t with = protekt::game::build_scene(sim, options).vertices.size();
    EXPECT_GT(with, without + 100U) << "a plate and a screenful of letters";
    options.statistics = nullptr;
    EXPECT_EQ(protekt::game::build_scene(sim, options).vertices.size(), without);
}

TEST(StatisticsScreen, EachSidesNumbersAreDrawnInThatSidesColour) {
    // The same tally, once with the player's side counting and once with the enemy's: the digits
    // moved to the other column and the other colour. Counted by ink, not by eye.
    const auto ink_count = [](const protekt::game::Scene& scene, float r, float g, float b) {
        return std::count_if(scene.vertices.begin(), scene.vertices.end(),
                             [&](const protekt::game::SceneVertex& vertex) {
                                 return std::abs(vertex.colour.at(0) - r) < 0.001F
                                        && std::abs(vertex.colour.at(1) - g) < 0.001F
                                        && std::abs(vertex.colour.at(2) - b) < 0.001F;
                             });
    };
    // `fight_mirrored`'s enemy wins; `fight`'s player does: so the tallies differ by side.
    Sim yours = Sim::create(fight());
    BattleStats yours_stats{yours};
    run(yours, yours_stats, 900);
    Sim theirs = Sim::create(fight_mirrored());
    BattleStats theirs_stats{theirs};
    run(theirs, theirs_stats, 900);

    protekt::game::SceneOptions options = screen_options();
    options.statistics = &yours_stats;
    const protekt::game::Scene a = protekt::game::build_scene(yours, options);
    options.statistics = &theirs_stats;
    const protekt::game::Scene b = protekt::game::build_scene(theirs, options);
    // YOU ink is (0.35, 1.0, 0.40); ENEMY ink is (1.0, 0.30, 0.25).
    EXPECT_NE(ink_count(a, 0.35F, 1.0F, 0.40F), ink_count(b, 0.35F, 1.0F, 0.40F));
    EXPECT_NE(ink_count(a, 1.0F, 0.30F, 0.25F), ink_count(b, 1.0F, 0.30F, 0.25F));
    EXPECT_GT(ink_count(a, 0.35F, 1.0F, 0.40F), 0);
    EXPECT_GT(ink_count(a, 1.0F, 0.30F, 0.25F), 0);
}

class PauseCliTest : public ::testing::Test {
protected:
    /// Runs a headless game and returns the `tick N, hash H` its capture line reports.
    static std::string tick_and_hash(std::vector<std::string> extra) {
        std::vector<std::string> storage{"--headless", "--seed", "77",        "--width",     "64",
                                         "--height",   "48",     "--capture", capture_path()};
        storage.insert(storage.end(), extra.begin(), extra.end());
        std::vector<std::string_view> args(storage.begin(), storage.end());
        std::ostringstream out;
        std::ostringstream err;
        EXPECT_EQ(protekt::app::run_game_cli(args, out, err), 0) << err.str();
        const std::string text = out.str();
        const std::size_t at = text.find(", tick ");
        EXPECT_NE(at, std::string::npos) << text;
        const std::size_t end = text.find(')', at);
        return text.substr(at, end - at);
    }
    static std::string capture_path() {
        return (std::filesystem::temp_directory_path() / "protekt_pause_test.ppm").string();
    }
};

#if defined(__SANITIZE_THREAD__)
constexpr bool kUnderThreadSanitizer = true;
#else
constexpr bool kUnderThreadSanitizer = false;
#endif

TEST_F(PauseCliTest, EscapePausesAndEscapeAgainResumes) {
    if (kUnderThreadSanitizer) {
        GTEST_SKIP() << "the headless renderer races inside the driver under TSan";
    }
    // 20 frames, one tick each. Paused on frame 6, resumed on frame 11: frames 6..10 step nothing.
    const std::string plain = tick_and_hash({"--frames", "20"});
    EXPECT_NE(plain.find("tick 20,"), std::string::npos) << plain;
    const std::string paused
        = tick_and_hash({"--frames", "20", "--key", "6", "escape", "--key", "11", "escape"});
    EXPECT_NE(paused.find("tick 15,"), std::string::npos)
        << "five paused frames must cost five ticks, and the resume must run again: " << paused;
    const std::string never_resumed = tick_and_hash({"--frames", "20", "--key", "6", "escape"});
    EXPECT_NE(never_resumed.find("tick 5,"), std::string::npos)
        << "ESC once pauses for good: " << never_resumed;
}

TEST_F(PauseCliTest, APausedBattleIsTheSameBattleAsOneThatWasNeverPaused) {
    if (kUnderThreadSanitizer) {
        GTEST_SKIP() << "the headless renderer races inside the driver under TSan";
    }
    // Same ticks, same hash: the pause is nowhere in the hashed state, and no order slipped in.
    const std::string plain = tick_and_hash({"--frames", "15"});
    const std::string paused
        = tick_and_hash({"--frames", "20", "--key", "6", "escape", "--key", "11", "escape"});
    EXPECT_EQ(plain, paused);
}

TEST_F(PauseCliTest, ThePausedFrameIsBlackWithBigBrightInkOnItNotAnEmptyPlate) {
    if (kUnderThreadSanitizer) {
        GTEST_SKIP() << "the headless renderer races inside the driver under TSan";
    }
    // Found in the capture, not in the vertices: a plate drawn as ONE quad sits at the depth of the
    // frame's corners, so the ink near the middle lay behind it and the screen was solid black. The
    // vertex tests could not see that. The pixels can.
    std::ostringstream out;
    std::ostringstream err;
    const std::string path = capture_path();
    const std::vector<std::string_view> args{
        "--headless", "--seed", "42",    "--frames", "10",     "--width",   "640",
        "--height",   "360",    "--key", "5",        "escape", "--capture", path};
    ASSERT_EQ(protekt::app::run_game_cli(args, out, err), 0) << err.str();
    std::ifstream file(path, std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    // "P6\n<w> <h>\n255\n" and then the pixels: the third newline ends the header.
    std::size_t body = 0;
    for (int newlines = 0; newlines < 3; ++body) {
        newlines += bytes.at(body) == '\n' ? 1 : 0;
    }
    ASSERT_EQ(bytes.compare(0, 3, "P6\n"), 0);
    const std::size_t width = 640;
    const std::size_t height = 360;
    ASSERT_EQ(bytes.size() - body, width * height * 3U);
    std::size_t black = 0;
    std::size_t bright = 0;
    for (std::size_t i = body; i + 2 < bytes.size(); i += 3) {
        const auto red = static_cast<unsigned char>(bytes.at(i));
        const auto green = static_cast<unsigned char>(bytes.at(i + 1));
        const auto blue = static_cast<unsigned char>(bytes.at(i + 2));
        black += (red < 8 && green < 8 && blue < 8) ? 1U : 0U;
        bright += (red >= 200 || green >= 200 || blue >= 200) ? 1U : 0U;
    }
    const std::size_t total = width * height;
    EXPECT_GT(black, total * 8U / 10U) << "a plate the whole frame wide: black " << black;
    EXPECT_GT(bright, total / 40U) << "and large ink on it: bright " << bright << " of " << total;
}

TEST_F(PauseCliTest, ATriggerPulledDuringThePauseIsNotRememberedWhenItEnds) {
    if (kUnderThreadSanitizer) {
        GTEST_SKIP() << "the headless renderer races inside the driver under TSan";
    }
    // The click on frame 18 lands in the middle of a pause (frames 16..20). A paused game takes no
    // orders: had the trigger been queued, the hull would fire on resume and the battle would
    // differ from one that was never paused. The control below shows the click DOES change a
    // battle when it is heard, so the equality is not vacuous.
    const std::string plain = tick_and_hash({"--frames", "40"});
    const std::string heard = tick_and_hash({"--frames", "40", "--fire", "18"});
    ASSERT_NE(plain, heard) << "a click that changes nothing proves nothing";
    const std::string paused = tick_and_hash(
        {"--frames", "45", "--key", "16", "escape", "--fire", "18", "--key", "21", "escape"});
    EXPECT_EQ(plain, paused);
}

} // namespace
