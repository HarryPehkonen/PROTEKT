// The radar: what it shows near you, and -- the part that matters -- what it refuses to show.
//
// These are pure geometry over a span of units, so they run anywhere, with no sim and no display.
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include "protekt/game/radar.hpp"
#include "protekt/sim/sim.hpp"

namespace {

using protekt::game::kRadarRadiusTiles;
using protekt::game::radar_marks;
using protekt::game::radar_panel;
using protekt::game::RadarMark;
using protekt::game::RadarPanel;
using protekt::game::RadarShape;
using protekt::game::RadarTone;
using protekt::render::Extent;
using protekt::sim::Fixed;
using protekt::sim::Side;
using protekt::sim::Unit;
using protekt::sim::UnitId;
using protekt::sim::UnitKind;

constexpr float kTolerance = 0.01F;

Unit unit_at(std::uint32_t id, float tile_x, float tile_y, Side side,
             UnitKind kind = UnitKind::Tank) {
    Unit u{};
    u.id = UnitId{id};
    u.kind = kind;
    u.side = side;
    u.x.raw = static_cast<std::int32_t>(tile_x * static_cast<float>(Fixed::kOne));
    u.y.raw = static_cast<std::int32_t>(tile_y * static_cast<float>(Fixed::kOne));
    u.alive = true;
    return u;
}

/// The viewer sits at the origin of a tidy little world, so every offset below reads as a compass
/// direction rather than as arithmetic.
constexpr float kViewerTile = 100.0F;

std::vector<Unit> with_viewer(std::vector<Unit> others, UnitKind viewer_kind = UnitKind::Tank) {
    std::vector<Unit> units;
    units.push_back(unit_at(1, kViewerTile, kViewerTile, Side::Player, viewer_kind));
    for (Unit& u : others) {
        u.x.raw += static_cast<std::int32_t>(kViewerTile * static_cast<float>(Fixed::kOne));
        u.y.raw += static_cast<std::int32_t>(kViewerTile * static_cast<float>(Fixed::kOne));
        units.push_back(u);
    }
    return units;
}

/// A unit `tiles` from the viewer, `degrees` clockwise from straight ahead, so tests can say
/// "twenty tiles dead ahead" and "ten tiles to my right" without doing trigonometry twice.
Unit at_bearing(std::uint32_t id, float tiles, float degrees, Side side,
                UnitKind kind = UnitKind::Tank) {
    const float radians = degrees * 3.14159265F / 180.0F;
    return unit_at(id, std::sin(radians) * tiles, std::cos(radians) * tiles, side, kind);
}

const RadarPanel kPanel = radar_panel(Extent{1280, 720});

/// The mark for the viewer: always first, so a test can find it without a magic index.
const RadarMark& viewer_mark(const std::vector<RadarMark>& marks) {
    EXPECT_FALSE(marks.empty());
    EXPECT_EQ(marks.front().tone, RadarTone::You);
    return marks.front();
}

float panel_middle(const RadarPanel& panel) { return panel.size * 0.5F; }

// ---------------------------------------------------------------- the panel in the frame

TEST(RadarPanel, SitsInTheBottomLeftCornerWithAMargin) {
    const RadarPanel panel = radar_panel(Extent{1280, 720});
    EXPECT_FLOAT_EQ(panel.x, protekt::game::kRadarMarginPixels);
    EXPECT_FLOAT_EQ(panel.y + panel.size + protekt::game::kRadarMarginPixels, 720.0F);
    EXPECT_GT(panel.size, 100.0F);
}

TEST(RadarPanel, FollowsTheFrameWidthAndIsClampedAtBothEnds) {
    // A small window still gets a usable radar, a huge one does not get an absurd one.
    const float small = radar_panel(Extent{640, 360}).size;
    const float wide = radar_panel(Extent{1280, 720}).size;
    const float huge = radar_panel(Extent{3840, 2160}).size;
    EXPECT_FLOAT_EQ(small, protekt::game::kRadarMinPixels);
    EXPECT_GT(wide, small);
    EXPECT_FLOAT_EQ(huge, protekt::game::kRadarMaxPixels);
    // And in every case the panel fits inside the frame it was asked about.
    for (const Extent extent : {Extent{640, 360}, Extent{1280, 720}, Extent{3840, 2160}}) {
        const RadarPanel panel = radar_panel(extent);
        EXPECT_GE(panel.x, 0.0F);
        EXPECT_GE(panel.y, 0.0F);
        EXPECT_LE(panel.y + panel.size, static_cast<float>(extent.height));
        EXPECT_LE(panel.x + panel.size, static_cast<float>(extent.width));
    }
}

TEST(RadarPanel, ShrinksToFitAWindowTooSmallForTheUsualSize) {
    const RadarPanel panel = radar_panel(Extent{400, 200});
    EXPECT_LE(panel.y + panel.size, 200.0F);
    EXPECT_GT(panel.size, 0.0F);
}

// ---------------------------------------------------------------- you, at the centre

TEST(RadarYou, SitsAtTheMiddlePointingUp) {
    const std::vector<Unit> units = with_viewer({});
    const std::vector<RadarMark> marks = radar_marks(units, UnitId{1}, 0.0F, kPanel);
    const RadarMark& you = viewer_mark(marks);
    EXPECT_NEAR(you.px, panel_middle(kPanel), kTolerance);
    EXPECT_NEAR(you.py, panel_middle(kPanel), kTolerance);
    EXPECT_EQ(you.shape, RadarShape::Triangle);
    EXPECT_FLOAT_EQ(you.angle, 0.0F); // heading-up: you always point up
}

TEST(RadarYou, IsDrawnOnceAndItsOwnUnitIsNotMarkedTwice) {
    const std::vector<Unit> units = with_viewer({});
    const std::vector<RadarMark> marks = radar_marks(units, UnitId{1}, 0.0F, kPanel);
    EXPECT_EQ(marks.size(), 1U);
}

TEST(RadarYou, WithoutAViewerUnitThereIsNothingAtAll) {
    // A radar with no centre would be a lie about where the tank is. An invalid id, a missing unit
    // mean the same thing: no panel. (A dead one is different: see RadarYou.)
    const std::vector<Unit> units = with_viewer({});
    EXPECT_TRUE(radar_marks(units, UnitId{}, 0.0F, kPanel).empty());
    EXPECT_TRUE(radar_marks(units, UnitId{999}, 0.0F, kPanel).empty());
}

TEST(RadarYou, ADeadViewerIsStillTheCentreAndTheLivingAreStillMarked) {
    // Death leaves a spectator, not a blank screen.
    std::vector<Unit> units = with_viewer(
        {at_bearing(2, 10.0F, 0.0F, Side::Enemy),
         at_bearing(3, static_cast<float>(kRadarRadiusTiles) + 5.0F, 0.0F, Side::Enemy)});
    units.front().alive = false;
    const std::vector<RadarMark> marks = radar_marks(units, UnitId{1}, 0.0F, kPanel);
    ASSERT_EQ(marks.size(), 2U) << "the viewer and the one living unit in radius";
    EXPECT_NEAR(viewer_mark(marks).px, panel_middle(kPanel), kTolerance);
    EXPECT_LT(marks.at(1).py, panel_middle(kPanel));
}

TEST(RadarYou, ARadiusOfZeroDrawsNothingRatherThanEverything) {
    const std::vector<Unit> units = with_viewer({});
    EXPECT_TRUE(radar_marks(units, UnitId{1}, 0.0F, kPanel, 0).empty());
}

// ---------------------------------------------------------------- directions

TEST(RadarDirections, StraightAheadIsUpOnThePanel) {
    // The first mark after "you" is twenty tiles dead ahead: it must be ABOVE the middle, and the
    // panel's y runs from the top, so that means a smaller py.
    const std::vector<Unit> units = with_viewer({at_bearing(2, 20.0F, 0.0F, Side::Enemy)});
    const std::vector<RadarMark> marks = radar_marks(units, UnitId{1}, 0.0F, kPanel);
    ASSERT_EQ(marks.size(), 2U);
    EXPECT_NEAR(marks.at(1).px, panel_middle(kPanel), kTolerance);
    EXPECT_LT(marks.at(1).py, panel_middle(kPanel));
    EXPECT_EQ(marks.at(1).tone, RadarTone::Hostile);
}

TEST(RadarDirections, ToYourRightIsRightOnThePanel) {
    // At yaw 0 the camera looks down +Z and its right is +X, so the world's +X is on the RIGHT of
    // the panel -- the same side of the screen the projection draws it on (the oracle in
    // test_math).
    const std::vector<Unit> units = with_viewer({at_bearing(2, 20.0F, 90.0F, Side::Enemy)});
    const std::vector<RadarMark> marks = radar_marks(units, UnitId{1}, 0.0F, kPanel);
    ASSERT_EQ(marks.size(), 2U);
    EXPECT_NEAR(marks.at(1).py, panel_middle(kPanel), kTolerance);
    EXPECT_GT(marks.at(1).px, panel_middle(kPanel));
}

TEST(RadarDirections, ThePanelTurnsWithTheTank) {
    // The same unit, seen at two headings. Heading-up means it moves on the panel when you turn,
    // which is what makes "turn towards it" readable at a glance.
    const std::vector<Unit> units = with_viewer({at_bearing(2, 20.0F, 0.0F, Side::Enemy)});
    const RadarMark ahead = radar_marks(units, UnitId{1}, 0.0F, kPanel).at(1);
    const RadarMark turned = radar_marks(units, UnitId{1}, 1.57079632F, kPanel).at(1);
    EXPECT_LT(ahead.py, panel_middle(kPanel)); // dead ahead: up
    EXPECT_NEAR(ahead.px, panel_middle(kPanel), kTolerance);
    EXPECT_NEAR(turned.py, panel_middle(kPanel),
                kTolerance);                    // a quarter turn later: level with you
    EXPECT_NE(turned.px, panel_middle(kPanel)); // ...and off to one side
    EXPECT_NE(turned.px, ahead.px);
}

TEST(RadarDirections, DistanceIsToScale) {
    // Half the radius is half the panel's radius from the middle -- the one thing a radar must get
    // right, because it is how "how far away is it" is read.
    const std::vector<Unit> units = with_viewer(
        {at_bearing(2, 20.0F, 0.0F, Side::Enemy), at_bearing(3, 40.0F, 0.0F, Side::Enemy)});
    const std::vector<RadarMark> marks = radar_marks(units, UnitId{1}, 0.0F, kPanel);
    ASSERT_EQ(marks.size(), 3U);
    const float middle = panel_middle(kPanel);
    const float near_offset = middle - marks.at(1).py;
    const float far_offset = middle - marks.at(2).py;
    EXPECT_NEAR(far_offset, 2.0F * near_offset, 0.5F);
    EXPECT_GT(far_offset, 0.0F);
}

// ---------------------------------------------------------------- the radius is a promise

TEST(RadarRange, AHostileInsideTheRadiusIsMarked) {
    const std::vector<Unit> units
        = with_viewer({at_bearing(2, static_cast<float>(kRadarRadiusTiles), 30.0F, Side::Enemy)});
    const std::vector<RadarMark> marks = radar_marks(units, UnitId{1}, 0.0F, kPanel);
    ASSERT_EQ(marks.size(), 2U);
    EXPECT_EQ(marks.at(1).tone, RadarTone::Hostile);
    EXPECT_EQ(marks.at(1).shape, RadarShape::Square);
}

TEST(RadarRange, AHostileJustOutsideTheRadiusIsNotMarkedAtAll) {
    // This is the whole design. Not dimmed, not on the rim, not an arrow: absent. A player must not
    // be able to see an enemy asset he has no business knowing about.
    const std::vector<Unit> units = with_viewer(
        {at_bearing(2, static_cast<float>(kRadarRadiusTiles) + 0.5F, 30.0F, Side::Enemy)});
    const std::vector<RadarMark> marks = radar_marks(units, UnitId{1}, 0.0F, kPanel);
    EXPECT_EQ(marks.size(), 1U); // just you
}

TEST(RadarRange, AFarAwayHostileIsStillNotMarkedWhenItIsAlmostInRange) {
    const std::vector<Unit> units = with_viewer(
        {at_bearing(2, static_cast<float>(kRadarRadiusTiles) * 3.0F, 200.0F, Side::Enemy)});
    EXPECT_EQ(radar_marks(units, UnitId{1}, 0.0F, kPanel).size(), 1U);
}

TEST(RadarRange, YourOwnUnitsOutsideTheRadiusAreDroppedToo) {
    // Symmetry, and a readable panel: the radar is not a free roster of your own spread.
    const std::vector<Unit> units = with_viewer(
        {at_bearing(2, static_cast<float>(kRadarRadiusTiles) + 1.0F, 0.0F, Side::Player)});
    EXPECT_EQ(radar_marks(units, UnitId{1}, 0.0F, kPanel).size(), 1U);
}

TEST(RadarRange, DeadUnitsAreNotMarked) {
    std::vector<Unit> units = with_viewer({at_bearing(2, 5.0F, 0.0F, Side::Enemy)});
    units.at(1).alive = false;
    EXPECT_EQ(radar_marks(units, UnitId{1}, 0.0F, kPanel).size(), 1U);
}

TEST(RadarRange, AHostileSittingOnTopOfYouIsMarkedAndDoesNotDivideByZero) {
    const std::vector<Unit> units = with_viewer({at_bearing(2, 0.0F, 0.0F, Side::Enemy)});
    const std::vector<RadarMark> marks = radar_marks(units, UnitId{1}, 0.0F, kPanel);
    ASSERT_EQ(marks.size(), 2U);
    for (const RadarMark& mark : marks) {
        EXPECT_TRUE(std::isfinite(mark.px));
        EXPECT_TRUE(std::isfinite(mark.py));
    }
}

// ---------------------------------------------------------------- your base, always

TEST(RadarBase, YourBaseInRangeIsAHollowSquareAndNotAnArrow) {
    const std::vector<Unit> units
        = with_viewer({at_bearing(2, 10.0F, 45.0F, Side::Player, UnitKind::Factory)});
    const std::vector<RadarMark> marks = radar_marks(units, UnitId{1}, 0.0F, kPanel);
    ASSERT_EQ(marks.size(), 2U);
    EXPECT_EQ(marks.at(1).shape, RadarShape::Hollow);
    EXPECT_EQ(marks.at(1).tone, RadarTone::Yours);
}

TEST(RadarBase, YourBaseOutOfRangeBecomesAnArrowAtTheRim) {
    // The one thing the player is never lost about. A hundred and twenty tiles away: past the edge
    // of the panel, so the mark is placed at the rim, still inside it, pointing the way to turn.
    const std::vector<Unit> units
        = with_viewer({at_bearing(2, 120.0F, 0.0F, Side::Player, UnitKind::Factory)});
    const std::vector<RadarMark> marks = radar_marks(units, UnitId{1}, 0.0F, kPanel);
    ASSERT_EQ(marks.size(), 2U);
    const RadarMark& arrow = marks.at(1);
    EXPECT_EQ(arrow.shape, RadarShape::Arrow);
    EXPECT_EQ(arrow.tone, RadarTone::Yours);

    const float middle = panel_middle(kPanel);
    EXPECT_NEAR(arrow.px, middle, 1.0F);    // dead ahead
    EXPECT_LT(arrow.py, middle);            // so the arrow is above the middle
    EXPECT_GT(arrow.py - arrow.half, 0.0F); // and wholly inside the panel
    EXPECT_LT(arrow.py + arrow.half, kPanel.size);
    EXPECT_FLOAT_EQ(arrow.angle, 0.0F); // pointing straight up: turn forwards
}

TEST(RadarBase, AnArrowPointsTheWayToTurn) {
    // Ninety degrees clockwise from ahead: the arrow must sit on the panel's side, not above or
    // below, and it must still be inside the panel.
    const std::vector<Unit> units
        = with_viewer({at_bearing(2, 200.0F, 90.0F, Side::Player, UnitKind::Factory)});
    const std::vector<RadarMark> marks = radar_marks(units, UnitId{1}, 0.0F, kPanel);
    ASSERT_EQ(marks.size(), 2U);
    const RadarMark& arrow = marks.at(1);
    const float middle = panel_middle(kPanel);
    EXPECT_NEAR(arrow.py, middle, 1.0F);
    EXPECT_NE(arrow.px, middle);
    EXPECT_GE(arrow.px - arrow.half, 0.0F);
    EXPECT_LE(arrow.px + arrow.half, kPanel.size);
}

TEST(RadarBase, AFarAwayBaseIsNotLostWhenYouTurn) {
    // The arrow is a direction, so it must survive the panel turning: it stays at the same distance
    // from the middle, in a different place.
    const std::vector<Unit> units
        = with_viewer({at_bearing(2, 120.0F, 0.0F, Side::Player, UnitKind::Factory)});
    const RadarMark ahead = radar_marks(units, UnitId{1}, 0.0F, kPanel).at(1);
    const RadarMark turned = radar_marks(units, UnitId{1}, 3.14159265F, kPanel).at(1);
    const float middle = panel_middle(kPanel);
    EXPECT_NEAR(std::hypot(ahead.px - middle, ahead.py - middle),
                std::hypot(turned.px - middle, turned.py - middle), 0.5F);
    EXPECT_GT(turned.py, middle); // half a turn later it is behind you: below the middle
}

TEST(RadarBase, AnEnemyBaseOutOfRangeIsNotMarkedJustBecauseItIsABase) {
    // Their factories are assets, exactly like their tanks: outside the radius, nothing.
    const std::vector<Unit> units
        = with_viewer({at_bearing(2, 120.0F, 0.0F, Side::Enemy, UnitKind::Factory)});
    EXPECT_EQ(radar_marks(units, UnitId{1}, 0.0F, kPanel).size(), 1U);
}

TEST(RadarBase, AnEnemyBaseInRangeIsARedSquareAndNotYourBase) {
    const std::vector<Unit> units
        = with_viewer({at_bearing(2, 10.0F, 45.0F, Side::Enemy, UnitKind::Factory)});
    const std::vector<RadarMark> marks = radar_marks(units, UnitId{1}, 0.0F, kPanel);
    ASSERT_EQ(marks.size(), 2U);
    EXPECT_EQ(marks.at(1).shape, RadarShape::Square);
    EXPECT_EQ(marks.at(1).tone, RadarTone::Hostile);
}

// ---------------------------------------------------------------- the panel's own frame

TEST(RadarMarks, EveryMarkLiesInsideThePanel) {
    // A mark outside the panel is a mark drawn over the world with no panel under it, which is how
    // one ended up in the frame's top-left corner once. The panel's rectangle is the contract.
    const std::vector<Unit> units = with_viewer({
        at_bearing(2, 5.0F, 10.0F, Side::Enemy),
        at_bearing(3, 39.0F, 200.0F, Side::Player),
        at_bearing(4, 500.0F, 300.0F, Side::Player, UnitKind::Factory),
        at_bearing(5, 1.0F, 355.0F, Side::Player, UnitKind::Turret),
    });
    const std::vector<RadarMark> marks = radar_marks(units, UnitId{1}, 0.7F, kPanel);
    EXPECT_EQ(marks.size(), 5U);
    for (const RadarMark& mark : marks) {
        EXPECT_GE(mark.px - mark.half, 0.0F);
        EXPECT_LE(mark.px + mark.half, kPanel.size);
        EXPECT_GE(mark.py - mark.half, 0.0F);
        EXPECT_LE(mark.py + mark.half, kPanel.size);
    }
}

TEST(RadarMarks, ABaseIsBiggerThanATankAndYouAreInBetween) {
    const std::vector<Unit> units = with_viewer({
        at_bearing(2, 5.0F, 0.0F, Side::Player),
        at_bearing(3, 6.0F, 0.0F, Side::Player, UnitKind::Factory),
    });
    const std::vector<RadarMark> marks = radar_marks(units, UnitId{1}, 0.0F, kPanel);
    ASSERT_EQ(marks.size(), 3U);
    EXPECT_GT(marks.at(2).half, marks.at(1).half); // the base reads bigger than a tank
    EXPECT_GT(marks.at(0).half, marks.at(1).half); // and you read bigger than both
}

TEST(RadarMarks, TheSameStateGivesTheSameMarksInTheSameOrder) {
    const std::vector<Unit> units = with_viewer({
        at_bearing(2, 5.0F, 10.0F, Side::Enemy),
        at_bearing(3, 15.0F, 200.0F, Side::Player),
    });
    const std::vector<RadarMark> first = radar_marks(units, UnitId{1}, 0.3F, kPanel);
    const std::vector<RadarMark> second = radar_marks(units, UnitId{1}, 0.3F, kPanel);
    ASSERT_EQ(first.size(), second.size());
    for (std::size_t i = 0; i < first.size(); ++i) {
        EXPECT_FLOAT_EQ(first.at(i).px, second.at(i).px);
        EXPECT_FLOAT_EQ(first.at(i).py, second.at(i).py);
        EXPECT_EQ(first.at(i).tone, second.at(i).tone);
        EXPECT_EQ(first.at(i).shape, second.at(i).shape);
    }
}

} // namespace
