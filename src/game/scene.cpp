#include "protekt/game/scene.hpp"

#include "protekt/game/pick.hpp"
#include "protekt/game/radar.hpp"
#include "protekt/math/math.hpp"

#include <algorithm>

#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace protekt::game {

namespace {

using math::add;
using math::cross;
using math::dot;
using math::kPi;
using math::look_at;
using math::Mat4;
using math::multiply;
using math::normalise;
using math::perspective;
using math::Vec3;

Vec3 terrain_colour(world::Terrain terrain) {
    switch (terrain) {
    case world::Terrain::Flat:
        return {0.25F, 0.55F, 0.20F};
    case world::Terrain::Rough:
        return {0.45F, 0.42F, 0.18F};
    case world::Terrain::Ridge:
        return {0.55F, 0.50F, 0.48F};
    case world::Terrain::Crater:
        return {0.15F, 0.12F, 0.10F};
    }
    return {0.0F, 0.0F, 0.0F};
}

Vec3 side_colour(sim::Side side) {
    return side == sim::Side::Player ? Vec3{0.15F, 0.35F, 0.85F} : Vec3{0.85F, 0.15F, 0.15F};
}

void emit_triangle(std::vector<render::SceneVertex>& out, const Vec3& a, const Vec3& b,
                   const Vec3& c, const Vec3& colour) {
    out.push_back(render::SceneVertex{a, colour});
    out.push_back(render::SceneVertex{b, colour});
    out.push_back(render::SceneVertex{c, colour});
}

void emit_quad(std::vector<render::SceneVertex>& out, const Vec3& a, const Vec3& b, const Vec3& c,
               const Vec3& d, const Vec3& colour) {
    emit_triangle(out, a, b, c, colour);
    emit_triangle(out, a, c, d, colour);
}

void build_terrain(std::vector<render::SceneVertex>& out, std::uint64_t seed, int centre_x,
                   int centre_y, int radius) {
    // Each tile's corners are sampled one tile beyond it on the far edge (tx+1, ty+1); widening
    // the low edge by one tile too keeps the sampled HEIGHT field symmetric around the camera,
    // even though the tolerance on the mesh's visual extent absorbs the extra row.
    for (int ty = centre_y - radius - 1; ty <= centre_y + radius; ++ty) {
        for (int tx = centre_x - radius - 1; tx <= centre_x + radius; ++tx) {
            const Vec3 colour = terrain_colour(world::sample(seed, tx, ty).terrain);
            const float x0 = static_cast<float>(tx) * kMetresPerTile;
            const float x1 = static_cast<float>(tx + 1) * kMetresPerTile;
            const float z0 = static_cast<float>(ty) * kMetresPerTile;
            const float z1 = static_cast<float>(ty + 1) * kMetresPerTile;
            const Vec3 p00{x0, height_metres(seed, tx, ty), z0};
            const Vec3 p10{x1, height_metres(seed, tx + 1, ty), z0};
            const Vec3 p11{x1, height_metres(seed, tx + 1, ty + 1), z1};
            const Vec3 p01{x0, height_metres(seed, tx, ty + 1), z1};
            emit_quad(out, p00, p10, p11, p01, colour);
        }
    }
}

/// A box standing on the ground beneath (x, z): wider and shorter for a tank or turret, taller
/// for a factory.
/// A selected unit keeps its side's colour and is mixed towards white, so a glance tells you both
/// whose it is and that it is yours to move.
Vec3 highlight(const Vec3& colour) {
    constexpr float kMix = 0.55F;
    return Vec3{colour[0] + ((1.0F - colour[0]) * kMix), colour[1] + ((1.0F - colour[1]) * kMix),
                colour[2] + ((1.0F - colour[2]) * kMix)};
}

/// The unit under the crosshair is mixed toward a warning colour, so the player can see what a
/// click there would act on -- the reticle says it in colour, this says it on the unit itself.
/// Own and hostile are told apart: amber for yours ("do not shoot this"), a hot salmon-red for a
/// hostile (the reticle's red family, lifted in green and blue because the enemy's base colour is
/// already a dark red and a plain red mix would barely move it). Neither is a white mix, so neither
/// can be mistaken for `highlight`; the mix is strong (80%) so each reads apart from the unmarked
/// side colour at a glance.
Vec3 aim_tint(const Vec3& colour, bool own) {
    constexpr float kMix = 0.8F;
    constexpr Vec3 kOwnWarning{1.0F, 0.70F, 0.10F};
    constexpr Vec3 kHostileWarning{1.0F, 0.45F, 0.35F};
    const Vec3& warning = own ? kOwnWarning : kHostileWarning;
    return Vec3{colour[0] + ((warning[0] - colour[0]) * kMix),
                colour[1] + ((warning[1] - colour[1]) * kMix),
                colour[2] + ((warning[2] - colour[2]) * kMix)};
}

/// How a unit is marked this frame. Precedence: SELECTED wins over AIMED, because selection is the
/// state the player set on purpose and must never be masked; the aim cue only decorates a unit
/// that is not selected.
enum class Mark : std::uint8_t { None, Selected, AimedOwn, AimedHostile };

void build_unit_box(std::vector<render::SceneVertex>& out, std::uint64_t seed,
                    const sim::Unit& unit, Mark mark, std::optional<float> hull_z) {
    const float x
        = static_cast<float>(unit.x.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float z
        = static_cast<float>(unit.y.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float ground = hull_z.value_or(ground_metres(seed, unit.x, unit.y));
    const float half_width = kMetresPerTile * 0.3F;
    const float height = unit.kind == sim::UnitKind::Factory ? 4.5F : 1.8F;
    Vec3 colour = side_colour(unit.side);
    if (mark == Mark::Selected) {
        colour = highlight(colour);
    } else if (mark != Mark::None) {
        colour = aim_tint(colour, mark == Mark::AimedOwn);
    }

    const float x0 = x - half_width;
    const float x1 = x + half_width;
    const float z0 = z - half_width;
    const float z1 = z + half_width;
    const float y0 = ground;
    const float y1 = ground + height;
    const std::array<Vec3, 8> p{{{x0, y0, z0},
                                 {x1, y0, z0},
                                 {x1, y1, z0},
                                 {x0, y1, z0},
                                 {x0, y0, z1},
                                 {x1, y0, z1},
                                 {x1, y1, z1},
                                 {x0, y1, z1}}};
    emit_quad(out, p[0], p[1], p[2], p[3], colour); // near
    emit_quad(out, p[5], p[4], p[7], p[6], colour); // far
    emit_quad(out, p[4], p[0], p[3], p[7], colour); // left
    emit_quad(out, p[1], p[5], p[6], p[2], colour); // right
    emit_quad(out, p[3], p[2], p[6], p[7], colour); // top
    emit_quad(out, p[4], p[5], p[1], p[0], colour); // bottom
}

/// The reticle's distance from the eye, in metres. This close, nothing can stand between the eye
/// and the mark: it is a screen mark, and a crater rim must not be able to hide it.
constexpr float kReticleDistanceMetres = 1.5F;
/// Its half-size as a fraction of the half-screen at that distance, so it looks the same at any
/// extent.
constexpr float kReticleHalfFraction = 0.055F;
/// How thick each bar is, as a fraction of the half-size.
constexpr float kTriangleBaseFraction = 0.85F;
constexpr float kTriangleTurn = 2.35619449F; ///< 135 degrees: where a triangle's base corners sit
constexpr float kReticleBarFraction = 0.22F;

/// The reticle's colour is the ANSWER to "what would a click here do": red fires, green selects,
/// white is ground. Text is not drawn anywhere in this project yet, so colour is the only channel
/// there is.
std::array<float, 3> reticle_colour(const sim::Sim& sim, const Pick& picked, sim::Side viewer) {
    constexpr std::array<float, 3> kGround{0.92F, 0.92F, 0.92F};
    constexpr std::array<float, 3> kHostile{1.0F, 0.25F, 0.25F};
    constexpr std::array<float, 3> kOwn{0.45F, 1.0F, 0.45F};
    if (!picked.has_unit()) {
        return kGround;
    }
    const sim::Unit* unit = sim.find(picked.unit);
    if (unit == nullptr) {
        return kGround;
    }
    return unit->side == viewer ? kOwn : kHostile;
}

/// How far the radar is drawn from the eye, PER LAYER, derived from the frame rather than fixed.
///
/// Two measured traps sit behind these numbers, and both were found by looking at frames:
///
/// 1. `ray_point` puts a mark `d` metres FORWARD only for the pixel the ray runs straight through.
///    An off-centre pixel sits further along its own ray, so its forward distance is `d / |dir|`,
///    and |dir| reaches about 1.55 at a 1280x720 frame corner and grows with the aspect ratio. A
///    panel at 0.105 m was therefore only 0.07 m in front of the eye -- inside the 0.1 m near
///    plane, where it is silently clipped and simply is not there. The base distance scales with
///    the frame's own corner ray, so no resolution can push the panel into the near plane.
///
/// 2. The depth test decides between the layers per PIXEL, and depth is `1 - near/forward`, so a
///    LARGE quad interpolates its depth across its own corners: |dir| varies by about a third
///    across a corner panel, and a face 3% further out than the marks still reads NEARER than them
///    in the middle of the panel -- the marks then lose every pixel and the panel is an empty dark
///    square. The ratios below are therefore 1 : 1.6 : 2.2, wider than that variation, so the layer
///    that should be on top is on top everywhere.
///
/// The panel is still only tens of centimetres from the eye -- nearer than any ground the tank can
/// be standing on -- so terrain cannot hide it either.
constexpr float kHudClearance = 1.25F;  ///< how far past the near plane the nearest layer sits
constexpr float kHudFaceRatio = 1.6F;   ///< the panel's face, relative to the marks
constexpr float kHudBorderRatio = 2.2F; ///< and its border, relative to the marks

/// The length of the ray the frame's own corner looks along, unnormalised, exactly as `ray_through`
/// builds it: 1 at the middle of the frame, up to ~1.55 at a corner.
[[nodiscard]] float corner_ray_length(render::Extent extent) noexcept {
    const float half_tan = std::tan(math::kFovRadians * 0.5F);
    const float height = extent.height > 0 ? static_cast<float>(extent.height) : 1.0F;
    const float rx = (static_cast<float>(extent.width) / height) * half_tan;
    return std::sqrt(1.0F + (rx * rx) + (half_tan * half_tan));
}
constexpr float kRadarOutlineFraction = 0.5F; ///< an outline's bar thickness, of its half-size

std::array<float, 3> radar_colour(RadarTone tone) {
    switch (tone) {
    case RadarTone::You:
        return {0.55F, 1.0F, 0.55F};
    case RadarTone::Yours:
        return {0.30F, 0.85F, 0.35F};
    case RadarTone::Hostile:
        return {1.0F, 0.30F, 0.25F};
    }
    return {1.0F, 1.0F, 1.0F};
}

constexpr int kRadarSegments = 48;                ///< slices in the disc and the ring
constexpr float kHealthBarHeightFraction = 0.08F; ///< the bar's height, of the panel's size
constexpr float kHealthBarMinPixels = 8.0F;
constexpr float kHealthBarGapPixels = 4.0F; ///< between the bar and the panel's border

/// Green from half health up, amber from a fifth, red below -- in integers, so the thresholds are
/// exact. Green and red are the radar's own "yours" and "hostile" colours.
std::array<float, 3> health_colour(std::uint32_t hp, std::uint32_t max_hp) {
    if (hp * 2 >= max_hp) {
        return radar_colour(RadarTone::Yours);
    }
    if (hp * 5 >= max_hp) {
        return {1.0F, 0.75F, 0.20F};
    }
    return radar_colour(RadarTone::Hostile);
}

/// The radar, drawn in PANEL PIXELS: every corner goes through `ray_point`, the same pixel-to-world
/// function the picker uses, so the panel is exactly where its pixels are and a mark can never sit
/// somewhere other than its own place on the panel. Three metres of nothing, a dark face so the
/// marks read over any terrain, and then the marks: dots for units, an outline for your base, a
/// triangle for you, an arrow at the rim for a base you cannot see.
void append_radar(std::vector<render::SceneVertex>& out, const SceneOptions& options,
                  const sim::Sim& sim) {
    if (!options.viewer_unit.valid()) {
        return;
    }
    const RadarPanel panel = radar_panel(options.extent);
    const std::vector<RadarMark> marks
        = radar_marks(sim.units(), options.viewer_unit, options.camera.yaw, panel);
    if (marks.empty()) {
        return; // no such unit, or a dead one: there is nothing for the radar to be centred on
    }
    const float marks_at = math::kNearPlane * kHudClearance * corner_ray_length(options.extent);
    const float face_at = marks_at * kHudFaceRatio;
    const float border_at = marks_at * kHudBorderRatio;
    const auto at = [&](float distance, float px, float py) {
        const RayPoint point = ray_point(options.camera, options.extent, px, py, distance);
        return Vec3{point.x, point.y, point.z};
    };
    const auto rect = [&](float distance, float ax, float ay, float bx, float by,
                          const std::array<float, 3>& rgb) {
        emit_quad(out, at(distance, ax, ay), at(distance, bx, ay), at(distance, bx, by),
                  at(distance, ax, by), rgb);
    };
    const float x0 = panel.x - kRadarBorderPixels;
    const float y0 = panel.y - kRadarBorderPixels;
    const float x1 = panel.x + panel.size + kRadarBorderPixels;
    constexpr std::array<float, 3> kBorder{0.80F, 0.82F, 0.85F};
    constexpr std::array<float, 3> kFace{0.04F, 0.05F, 0.06F};

    // The panel is ROUND: a disc inscribed in its square, ringed by the border. The corners of the
    // square are not painted. Both are fans and strips of `kRadarSegments` slices, walked clockwise
    // on the screen like `rect`, and every point goes through `at` like everything else.
    const float centre_x = panel.x + (panel.size * 0.5F);
    const float centre_y = panel.y + (panel.size * 0.5F);
    const float inner = panel.size * 0.5F;
    const float outer = inner + kRadarBorderPixels;
    const auto rim = [&](float radius, int slice) {
        const float angle = (2.0F * kPi * static_cast<float>(slice)) / kRadarSegments;
        return std::array<float, 2>{centre_x + (radius * std::cos(angle)),
                                    centre_y + (radius * std::sin(angle))};
    };
    for (int slice = 0; slice < kRadarSegments; ++slice) {
        const std::array<float, 2> in_a = rim(inner, slice);
        const std::array<float, 2> in_b = rim(inner, slice + 1);
        const std::array<float, 2> out_a = rim(outer, slice);
        const std::array<float, 2> out_b = rim(outer, slice + 1);
        emit_quad(out, at(border_at, out_a.at(0), out_a.at(1)),
                  at(border_at, out_b.at(0), out_b.at(1)), at(border_at, in_b.at(0), in_b.at(1)),
                  at(border_at, in_a.at(0), in_a.at(1)), kBorder);
    }
    for (int slice = 0; slice < kRadarSegments; ++slice) {
        const std::array<float, 2> a = rim(inner, slice);
        const std::array<float, 2> b = rim(inner, slice + 1);
        emit_triangle(out, at(face_at, centre_x, centre_y), at(face_at, a.at(0), a.at(1)),
                      at(face_at, b.at(0), b.at(1)), kFace);
    }

    // The viewer's hull as a bar directly above the panel: the same width as the panel with its
    // border, a small gap up. The sim owns the numbers; nothing is added to `Unit` for this.
    const sim::Unit* viewer = sim.find(options.viewer_unit);
    if (viewer != nullptr && viewer->alive) {
        const float height = std::max(kHealthBarMinPixels, panel.size * kHealthBarHeightFraction);
        const float bottom = y0 - kHealthBarGapPixels;
        const float top = bottom - height;
        rect(face_at, x0, top, x1, bottom, kFace);
        const std::uint32_t max_hp = sim::profile(viewer->kind).hp;
        const std::uint32_t hp = std::min<std::uint32_t>(viewer->hp, max_hp);
        if (max_hp > 0 && hp > 0) {
            const float fill
                = x0 + ((x1 - x0) * static_cast<float>(hp) / static_cast<float>(max_hp));
            rect(marks_at, x0, top, fill, bottom, health_colour(hp, max_hp));
        }
    }

    for (const RadarMark& mark : marks) {
        const std::array<float, 3> rgb = radar_colour(mark.tone);
        // The mark's own coordinates are PANEL pixels; the frame is what gets drawn into, so the
        // panel's corner is added once, here, and nowhere else. Forgetting it puts every unit at
        // the top-left corner of the screen.
        const float centre_x = panel.x + mark.px;
        const float centre_y = panel.y + mark.py;
        const float left = centre_x - mark.half;
        const float right = centre_x + mark.half;
        const float top = centre_y - mark.half;
        const float bottom = centre_y + mark.half;
        if (mark.shape == RadarShape::Square) {
            rect(marks_at, left, top, right, bottom, rgb);
            continue;
        }
        if (mark.shape == RadarShape::Hollow) {
            const float bar = mark.half * kRadarOutlineFraction;
            rect(marks_at, left, top, right, top + bar, rgb);
            rect(marks_at, left, bottom - bar, right, bottom, rgb);
            rect(marks_at, left, top + bar, left + bar, bottom - bar, rgb);
            rect(marks_at, right - bar, top + bar, right, bottom - bar, rgb);
            continue;
        }
        // A triangle pointing along its angle: the apex, and two corners 135 degrees either side.
        const auto corner = [&](float radius, float turn) {
            const float angle = mark.angle + turn;
            return std::array<float, 2>{centre_x + (radius * std::sin(angle)),
                                        centre_y - (radius * std::cos(angle))};
        };
        const std::array<float, 2> apex = corner(mark.half, 0.0F);
        const std::array<float, 2> side_a
            = corner(mark.half * kTriangleBaseFraction, kTriangleTurn);
        const std::array<float, 2> side_b
            = corner(mark.half * kTriangleBaseFraction, -kTriangleTurn);
        out.push_back(render::SceneVertex{at(marks_at, apex.at(0), apex.at(1)), rgb});
        out.push_back(render::SceneVertex{at(marks_at, side_a.at(0), side_a.at(1)), rgb});
        out.push_back(render::SceneVertex{at(marks_at, side_b.at(0), side_b.at(1)), rgb});
    }
}

/// Four bars forming a square outline, in the plane the camera looks along, centred on the point
/// the click's own ray reaches. Six vertices per bar: two triangles, and no winding to get wrong,
/// because the pipeline culls nothing.
void append_reticle(std::vector<render::SceneVertex>& out, const SceneOptions& options,
                    const sim::Sim& sim, const Pick& picked) {
    if (!options.aim.has_value()) {
        return;
    }
    const Camera& camera = options.camera;
    const float screen_x = options.aim->x;
    const float screen_y = options.aim->y;
    const std::array<float, 3> colour = reticle_colour(sim, picked, options.viewer_side);
    const RayPoint centre
        = ray_point(camera, options.extent, screen_x, screen_y, kReticleDistanceMetres);
    const math::Basis basis = math::basis_from(camera.yaw, camera.pitch);
    const float half
        = kReticleDistanceMetres * std::tan(math::kFovRadians * 0.5F) * kReticleHalfFraction;
    const float bar = half * kReticleBarFraction;
    const auto corner = [&](float right, float up) {
        return std::array<float, 3>{centre.x + (right * basis.right.at(0)) + (up * basis.up.at(0)),
                                    centre.y + (right * basis.right.at(1)) + (up * basis.up.at(1)),
                                    centre.z + (right * basis.right.at(2)) + (up * basis.up.at(2))};
    };
    const auto bar_quad = [&](float r0, float u0, float r1, float u1) {
        const std::array<float, 3> a = corner(r0, u0);
        const std::array<float, 3> b = corner(r1, u0);
        const std::array<float, 3> c = corner(r1, u1);
        const std::array<float, 3> d = corner(r0, u1);
        out.push_back(render::SceneVertex{a, colour});
        out.push_back(render::SceneVertex{b, colour});
        out.push_back(render::SceneVertex{c, colour});
        out.push_back(render::SceneVertex{a, colour});
        out.push_back(render::SceneVertex{c, colour});
        out.push_back(render::SceneVertex{d, colour});
    };
    bar_quad(-half, half - bar, half, half);               // top
    bar_quad(-half, -half, half, -half + bar);             // bottom
    bar_quad(-half, -half + bar, -half + bar, half - bar); // left
    bar_quad(half - bar, -half + bar, half, half - bar);   // right
}

} // namespace

float height_metres(std::uint64_t seed, int tile_x, int tile_y) noexcept {
    return static_cast<float>(world::sample(seed, tile_x, tile_y).height) * kMetresPerHeightUnit;
}

namespace {

float blend_corners(std::uint64_t seed, int tx, int ty, float fx, float fy) noexcept {
    const float h00 = height_metres(seed, tx, ty);
    const float h10 = height_metres(seed, tx + 1, ty);
    const float h01 = height_metres(seed, tx, ty + 1);
    const float h11 = height_metres(seed, tx + 1, ty + 1);
    const float along_x0 = h00 + ((h10 - h00) * fx);
    const float along_x1 = h01 + ((h11 - h01) * fx);
    return along_x0 + ((along_x1 - along_x0) * fy);
}

} // namespace

float ground_metres(std::uint64_t seed, sim::Fixed x, sim::Fixed y) noexcept {
    constexpr float kOne = static_cast<float>(sim::Fixed::kOne);
    const std::int32_t mask = sim::Fixed::kOne - 1;
    return blend_corners(seed, x.floor_int(), y.floor_int(),
                         static_cast<float>(x.raw & mask) / kOne,
                         static_cast<float>(y.raw & mask) / kOne);
}

float ground_metres(std::uint64_t seed, float tile_x, float tile_y) noexcept {
    const float fx = std::floor(tile_x);
    const float fy = std::floor(tile_y);
    return blend_corners(seed, static_cast<int>(fx), static_cast<int>(fy), tile_x - fx,
                         tile_y - fy);
}

Suspension suspend(Suspension state, float target_z, std::uint32_t ticks) noexcept {
    for (std::uint32_t tick = 0; tick < ticks; ++tick) {
        state.z_vel += ((target_z - state.z) * kSpringPerTick) - (state.z_vel * kDampPerTick);
        state.z += state.z_vel;
    }
    return state;
}

void Hover::advance(sim::Tick tick, float ground_metres, bool boost) noexcept {
    const float rest = ground_metres + kRideHeightMetres;
    const float target = rest + (boost ? kBoostLiftMetres : 0.0F);
    if (!clocked_) {
        // At rest at ride height even if boost is already held: it rises from there, not from
        // nowhere.
        state_ = Suspension{rest, 0.0F};
        clocked_ = true;
    } else {
        const sim::Tick elapsed = tick > last_tick_ ? tick - last_tick_ : 0;
        state_ = suspend(state_, target, static_cast<std::uint32_t>(elapsed));
    }
    last_tick_ = tick;
}

std::optional<float> Hover::z() const noexcept {
    return clocked_ ? std::optional<float>{state_.z} : std::nullopt;
}

float Camera::wrap_angle(float radians) noexcept {
    float wrapped = std::fmod(radians + kPi, 2.0F * kPi);
    if (wrapped < 0.0F) {
        wrapped += 2.0F * kPi;
    }
    return wrapped - kPi;
}

std::array<float, 16> view_projection(const Camera& camera, float aspect,
                                      float fov_radians) noexcept {
    return math::view_projection(Vec3{camera.x, camera.y, camera.z}, camera.yaw, camera.pitch,
                                 aspect, fov_radians);
}

render::Scene build_scene(const sim::Sim& sim, const SceneOptions& options,
                          std::span<const sim::UnitId> selected) {
    render::Scene scene;
    const float aspect
        = static_cast<float>(options.extent.width) / static_cast<float>(options.extent.height);
    scene.view_projection = view_projection(options.camera, aspect);

    const int centre_x = static_cast<int>(std::floor(options.camera.x / kMetresPerTile));
    const int centre_y = static_cast<int>(std::floor(options.camera.z / kMetresPerTile));
    build_terrain(scene.vertices, options.seed, centre_x, centre_y, options.radius_tiles);

    // The one pick the frame makes: the reticle and the unit tint both read it, so they can never
    // disagree about what is under the crosshair.
    Pick aimed;
    if (options.aim.has_value()) {
        aimed = pick(sim, options, options.aim->x, options.aim->y);
    }

    for (const sim::Unit& unit : sim.units()) {
        if (!unit.alive) {
            continue;
        }
        // Only draw units within the terrain that was actually meshed: a unit far outside the
        // rendered radius has no ground to stand on in this scene.
        const int dx = unit.x.floor_int() - centre_x;
        const int dy = unit.y.floor_int() - centre_y;
        if (std::abs(dx) <= options.radius_tiles && std::abs(dy) <= options.radius_tiles) {
            const bool is_selected
                = std::find(selected.begin(), selected.end(), unit.id) != selected.end();
            Mark mark = Mark::None;
            if (is_selected) {
                mark = Mark::Selected;
            } else if (aimed.has_unit() && aimed.unit == unit.id) {
                mark = unit.side == options.viewer_side ? Mark::AimedOwn : Mark::AimedHostile;
            }
            const bool hovering = options.hover.has_value() && options.hover->unit == unit.id;
            build_unit_box(scene.vertices, options.seed, unit, mark,
                           hovering ? std::optional<float>{options.hover->z} : std::nullopt);
        }
    }

    append_radar(scene.vertices, options, sim);
    append_reticle(scene.vertices, options, sim, aimed);

    return scene;
}

render::Scene build_calibration_scene(const SceneOptions& options) {
    render::Scene scene;
    const float aspect
        = static_cast<float>(options.extent.width) / static_cast<float>(options.extent.height);
    scene.view_projection = view_projection(options.camera, aspect);

    const float distance = math::kNearPlane * kHudClearance * corner_ray_length(options.extent);
    const float width = static_cast<float>(options.extent.width);
    const float height = static_cast<float>(options.extent.height);
    const auto at = [&](float px, float py) {
        const RayPoint point = ray_point(options.camera, options.extent, px, py, distance);
        return Vec3{point.x, point.y, point.z};
    };
    constexpr std::array<std::array<float, 3>, 3> kBars{
        {{1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}}};
    for (std::size_t i = 0; i < kBars.size(); ++i) {
        const float x0 = width * static_cast<float>(i) / 3.0F;
        const float x1 = width * static_cast<float>(i + 1) / 3.0F;
        emit_quad(scene.vertices, at(x0, 0.0F), at(x1, 0.0F), at(x1, height), at(x0, height),
                  kBars.at(i));
    }
    return scene;
}

} // namespace protekt::game
