// The radar: what it shows near you, and -- the part that matters -- what it refuses to show.
//
// These are pure geometry over a span of units, so they run anywhere, with no sim and no display.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "protekt/game/radar.hpp"
#include "protekt/game/scene.hpp" // kMetresPerTile and height_metres: what a metre is, in tiles
#include "protekt/sim/sim.hpp"

namespace {

using protekt::game::Extent;
using protekt::game::kRadarAspect;
using protekt::game::kRadarElevationRadians;
using protekt::game::kRadarRadiusTiles;
using protekt::game::kScoutRadarRadiusTiles;
using protekt::game::radar_compass;
using protekt::game::radar_marks;
using protekt::game::radar_panel;
using protekt::game::radar_project;
using protekt::game::RadarLetter;
using protekt::game::RadarMark;
using protekt::game::RadarPanel;
using protekt::game::RadarPoint;
using protekt::game::RadarShape;
using protekt::game::RadarTone;
using protekt::game::viewer_radar_radius;
using protekt::sim::Fixed;
using protekt::sim::Scenario;
using protekt::sim::Side;
using protekt::sim::Sim;
using protekt::sim::Spawn;
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

float panel_middle(const RadarPanel& panel) { return panel.width * 0.5F; }
float panel_middle_y(const RadarPanel& panel) { return panel.height * 0.5F; }

/// Pixels per tile on the panel's x axis, and on its y axis: the tilt squeezes y by sin(elevation).
float pixels_per_tile(const RadarPanel& panel) {
    return panel.width / (2.0F * static_cast<float>(kRadarRadiusTiles));
}
float squeeze() { return std::sin(kRadarElevationRadians); }

/// Where a point sits against the oval face, 1 on the rim: the ellipse equation, semi-axes in
/// pixels.
float ellipse_value(const RadarPanel& panel, float px, float py, float inset = 0.0F) {
    const float a = (panel.width * 0.5F) - inset;
    const float b = (panel.height * 0.5F) - inset;
    const float x = (px - panel_middle(panel)) / a;
    const float y = (py - panel_middle_y(panel)) / b;
    return (x * x) + (y * y);
}

// ---------------------------------------------------------------- the panel in the frame

TEST(RadarPanel, SitsInTheBottomLeftCornerWithAMargin) {
    const RadarPanel panel = radar_panel(Extent{1280, 720});
    EXPECT_FLOAT_EQ(panel.x, protekt::game::kPanelMarginPixels);
    EXPECT_FLOAT_EQ(panel.y + panel.height + protekt::game::kPanelMarginPixels, 720.0F);
    EXPECT_GT(panel.width, 100.0F);
}

TEST(RadarPanel, IsBZ2sWideShallowOval) {
    // Measured off the user's own screenshots: the disc is 0.258 of the frame wide and 0.358 as
    // tall as it is wide -- a circular ground disc seen from 21 degrees up.
    const RadarPanel panel = radar_panel(Extent{1280, 720});
    EXPECT_NEAR(panel.width, 0.258F * 1280.0F, 1.0F);
    EXPECT_NEAR(panel.height, 118.0F, 1.0F);
    EXPECT_NEAR(panel.height / panel.width, kRadarAspect, 0.003F);
    EXPECT_NEAR(kRadarAspect, std::sin(kRadarElevationRadians), 0.001F)
        << "the oval is the ground circle seen at this elevation";
}

TEST(RadarPanel, FollowsTheFrameWidthAndIsClampedAtBothEnds) {
    // A small window still gets a usable radar, a huge one does not get an absurd one.
    const float small = radar_panel(Extent{640, 360}).width;
    const float wide = radar_panel(Extent{1280, 720}).width;
    const float huge = radar_panel(Extent{3840, 2160}).width;
    EXPECT_FLOAT_EQ(small, protekt::game::kRadarMinPixels);
    EXPECT_GT(wide, small);
    EXPECT_FLOAT_EQ(huge, protekt::game::kRadarMaxPixels);
    // And in every case the panel fits inside the frame it was asked about.
    for (const Extent extent : {Extent{640, 360}, Extent{1280, 720}, Extent{3840, 2160}}) {
        const RadarPanel panel = radar_panel(extent);
        EXPECT_GE(panel.x, 0.0F);
        EXPECT_GE(panel.y, 0.0F);
        EXPECT_LE(panel.y + panel.height, static_cast<float>(extent.height));
        EXPECT_LE(panel.x + panel.width, static_cast<float>(extent.width));
        EXPECT_NEAR(panel.height, panel.width * kRadarAspect, 0.01F);
    }
}

TEST(RadarPanel, ShrinksToFitAWindowTooSmallForTheUsualSize) {
    const RadarPanel panel = radar_panel(Extent{400, 200});
    EXPECT_LE(panel.y + panel.height, 200.0F);
    EXPECT_GT(panel.width, 0.0F);
    EXPECT_NEAR(panel.height, panel.width * kRadarAspect, 0.01F);
}

// ---------------------------------------------------------------- you, at the centre

TEST(RadarYou, SitsAtTheMiddlePointingUp) {
    const std::vector<Unit> units = with_viewer({});
    const std::vector<RadarMark> marks = radar_marks(units, UnitId{1}, 0.0F, kPanel);
    const RadarMark& you = viewer_mark(marks);
    EXPECT_NEAR(you.px, panel_middle(kPanel), kTolerance);
    EXPECT_NEAR(you.py, panel_middle_y(kPanel), kTolerance);
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
    EXPECT_LT(marks.at(1).py, panel_middle_y(kPanel));
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
    EXPECT_LT(marks.at(1).py, panel_middle_y(kPanel));
    EXPECT_EQ(marks.at(1).tone, RadarTone::Hostile);
}

TEST(RadarDirections, ToYourRightIsRightOnThePanel) {
    // At yaw 0 the camera looks down +Z and its right is +X, so the world's +X is on the RIGHT of
    // the panel -- the same side of the screen the projection draws it on (the oracle in
    // test_math).
    const std::vector<Unit> units = with_viewer({at_bearing(2, 20.0F, 90.0F, Side::Enemy)});
    const std::vector<RadarMark> marks = radar_marks(units, UnitId{1}, 0.0F, kPanel);
    ASSERT_EQ(marks.size(), 2U);
    EXPECT_NEAR(marks.at(1).py, panel_middle_y(kPanel), kTolerance);
    EXPECT_GT(marks.at(1).px, panel_middle(kPanel));
}

TEST(RadarDirections, ThePanelTurnsWithTheTank) {
    // The same unit, seen at two headings. Heading-up means it moves on the panel when you turn,
    // which is what makes "turn towards it" readable at a glance.
    const std::vector<Unit> units = with_viewer({at_bearing(2, 20.0F, 0.0F, Side::Enemy)});
    const RadarMark ahead = radar_marks(units, UnitId{1}, 0.0F, kPanel).at(1);
    const RadarMark turned = radar_marks(units, UnitId{1}, 1.57079632F, kPanel).at(1);
    EXPECT_LT(ahead.py, panel_middle_y(kPanel)); // dead ahead: up
    EXPECT_NEAR(ahead.px, panel_middle(kPanel), kTolerance);
    EXPECT_NEAR(turned.py, panel_middle_y(kPanel),
                kTolerance);                    // a quarter turn later: level with you
    EXPECT_NE(turned.px, panel_middle(kPanel)); // ...and off to one side
    EXPECT_NE(turned.px, ahead.px);
}

TEST(RadarDirections, DistanceIsToScale) {
    // Twice the distance is twice the offset from the middle -- the one thing a radar must get
    // right, because it is how "how far away is it" is read. (Kept well inside the oval, where a
    // mark is not pinned.)
    const std::vector<Unit> units = with_viewer(
        {at_bearing(2, 10.0F, 0.0F, Side::Enemy), at_bearing(3, 20.0F, 0.0F, Side::Enemy)});
    const std::vector<RadarMark> marks = radar_marks(units, UnitId{1}, 0.0F, kPanel);
    ASSERT_EQ(marks.size(), 3U);
    const float middle = panel_middle_y(kPanel);
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
    EXPECT_NEAR(arrow.px, middle, 1.0F);         // dead ahead
    EXPECT_LT(arrow.py, panel_middle_y(kPanel)); // so the arrow is above the middle
    EXPECT_GT(arrow.py - arrow.half, 0.0F);      // and wholly inside the panel
    EXPECT_LT(arrow.py + arrow.half, kPanel.height);
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
    EXPECT_NEAR(arrow.py, panel_middle_y(kPanel), 1.0F);
    EXPECT_NE(arrow.px, middle);
    EXPECT_GE(arrow.px - arrow.half, 0.0F);
    EXPECT_LE(arrow.px + arrow.half, kPanel.width);
}

TEST(RadarBase, AFarAwayBaseIsNotLostWhenYouTurn) {
    // The arrow is a direction, so it must survive the panel turning: it stays at the same distance
    // from the middle, in a different place.
    const std::vector<Unit> units
        = with_viewer({at_bearing(2, 120.0F, 0.0F, Side::Player, UnitKind::Factory)});
    const RadarMark ahead = radar_marks(units, UnitId{1}, 0.0F, kPanel).at(1);
    const RadarMark turned = radar_marks(units, UnitId{1}, 3.14159265F, kPanel).at(1);
    const float middle = panel_middle(kPanel);
    EXPECT_NEAR(std::hypot(ahead.px - middle, ahead.py - panel_middle_y(kPanel)),
                std::hypot(turned.px - middle, turned.py - panel_middle_y(kPanel)), 0.5F);
    EXPECT_GT(turned.py, panel_middle_y(kPanel)); // half a turn later it is behind you: below
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
        EXPECT_LE(mark.px + mark.half, kPanel.width);
        EXPECT_GE(mark.py - mark.half, 0.0F);
        EXPECT_LE(mark.py + mark.half, kPanel.height);
    }
}

TEST(RadarMarks, EveryMarkBoxLiesInsideTheOvalAndItsBorder) {
    // The oval, not the rectangle, is the contract: the corners of a mark's box must be on the face
    // or on the border ring, whatever the heading and however far out the unit is.
    const std::vector<Unit> units = with_viewer({
        at_bearing(2, 40.0F, 0.0F, Side::Enemy),
        at_bearing(3, 40.0F, 90.0F, Side::Enemy),
        at_bearing(4, 39.0F, 135.0F, Side::Enemy, UnitKind::Factory),
        at_bearing(5, 40.0F, 180.0F, Side::Player),
        at_bearing(6, 500.0F, 300.0F, Side::Player, UnitKind::Factory),
        at_bearing(7, 500.0F, 0.0F, Side::Player, UnitKind::Factory),
    });
    const float border = protekt::game::kRadarBorderPixels;
    for (const float yaw : {0.0F, 0.7F, 2.0F, -1.3F}) {
        for (const RadarMark& mark : radar_marks(units, UnitId{1}, yaw, kPanel)) {
            for (const float dx : {-mark.half, mark.half}) {
                for (const float dy : {-mark.half, mark.half}) {
                    EXPECT_LE(ellipse_value(kPanel, mark.px + dx, mark.py + dy, -border), 1.0F)
                        << "yaw " << yaw << " shape " << static_cast<int>(mark.shape);
                }
            }
        }
    }
}

// ---------------------------------------------------------------- the tilted plane

TEST(RadarProject, TheMiddleOfThePanelIsTheViewer) {
    const RadarPoint p = radar_project(kPanel, 0.0F, 0.0F);
    EXPECT_NEAR(p.px, panel_middle(kPanel), kTolerance);
    EXPECT_NEAR(p.py, panel_middle_y(kPanel), kTolerance);
}

TEST(RadarProject, ForwardIsUpTheVerticalCentreLine) {
    const RadarPoint p = radar_project(kPanel, 0.0F, 20.0F);
    EXPECT_NEAR(p.px, panel_middle(kPanel), kTolerance);
    EXPECT_NEAR(p.py, panel_middle_y(kPanel) - (20.0F * pixels_per_tile(kPanel) * squeeze()),
                0.01F);
    EXPECT_LT(p.py, panel_middle_y(kPanel)) << "above the centre";
}

TEST(RadarProject, SidewaysIsAlongTheHorizontalCentreLineAtFullScale) {
    const RadarPoint p = radar_project(kPanel, 20.0F, 0.0F);
    EXPECT_NEAR(p.py, panel_middle_y(kPanel), kTolerance);
    EXPECT_NEAR(p.px, panel_middle(kPanel) + (20.0F * pixels_per_tile(kPanel)), 0.01F);
}

TEST(RadarProject, TheRadiusLandsOnTheRimOfTheEllipseInEveryDirection) {
    const float radius = static_cast<float>(kRadarRadiusTiles);
    for (const float degrees : {0.0F, 30.0F, 90.0F, 150.0F, 180.0F, 250.0F, 315.0F}) {
        const float r = degrees * 3.14159265F / 180.0F;
        const RadarPoint p = radar_project(kPanel, std::sin(r) * radius, std::cos(r) * radius);
        EXPECT_NEAR(ellipse_value(kPanel, p.px, p.py), 1.0F, 0.01F) << degrees << " degrees";
    }
}

TEST(RadarProject, TwiceTheDistanceIsTwiceAsFarInXAndForeshortenedInY) {
    // The tilt as a ratio a test can hold: along x a tile is `s` pixels, along y it is only
    // `s * sin(elevation)` -- 0.358 as many.
    const RadarPoint one = radar_project(kPanel, 10.0F, 10.0F);
    const RadarPoint two = radar_project(kPanel, 20.0F, 20.0F);
    const float dx1 = one.px - panel_middle(kPanel);
    const float dx2 = two.px - panel_middle(kPanel);
    const float dy1 = panel_middle_y(kPanel) - one.py;
    const float dy2 = panel_middle_y(kPanel) - two.py;
    EXPECT_NEAR(dx2, 2.0F * dx1, 0.01F);
    EXPECT_NEAR(dy2, 2.0F * dy1, 0.01F);
    EXPECT_NEAR(dy1 / dx1, kRadarAspect, 0.001F) << "a tile ahead is 0.358 of a tile across";
}

TEST(RadarProject, ARadiusOfZeroDoesNotDivideByZero) {
    const RadarPoint p = radar_project(kPanel, 5.0F, 5.0F, 0);
    EXPECT_TRUE(std::isfinite(p.px));
    EXPECT_TRUE(std::isfinite(p.py));
}

TEST(RadarMarks, ADueNorthMarkIsAboveTheCentreOnTheVerticalLineByTheProjection) {
    const std::vector<Unit> units = with_viewer({at_bearing(2, 25.0F, 0.0F, Side::Enemy)});
    const RadarMark m = radar_marks(units, UnitId{1}, 0.0F, kPanel).at(1);
    EXPECT_NEAR(m.px, panel_middle(kPanel), kTolerance);
    EXPECT_NEAR(panel_middle_y(kPanel) - m.py, 25.0F * pixels_per_tile(kPanel) * squeeze(), 0.05F);
}

TEST(RadarMarks, ADueEastMarkIsOnTheHorizontalCentreLine) {
    const std::vector<Unit> units = with_viewer({at_bearing(2, 25.0F, 90.0F, Side::Enemy)});
    const RadarMark m = radar_marks(units, UnitId{1}, 0.0F, kPanel).at(1);
    EXPECT_NEAR(m.py, panel_middle_y(kPanel), kTolerance);
    EXPECT_NEAR(m.px - panel_middle(kPanel), 25.0F * pixels_per_tile(kPanel), 0.05F);
}

TEST(RadarMarks, TwiceTheDistanceMovesTwiceAsFarInXAndAsFarTimesTheAspectInY) {
    const std::vector<Unit> units = with_viewer({
        at_bearing(2, 8.0F, 45.0F, Side::Enemy),
        at_bearing(3, 16.0F, 45.0F, Side::Enemy),
    });
    const std::vector<RadarMark> marks = radar_marks(units, UnitId{1}, 0.0F, kPanel);
    const float dx1 = marks.at(1).px - panel_middle(kPanel);
    const float dx2 = marks.at(2).px - panel_middle(kPanel);
    const float dy1 = panel_middle_y(kPanel) - marks.at(1).py;
    const float dy2 = panel_middle_y(kPanel) - marks.at(2).py;
    EXPECT_NEAR(dx2, 2.0F * dx1, 0.05F);
    EXPECT_NEAR(dy2, 2.0F * dy1, 0.05F);
    EXPECT_NEAR(dy1 / dx1, kRadarAspect, 0.002F)
        << "at 45 degrees ahead and across are equal tiles";
}

TEST(RadarMarks, AMarkAtTheRadiusIsPinnedJustInsideTheRimNotPastIt) {
    // The projection puts it ON the rim; its box would then stick out of the oval, so it is drawn
    // pulled in by its own size -- the same pin the nav marks have always had.
    const std::vector<Unit> units
        = with_viewer({at_bearing(2, static_cast<float>(kRadarRadiusTiles), 90.0F, Side::Enemy)});
    const RadarMark m = radar_marks(units, UnitId{1}, 0.0F, kPanel).at(1);
    EXPECT_LE(ellipse_value(kPanel, m.px, m.py), 1.0F);
    EXPECT_GT(ellipse_value(kPanel, m.px, m.py), 0.7F)
        << "still out at the rim, not near the middle";
    EXPECT_NEAR(m.py, panel_middle_y(kPanel), kTolerance);
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

/// R3b2: a mortar and a rocket are units like any other -- the same square dot a tank gets, in
/// the same size and tone, with nothing special-cased for them.
TEST(RadarMarks, AMortarAndARocketAreMarkedExactlyAsATankIs) {
    const std::vector<Unit> units = with_viewer({
        at_bearing(2, 5.0F, 0.0F, Side::Enemy),
        at_bearing(3, 8.0F, 90.0F, Side::Enemy, UnitKind::Mortar),
        at_bearing(4, 11.0F, 180.0F, Side::Player, UnitKind::Rocket),
    });
    const std::vector<RadarMark> marks = radar_marks(units, UnitId{1}, 0.0F, kPanel);
    ASSERT_EQ(marks.size(), 4U);
    for (std::size_t i = 2; i < 4; ++i) {
        EXPECT_EQ(marks.at(i).shape, RadarShape::Square) << "mark " << i;
        EXPECT_FLOAT_EQ(marks.at(i).half, marks.at(1).half) << "mark " << i;
    }
    EXPECT_EQ(marks.at(2).tone, RadarTone::Hostile);
    EXPECT_EQ(marks.at(3).tone, RadarTone::Yours);
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

// The compass: N, E, S, W on the ring, true world directions (x east, z north, as the scene and the
// sky define them), placed from the same yaw the marks use.

const RadarLetter& letter(const std::array<RadarLetter, 4>& letters, char glyph) {
    for (const RadarLetter& l : letters) {
        if (l.glyph == glyph) {
            return l;
        }
    }
    ADD_FAILURE() << "no letter " << glyph;
    return letters.at(0);
}

TEST(RadarCompass, FacingNorthPutsNAtTheTopOfTheRing) {
    const auto letters = radar_compass(0.0F, kPanel);
    const float middle = kPanel.width * 0.5F;
    EXPECT_NEAR(letter(letters, 'N').px, middle, kTolerance);
    EXPECT_LT(letter(letters, 'N').py, panel_middle_y(kPanel)); // above the centre: the top
    EXPECT_GT(letter(letters, 'E').px, middle);                 // east is to the right
    EXPECT_NEAR(letter(letters, 'E').py, panel_middle_y(kPanel), kTolerance);
    EXPECT_GT(letter(letters, 'S').py, panel_middle_y(kPanel));
    EXPECT_LT(letter(letters, 'W').px, middle);
}

TEST(RadarCompass, TheLettersSitOnTheEllipseInsetFromTheRim) {
    for (const float yaw : {0.0F, 0.4F, 2.0F}) {
        for (const RadarLetter& l : radar_compass(yaw, kPanel)) {
            EXPECT_NEAR(ellipse_value(kPanel, l.px, l.py, protekt::game::kRadarLetterInsetPixels),
                        1.0F, 0.001F)
                << l.glyph << " at yaw " << yaw;
        }
    }
}

TEST(RadarCompass, TurningTheViewRotatesTheLettersTheOtherWay) {
    // Turned a quarter turn to the right (yaw +90), north is on the viewer's left: the top of the
    // panel is east. This is the same yaw the marks use: a unit due north lands where N does.
    constexpr float kQuarter = 1.57079632F;
    const auto letters = radar_compass(kQuarter, kPanel);
    const float middle = kPanel.width * 0.5F;
    EXPECT_LT(letter(letters, 'N').px, middle);
    EXPECT_NEAR(letter(letters, 'N').py, panel_middle_y(kPanel), kTolerance);
    EXPECT_NEAR(letter(letters, 'E').px, middle, kTolerance);
    EXPECT_LT(letter(letters, 'E').py, panel_middle_y(kPanel));

    // Agreement with the marks: a unit straight north, on the same yaw, is on N's side of the
    // panel.
    const std::vector<Unit> units = with_viewer({unit_at(2, 0.0F, 10.0F, Side::Enemy)});
    const RadarMark north_unit = radar_marks(units, UnitId{1}, 0.7F, kPanel).at(1);
    const auto turned = radar_compass(0.7F, kPanel);
    const RadarLetter& n = letter(turned, 'N');
    // The same bearing: the unit in the tilted plane's own terms (y un-squeezed), the letter in
    // terms of the ellipse it rides (each axis over its own semi-axis).
    const float my = panel_middle_y(kPanel);
    const float inset = protekt::game::kRadarLetterInsetPixels;
    EXPECT_NEAR(std::atan2(north_unit.px - middle, (my - north_unit.py) / kRadarAspect),
                std::atan2((n.px - middle) / (middle - inset), (my - n.py) / (my - inset)), 0.001F);
}

TEST(RadarCompass, ExactlyNESWAndNoDiagonals) {
    const auto letters = radar_compass(0.3F, kPanel);
    std::string glyphs;
    for (const RadarLetter& l : letters) {
        glyphs += l.glyph;
    }
    std::sort(glyphs.begin(), glyphs.end());
    EXPECT_EQ(glyphs, "ENSW");
    // Neighbours are a quarter turn apart: nothing sits at an eighth.
    // (A quarter turn on the ground, so measured with y un-squeezed.)
    const float middle = kPanel.width * 0.5F;
    const float my = panel_middle_y(kPanel);
    const auto bearing = [&](const RadarLetter& l) {
        return std::atan2((l.px - middle) / (middle - protekt::game::kRadarLetterInsetPixels),
                          (my - l.py) / (my - protekt::game::kRadarLetterInsetPixels));
    };
    EXPECT_NEAR(bearing(letter(letters, 'E')) - bearing(letter(letters, 'N')), 1.57079632F, 0.001F);
}

// ---------------------------------------------------------------- the terrain mesh (RADAR3D-2)
//
// The mesh is pure geometry over the world's own height field, so these run anywhere: no sim, no
// display. The pixel acceptance (a mesh-on frame differs from a mesh-off one ONLY inside the oval)
// belongs to the render tests; these pin the geometry that makes it possible.

using protekt::game::kRadarMeshLinePixels;
using protekt::game::kRadarMeshStepTiles;
using protekt::game::radar_mesh;
using protekt::game::RadarSegment;

/// A seed the mesh tests share: any seed would do, but a named one makes a failure reproducible.
constexpr std::uint64_t kMeshSeed = 424242;

/// The mesh's own yaw: straight up the panel, so a test that checks the tilt reads as arithmetic.
constexpr float kMeshYaw = 0.0F;

TEST(RadarMesh, EverySegmentLiesInsideTheOval) {
    // The oval is the contract, exactly as it is for marks: a segment's BOTH ends are on the face.
    // The mesh clips analytically in the panel's unit-disc space, so this must hold at every yaw
    // and at the mesh's own step, without a single line reaching a rectangle corner.
    for (const float yaw : {0.0F, 0.8F, -2.1F, 3.0F}) {
        const std::vector<RadarSegment> segments
            = radar_mesh(kMeshSeed, kViewerTile, kViewerTile, yaw, kPanel);
        ASSERT_FALSE(segments.empty()) << "yaw " << yaw << " drew no mesh at all";
        for (const RadarSegment& segment : segments) {
            EXPECT_LE(ellipse_value(kPanel, segment.a.px, segment.a.py), 1.0001F) << "yaw " << yaw;
            EXPECT_LE(ellipse_value(kPanel, segment.b.px, segment.b.py), 1.0001F) << "yaw " << yaw;
            EXPECT_TRUE(std::isfinite(segment.a.px) && std::isfinite(segment.a.py));
            EXPECT_TRUE(std::isfinite(segment.b.px) && std::isfinite(segment.b.py));
        }
    }
}

TEST(RadarMesh, IsDeterministicForASeedAndViewer) {
    // Same seed, same viewer, same yaw: the same list in the same order. The mesh is rebuilt every
    // frame and must not swim between two frames that changed nothing.
    const std::vector<RadarSegment> first
        = radar_mesh(kMeshSeed, kViewerTile, kViewerTile, 0.37F, kPanel);
    const std::vector<RadarSegment> second
        = radar_mesh(kMeshSeed, kViewerTile, kViewerTile, 0.37F, kPanel);
    ASSERT_EQ(first.size(), second.size());
    for (std::size_t i = 0; i < first.size(); ++i) {
        EXPECT_FLOAT_EQ(first.at(i).a.px, second.at(i).a.px);
        EXPECT_FLOAT_EQ(first.at(i).a.py, second.at(i).a.py);
        EXPECT_FLOAT_EQ(first.at(i).b.px, second.at(i).b.px);
        EXPECT_FLOAT_EQ(first.at(i).b.py, second.at(i).b.py);
    }
}

TEST(RadarMesh, EverySampledCornerIsTheWorldsOwnHeightAtThatTile) {
    // The coupling, checked point by point rather than in aggregate: a corner at a known tile is in
    // the mesh exactly where `height_metres` for that tile and `radar_project` put it. If the mesh
    // ignored the terrain, or read some other tile, nothing in it would match this point.
    // Two tiles left of the viewer and dead ahead of it: sampled by the default step, well inside
    // the oval, and its own height is not zero (asserted below, so a flat spot cannot pass).
    const int tile_x = static_cast<int>(kViewerTile) - 2;
    const int tile_z = static_cast<int>(kViewerTile);
    const float h = protekt::game::height_metres(kMeshSeed, tile_x, tile_z);
    const RadarPoint flat = radar_project(kPanel, -2.0F, 0.0F, kRadarRadiusTiles, 0.0F);
    const RadarPoint want = radar_project(kPanel, -2.0F, 0.0F, kRadarRadiusTiles, h);
    EXPECT_GT(std::abs(want.py - flat.py), 1.0e-3F)
        << "this corner is flat in this seed: the test would pass without any relief at all";

    bool found = false;
    for (const RadarSegment& segment :
         radar_mesh(kMeshSeed, kViewerTile, kViewerTile, kMeshYaw, kPanel)) {
        for (const RadarPoint& p : {segment.a, segment.b}) {
            if (std::abs(p.px - want.px) < 1.0e-3F && std::abs(p.py - want.py) < 1.0e-3F) {
                found = true;
            }
        }
    }
    EXPECT_TRUE(found) << "the mesh does not put tile (" << tile_x << "," << tile_z
                       << ") where the world says it is";
}

TEST(RadarMesh, ASeedChangeMovesTheWholePicture) {
    // Aggregate form of the same coupling: the ground is different ground, so the mesh's own extent
    // changes. Clipping means the two seeds need not agree on a COUNT (a hill near the rim clips a
    // segment away), so this compares the shape, which is what a frame shows.
    const std::vector<RadarSegment> first
        = radar_mesh(kMeshSeed, kViewerTile, kViewerTile, kMeshYaw, kPanel);
    const std::vector<RadarSegment> other
        = radar_mesh(kMeshSeed + 1, kViewerTile, kViewerTile, kMeshYaw, kPanel);
    ASSERT_FALSE(first.empty());
    ASSERT_FALSE(other.empty());
    const auto topmost = [](const std::vector<RadarSegment>& segments) {
        float top = 1.0e9F;
        for (const RadarSegment& s : segments) {
            top = std::min({top, s.a.py, s.b.py});
        }
        return top;
    };
    EXPECT_GT(std::abs(topmost(first) - topmost(other)), 1.0F)
        << "two seeds drew the same skyline: the mesh is not reading the terrain";
}

TEST(RadarMesh, TheReliefLiftsTheGroundUpThePanel) {
    // `radar_project`'s height term, measured rather than assumed: a metre is `kMetresPerTile`
    // tiles, so a tile's height in metres becomes `height / kMetresPerTile` tiles of offset, and
    // that offset reads up the tilted plane as `cos(elevation)` of its length. A hill therefore
    // sits HIGHER on the panel (smaller py) than flat ground at the same spot.
    const float h = 6.0F; // metres, about three tiles of hill
    const RadarPoint flat = radar_project(kPanel, 0.0F, 0.0F);
    const RadarPoint lifted = radar_project(kPanel, 0.0F, 0.0F, kRadarRadiusTiles, h);
    const float scale = pixels_per_tile(kPanel);
    const float expected
        = (h / protekt::game::kMetresPerTile) * std::cos(kRadarElevationRadians) * scale;
    EXPECT_NEAR(flat.py - lifted.py, expected, 0.01F);
    EXPECT_LT(lifted.py, flat.py) << "a hill must read above the flat plane, never below it";
}

TEST(RadarMesh, TheStepIsTheFrameCostDial) {
    // The brief's cost dial: step 1 is four times the corners of step 2. Both must draw something,
    // and the coarse mesh must be the cheaper one, or the dial does nothing.
    const std::vector<RadarSegment> fine
        = radar_mesh(kMeshSeed, kViewerTile, kViewerTile, kMeshYaw, kPanel, kRadarRadiusTiles, 1);
    const std::vector<RadarSegment> coarse
        = radar_mesh(kMeshSeed, kViewerTile, kViewerTile, kMeshYaw, kPanel, kRadarRadiusTiles,
                     kRadarMeshStepTiles);
    ASSERT_FALSE(fine.empty());
    ASSERT_FALSE(coarse.empty());
    EXPECT_GT(fine.size(), coarse.size()) << "step " << kRadarMeshStepTiles << " should cost less";
}

TEST(RadarMesh, ARadiusOfZeroDrawsNothingRatherThanEverything) {
    // A degenerate radius must be an empty mesh, not a grid over the whole world.
    EXPECT_TRUE(radar_mesh(kMeshSeed, kViewerTile, kViewerTile, kMeshYaw, kPanel, 0).empty());
    EXPECT_TRUE(
        radar_mesh(kMeshSeed, kViewerTile, kViewerTile, kMeshYaw, kPanel, kRadarRadiusTiles, 0)
            .empty());
    const RadarPanel empty{};
    EXPECT_TRUE(radar_mesh(kMeshSeed, kViewerTile, kViewerTile, kMeshYaw, empty).empty());
}

TEST(RadarMesh, TheLineWidthScalesWithTheFrame) {
    // The width constant is stated at a 720-high frame and scales from there, so the mesh does not
    // fatten on a 1080 window. This pins the constant's own value and its stated reference height.
    EXPECT_FLOAT_EQ(kRadarMeshLinePixels, 1.0F);
    EXPECT_EQ(kRadarMeshStepTiles, 2);
}

// ---------------------------------------------------------------- the reach is the driven kind's

/// The scout's whole reason to exist (VIS): its radar reaches 80 tiles, every other kind's 40.
constexpr std::uint64_t kReachSeed = 42;
constexpr int kReachX = 20; ///< seed 42: this row is clear of craters at +3, +45, +79 and +81
constexpr int kReachY = 40;

/// A sim holding exactly a player `viewer` at (kReachX, kReachY) and one enemy tank `east` tiles
/// due east of it -- or the viewer alone for `east` 0. The positions are asserted, not assumed:
/// the sim moves a spawn off a crater, and a 79 that became an 81 would test nothing.
Sim reach_sim(UnitKind viewer, int east) {
    std::vector<Spawn> spawns{{viewer, Side::Player, kReachX, kReachY}};
    if (east > 0) {
        spawns.push_back({UnitKind::Tank, Side::Enemy, kReachX + east, kReachY});
    }
    Sim sim = Sim::create(Scenario{kReachSeed, spawns});
    EXPECT_EQ(sim.units().at(0).x, Fixed::from_int(kReachX)) << "the viewer was not moved";
    EXPECT_EQ(sim.units().at(0).y, Fixed::from_int(kReachY));
    if (east > 0) {
        EXPECT_EQ(sim.units().at(1).x, Fixed::from_int(kReachX + east))
            << "the enemy was not moved";
        EXPECT_EQ(sim.units().at(1).y, Fixed::from_int(kReachY));
    }
    return sim;
}

/// The hostile blips a `viewer` sees of an enemy `east` tiles away, at the reach the game layer
/// gives the driven unit -- `viewer_radar_radius`, as the scene draws it -- and not a number typed
/// in here.
int hostile_blips(UnitKind viewer, int east) {
    const Sim sim = reach_sim(viewer, east);
    const UnitId you = sim.units().at(0).id;
    const std::vector<RadarMark> marks
        = radar_marks(sim.units(), you, 0.0F, kPanel, viewer_radar_radius(sim, you));
    return static_cast<int>(std::count_if(marks.begin(), marks.end(), [](const RadarMark& mark) {
        return mark.tone == RadarTone::Hostile;
    }));
}

TEST(RadarReach, AScoutSeesEightyTilesAndEveryOtherKindForty) {
    EXPECT_EQ(kRadarRadiusTiles, 40) << "the default every projection is calibrated against";
    EXPECT_EQ(kScoutRadarRadiusTiles, 80);
    const Sim scout = reach_sim(UnitKind::Scout, 0);
    EXPECT_EQ(viewer_radar_radius(scout, scout.units().at(0).id), kScoutRadarRadiusTiles);
    const Sim tank = reach_sim(UnitKind::Tank, 0);
    EXPECT_EQ(viewer_radar_radius(tank, tank.units().at(0).id), kRadarRadiusTiles);
    EXPECT_EQ(viewer_radar_radius(scout, UnitId{}), kRadarRadiusTiles) << "no viewer";
    EXPECT_EQ(viewer_radar_radius(scout, UnitId{99}), kRadarRadiusTiles) << "a missing viewer";
}

TEST(RadarReach, ADeadScoutsRadarFallsBackToForty) {
    // The radar stays up after death; it must not go on claiming the scout's reach.
    Sim sim = Sim::create(Scenario{kReachSeed,
                                   {{UnitKind::Scout, Side::Player, kReachX, kReachY},
                                    {UnitKind::Tank, Side::Enemy, kReachX + 3, kReachY},
                                    {UnitKind::Tank, Side::Enemy, kReachX, kReachY + 3}}});
    const UnitId scout = sim.units().at(0).id;
    ASSERT_EQ(viewer_radar_radius(sim, scout), kScoutRadarRadiusTiles);
    for (int t = 0; t < 900 && sim.find(scout)->alive; ++t) {
        sim.step();
    }
    ASSERT_FALSE(sim.find(scout)->alive) << "two tanks at point blank kill a scout";
    EXPECT_EQ(viewer_radar_radius(sim, scout), kRadarRadiusTiles);
}

TEST(RadarReach, AScoutsRadarShowsAHostileAtSeventyNineTilesAndNotAtEightyOne) {
    EXPECT_EQ(hostile_blips(UnitKind::Scout, 45), 1);
    EXPECT_EQ(hostile_blips(UnitKind::Scout, 79), 1) << "the wider view";
    EXPECT_EQ(hostile_blips(UnitKind::Scout, 81), 0) << "and still a radius, not a map";
}

TEST(RadarReach, ATankInTheSameSeatSeesNoneOfThem) {
    EXPECT_EQ(hostile_blips(UnitKind::Tank, 45), 0) << "past a tank's 40";
    EXPECT_EQ(hostile_blips(UnitKind::Tank, 79), 0);
    EXPECT_EQ(hostile_blips(UnitKind::Tank, 81), 0);
}

} // namespace
