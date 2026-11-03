#include "protekt/game/kind_icon.hpp"

#include <initializer_list>
#include <string>
#include <vector>

#include "protekt/game/scene.hpp"
#include "protekt/game/vehicle_shape.hpp"

namespace protekt::game {

namespace {

/// The recipes are drawn in a frame whose feet rest on row 64 and whose top is row 0; the canvas
/// drops every shape by this many rows so the feet land on `kIconGround` and the icon stands on
/// the floor of its box.
constexpr std::size_t kRecipeGround = 64;
constexpr float kDrop = static_cast<float>(kIconGround - kRecipeGround);

/// THE CANVAS the recipes paint on: `kIconCells` rows of '#' (lit) and '.' (clear). Every shape is
/// sampled at the centre of each cell, in cell units from the top-left (x right, y down), and
/// either lights the cells it covers or, with `lit` false, CLEARS them -- which is how a window, a
/// road wheel's gap or a door is cut out of a silhouette. Later shapes paint over earlier ones.
class Canvas {
public:
    Canvas() {
        for (std::string& row : rows_) {
            row.assign(kIconCells, '.');
        }
    }

    /// The rectangle from (x0, y0) up to, not including, (x1, y1).
    void box(float x0, float y0, float x1, float y1, bool lit = true) {
        paint(lit, [&](float x, float y) { return x >= x0 && x < x1 && y >= y0 && y < y1; });
    }

    void disc(float cx, float cy, float radius, bool lit = true) {
        paint(lit, [&](float x, float y) {
            return ((x - cx) * (x - cx)) + ((y - cy) * (y - cy)) <= radius * radius;
        });
    }

    /// A polygon through `points` in order, any winding (even-odd), convex or not.
    void poly(std::initializer_list<std::array<float, 2>> points, bool lit = true) {
        const std::vector<std::array<float, 2>> corners(points);
        paint(lit, [&](float x, float y) {
            bool inside = false;
            for (std::size_t i = 0, j = corners.size() - 1; i < corners.size(); j = i++) {
                const auto& a = corners.at(i);
                const auto& b = corners.at(j);
                if (((a[1] > y) != (b[1] > y))
                    && (x < ((b[0] - a[0]) * (y - a[1]) / (b[1] - a[1])) + a[0])) {
                    inside = !inside;
                }
            }
            return inside;
        });
    }

    /// Lights every cell whose centre `inside` says is in the silhouette (same cell units, same
    /// drop as the shapes above): how a kind whose shape is written elsewhere puts it on the plate.
    template <typename Inside> void fill(const Inside& inside) { paint(true, inside); }

    /// Copies the left half onto the right, mirrored: for a silhouette that is its own reflection.
    void mirror_left_onto_right() {
        for (std::string& row : rows_) {
            for (std::size_t col = 0; col < kIconCells / 2; ++col) {
                row.at(kIconCells - 1 - col) = row.at(col);
            }
        }
    }

    [[nodiscard]] const std::array<std::string, kIconCells>& rows() const noexcept { return rows_; }

private:
    template <typename Inside> void paint(bool lit, const Inside& inside) {
        for (std::size_t row = 0; row < kIconCells; ++row) {
            for (std::size_t col = 0; col < kIconCells; ++col) {
                if (inside(static_cast<float>(col) + 0.5F,
                           static_cast<float>(row) + 0.5F - kDrop)) {
                    rows_.at(row).at(col) = lit ? '#' : '.';
                }
            }
        }
    }

    std::array<std::string, kIconCells> rows_{};
};

/// A finished mask and the rows it points into: built once, never moved or copied.
class Icon {
public:
    explicit Icon(void (*draw)(Canvas&)) {
        Canvas canvas;
        draw(canvas);
        rows_ = canvas.rows();
        for (std::size_t row = 0; row < kIconCells; ++row) {
            view_.at(row) = rows_.at(row);
        }
    }
    Icon(const Icon&) = delete;
    Icon& operator=(const Icon&) = delete;
    Icon(Icon&&) = delete;
    Icon& operator=(Icon&&) = delete;
    ~Icon() = default;

    [[nodiscard]] const IconMask& view() const noexcept { return view_; }

private:
    std::array<std::string, kIconCells> rows_{};
    IconMask view_{};
};

template <void (*Draw)(Canvas&)> const IconMask& art() {
    static const Icon icon{Draw};
    return icon.view();
}

constexpr float kGround = static_cast<float>(kRecipeGround)
                          + 1.0F; ///< the first row under the feet, in the recipe's frame

/// The empty cells either side of a shaped plate's silhouette.
constexpr float kShapeMargin = 1.0F;

/// A TRACK RUN, the shared part of every tracked kind: a rounded slab from `x0` to `x1` standing on
/// the ground, `height` tall, an end roller at each end and `wheels` road wheels along it, each a
/// clear ring round a lit hub so the run reads as links over wheels and not a bar.
void track(Canvas& c, float x0, float x1, float height, int wheels) {
    const float r = height * 0.5F;
    const float top = kGround - height;
    c.box(x0 + r, top, x1 - r, kGround);
    c.disc(x0 + r, top + r, r);
    c.disc(x1 - r, top + r, r);
    for (int i = 0; i < wheels; ++i) {
        const float cx
            = x0 + r
              + ((x1 - x0 - (2.0F * r)) * static_cast<float>(i) / static_cast<float>(wheels - 1));
        c.disc(cx, top + r, r * 0.62F, false);
        c.disc(cx, top + r, r * 0.28F);
    }
}

/// A WHEEL: a lit tyre round a clear ring round a lit hub.
void wheel(Canvas& c, float cx, float cy, float r) {
    c.disc(cx, cy, r);
    c.disc(cx, cy, r * 0.66F, false);
    c.disc(cx, cy, r * 0.3F);
}

// ---------------------------------------------------------------------------- the kinds

/// An assault tank: a heavier, boxier hull on a longer run, a tall slab turret and a thick barrel.
void draw_assault_tank(Canvas& c) {
    track(c, 3, 67, 18, 6);
    c.poly({{7, 46}, {11, 30}, {58, 30}, {66, 46}});
    c.box(9, 45, 63, 46, false);
    c.box(19, 14, 47, 30); // the tall turret
    c.poly({{19, 14}, {47, 14}, {51, 18}, {47, 22}, {19, 22}}, false);
    c.box(21, 15, 45, 21);
    c.box(47, 18, 71, 27); // the thick barrel
    c.box(64, 16, 69, 29); // the muzzle brake
    c.box(28, 8, 36, 14);  // a roof hatch
}

/// A rocket tank: the tank's hull with a slanted launcher box on top, three tubes open at its upper
/// end.
void draw_rocket_tank(Canvas& c) {
    track(c, 6, 64, 16, 5);
    c.poly({{10, 48}, {16, 36}, {54, 36}, {64, 48}});
    c.box(12, 47, 60, 48, false);
    c.poly({{14, 38}, {40, 12}, {58, 20}, {32, 42}});        // the launcher bank
    c.poly({{22, 36}, {40, 20}, {44, 24}, {28, 40}}, false); // a clear lane down its middle
    c.disc(46, 16, 3, false); // the muzzles, open ends of three tubes
    c.disc(52, 21, 3, false);
    c.disc(40, 11.5F, 2.5F, false);
}

/// A mortar bike: two big spoked wheels, a slim frame, a seat and handlebars, and a mortar tube
/// slanted up and forward off the back.
void draw_mortar_bike(Canvas& c) {
    wheel(c, 14, 53, 11);
    wheel(c, 58, 53, 11);
    c.poly({{14, 44}, {24, 38}, {52, 38}, {58, 46}, {52, 50}, {20, 50}}); // the frame
    c.box(22, 32, 36, 38);                                                // the seat
    c.box(10, 30, 16, 33);                                                // the handlebars
    c.box(13, 33, 16, 46);                                                // the fork
    c.poly({{34, 36}, {42, 38}, {66, 12}, {58, 8}});                      // the mortar tube
    c.poly({{56, 6}, {68, 14}, {64, 18}, {52, 10}}, false);               // the open mouth
}

/// A walker: a two-legged machine -- bent legs ending in broad feet, a squat body, a head and a
/// gun arm held out forward. Legs are what says "walker".
void draw_walker(Canvas& c) {
    c.poly({{22, 36}, {32, 36}, {30, 48}, {24, 58}, {14, 58}, {20, 48}}); // the rear leg
    c.box(8, 60, 28, kGround);                                            // its foot
    c.poly({{40, 36}, {50, 36}, {54, 48}, {60, 58}, {50, 58}, {44, 48}}); // the front leg
    c.box(44, 60, 66, kGround);                                           // its foot
    c.box(18, 18, 52, 38);                                                // the body
    c.poly({{18, 18}, {52, 18}, {48, 12}, {26, 12}});                     // the shoulders
    c.box(28, 22, 42, 28, false);                                         // the cockpit glass
    c.box(50, 24, 71, 30);                                                // the gun arm
    c.box(66, 22, 70, 32);
}

/// An APC: a long slab-sided hull with a sloped nose on four wheels, a small turret with a gun and
/// a row of firing ports.
void draw_apc(Canvas& c) {
    c.poly({{4, 54}, {4, 32}, {50, 32}, {68, 44}, {68, 54}});
    c.box(10, 36, 18, 42, false); // ports down the side
    c.box(24, 36, 32, 42, false);
    c.box(38, 36, 46, 42, false);
    c.box(20, 22, 40, 32); // the turret
    c.box(40, 25, 58, 29); // its gun
    for (const float x : {14.0F, 28.0F, 42.0F, 56.0F}) {
        wheel(c, x, 57, 7.5F);
    }
}

/// A bomber: a long fuselage with a nosecone and a canopy, a tall tail fin, a wing, and a bomb
/// slung under it -- a ball on a slab.
void draw_bomber(Canvas& c) {
    c.box(8, 30, 56, 42);
    c.poly({{56, 30}, {70, 36}, {56, 42}});           // the nose
    c.poly({{8, 30}, {4, 8}, {20, 8}, {28, 30}});     // the tail fin
    c.poly({{26, 42}, {50, 42}, {40, 56}, {20, 56}}); // the wing
    c.poly({{44, 30}, {50, 24}, {58, 30}}, false);    // the canopy
    c.box(4, 30, 10, 36, false);
    c.disc(33, 60, 4.5F);                   // the bomb
    c.poly({{26, 59}, {20, 56}, {20, 64}}); // its fins
}

/// A SHAPED KIND is NOT drawn here: its shape is `shape_of(kind)`, written once and shared with the
/// world mesh, and the plate is that shape seen side-on, fitted to the box with a one-cell margin
/// either side and its tracks on the ground line. Nothing about what the machine looks like is
/// written in these functions. `reach` is the farthest the plate must hold, in metres along: the
/// shape's own end, or more where something that is not a part sticks out past it.
float shape_scale(sim::UnitKind kind, float reach) {
    return (static_cast<float>(kIconCells) - (2.0F * kShapeMargin))
           / (reach - shape_extent(kind).along_min); // cells per metre
}

void project_shape(Canvas& c, sim::UnitKind kind, float scale) {
    const float along_min = shape_extent(kind).along_min;
    c.fill([&](float x, float y) {
        return shape_covers(kind, along_min + ((x - kShapeMargin) / scale), (kGround - y) / scale);
    });
}

/// A scavenger: its shape, projected.
void draw_scavenger(Canvas& c) {
    constexpr sim::UnitKind kKind = sim::UnitKind::Scavenger;
    project_shape(c, kKind, shape_scale(kKind, shape_extent(kKind).along_max));
}

/// A tank: its shape, projected, and then the BARREL as a separate small piece. The barrel is not a
/// part of the shape -- it elevates, and a side-on projection cannot say that -- so it is drawn
/// level, from the turret's front face to the muzzle collar's far end, in the mesh's own metres
/// (`kTankBarrelTip` and its neighbours in scene.hpp). The plate holds the barrel's tip as well as
/// the hull, so the whole machine fits the box. The turret group is drawn as authored, at zero.
void draw_tank(Canvas& c) {
    constexpr sim::UnitKind kKind = sim::UnitKind::Tank;
    const float scale = shape_scale(kKind, kTankBarrelTip);
    project_shape(c, kKind, scale);
    const float along_min = shape_extent(kKind).along_min;
    const auto col = [&](float along) { return kShapeMargin + ((along - along_min) * scale); };
    const auto row = [&](float up) { return kGround - (up * scale); };
    c.box(col(kTankTurretFront), row(kTankTurretBase + kTankBarrelTop),
          col(kTankBarrelTip - kTankCollarLength), row(kTankTurretBase + kTankBarrelBottom));
    c.box(col(kTankBarrelTip - kTankCollarLength), row(kTankTurretBase + kTankCollarTop),
          col(kTankBarrelTip), row(kTankTurretBase + kTankCollarBottom)); // the muzzle collar
}

/// A hover tank: its shape, projected, and then the SKIRT and the BARREL as separate pieces, as the
/// tank's barrel is. The skirt is the band of lift under the hull, from `kHoverSkirtBottom` up to
/// the hull's floor, which leaves the air gap under it on the plate; the barrel is drawn level
/// from the turret's front face to the tip. Both in the mesh's own metres (scene.hpp), and the
/// plate holds the barrel's tip as well as the nose.
void draw_hover_tank(Canvas& c) {
    constexpr sim::UnitKind kKind = sim::UnitKind::HoverTank;
    const float scale = shape_scale(kKind, kHoverBarrelTip);
    project_shape(c, kKind, scale);
    const float along_min = shape_extent(kKind).along_min;
    const auto col = [&](float along) { return kShapeMargin + ((along - along_min) * scale); };
    const auto row = [&](float up) { return kGround - (up * scale); };
    c.box(col(kHoverTail * kHoverSkirtInset), row(kHoverHullBottom),
          col(kHoverNose * kHoverSkirtInset), row(kHoverSkirtBottom));
    c.box(col(kHoverTurretFront), row(kHoverTurretBase + kHoverBarrelTop), col(kHoverBarrelTip),
          row(kHoverTurretBase + kHoverBarrelBottom));
}

/// A scout: its shape, projected.
void draw_scout(Canvas& c) {
    constexpr sim::UnitKind kKind = sim::UnitKind::Scout;
    project_shape(c, kKind, shape_scale(kKind, shape_extent(kKind).along_max));
}

/// The missile scout: its shape, projected.
void draw_msl_scout(Canvas& c) {
    constexpr sim::UnitKind kKind = sim::UnitKind::MslScout;
    project_shape(c, kKind, shape_scale(kKind, shape_extent(kKind).along_max));
}

/// The recycler on its tracks: a broad body on a long track run, two stacks, a cab at the front
/// with a window, and a bay door between them.
void draw_recycler(Canvas& c) {
    track(c, 2, 70, 16, 7);
    c.box(6, 28, 62, 48);
    c.box(12, 12, 20, 28); // the stacks
    c.box(26, 12, 34, 28);
    c.poly({{44, 28}, {48, 16}, {68, 16}, {68, 48}, {44, 48}}); // the cab
    c.box(50, 20, 64, 28, false);
    c.box(30, 34, 42, 48, false); // the door
}

/// The deployed recycler: a building -- a central tower with an antenna and a dish, a stack either
/// side, and a block of two bay doors under it. Nothing wheeled survives in it.
void draw_deployed_recycler(Canvas& c) {
    c.box(4, 38, 68, kGround);
    c.box(28, 12, 44, 38); // the tower
    c.box(34, 3, 38, 12);  // the antenna
    c.disc(36, 5, 3.5F);   // its dish
    c.box(8, 20, 18, 38);  // the stacks
    c.box(54, 20, 64, 38);
    c.box(33, 18, 39, 26, false);      // the tower's window
    c.box(10, 46, 30, kGround, false); // the bay doors
    c.box(42, 46, 62, kGround, false);
    c.box(10, 50, 30, 51); // door slats
    c.box(42, 50, 62, 51);
}

/// A factory: a chimney, a sawtooth roof over a long shed with a row of windows and a wide door.
void draw_factory(Canvas& c) {
    c.box(6, 6, 16, 36); // the chimney
    c.box(5, 4, 17, 9);
    c.box(4, 30, 68, kGround);
    c.poly({{20, 30}, {20, 18}, {32, 30}}); // three saw teeth
    c.poly({{32, 30}, {32, 18}, {44, 30}});
    c.poly({{44, 30}, {44, 18}, {56, 30}});
    for (const float x : {10.0F, 22.0F, 34.0F, 46.0F, 58.0F}) {
        c.box(x, 36, x + 6, 42, false);
    }
    c.box(22, 48, 50, kGround, false); // the wide door
}

/// The armory, Harri's own description: "the one with a missile on each side of it". A bunker with
/// a stepped roof and a door, a pointed missile standing upright either side. Drawn left, mirrored.
void draw_armory(Canvas& c) {
    c.box(18, 30, 54, kGround); // the bunker
    c.box(24, 20, 48, 30);      // its stepped roof
    c.box(30, 46, 42, kGround, false);
    c.box(26, 24, 31, 28, false);
    c.box(41, 24, 46, 28, false);
    c.box(8, 22, 14, kGround);                     // the left missile's body
    c.poly({{8, 22}, {11, 8}, {14, 22}});          // its nose
    c.poly({{8, 50}, {3, kGround}, {8, kGround}}); // its fin
    c.mirror_left_onto_right();
}

/// A defence turret: a dome on a wide flared base with a gun out of it and no wheels or tracks at
/// all.
void draw_turret(Canvas& c) {
    c.disc(34, 52, 20);
    c.box(0, 52, 72, 72, false);
    c.poly({{4, kGround}, {10, 50}, {58, 50}, {64, kGround}}); // the base
    c.box(44, 32, 71, 39);                                     // the gun
    c.box(64, 30, 69, 41);                                     // its muzzle brake
    c.box(18, 40, 30, 46, false);                              // a vision slit across the dome
    c.disc(34, 32, 3);                                         // an antenna knob on top
}

/// The deliberate "no art for this kind" mark: a hollow box with a dot in it.
void draw_fallback(Canvas& c) {
    c.box(14, 14, 58, 58);
    c.box(18, 18, 54, 54, false);
    c.disc(36, 36, 5);
}

} // namespace

const IconMask& kind_icon(sim::UnitKind kind) noexcept {
    switch (kind) {
    case sim::UnitKind::Tank:
        return art<draw_tank>();
    case sim::UnitKind::Turret:
    case sim::UnitKind::DeployedTurret: // the same plate: the icon is the turret's either way
        return art<draw_turret>();
    case sim::UnitKind::HoverTank:
        return art<draw_hover_tank>();
    case sim::UnitKind::MortarBike:
        return art<draw_mortar_bike>();
    case sim::UnitKind::RocketTank:
        return art<draw_rocket_tank>();
    case sim::UnitKind::Scavenger:
    case sim::UnitKind::DeployedScavenger:
        return art<draw_scavenger>();
    case sim::UnitKind::Recycler:
        return art<draw_recycler>();
    case sim::UnitKind::Scout:
        return art<draw_scout>();
    case sim::UnitKind::AssaultTank:
        return art<draw_assault_tank>();
    case sim::UnitKind::Walker:
        return art<draw_walker>();
    case sim::UnitKind::Apc:
        return art<draw_apc>();
    case sim::UnitKind::Bomber:
        return art<draw_bomber>();
    case sim::UnitKind::MslScout:
        return art<draw_msl_scout>();
    case sim::UnitKind::Factory:
        return art<draw_factory>();
    case sim::UnitKind::DeployedRecycler:
        return art<draw_deployed_recycler>();
    }
    return art<draw_fallback>();
}

const IconMask& armory_icon() noexcept { return art<draw_armory>(); }

const IconMask& fallback_icon() noexcept { return art<draw_fallback>(); }

} // namespace protekt::game
