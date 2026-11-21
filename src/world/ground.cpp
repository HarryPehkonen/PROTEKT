#include "protekt/world/ground.hpp"

#include <atomic>
#include <cstddef>
#include <utility>

namespace protekt::world {

namespace {

// The one piece of state in the world module, and why it is acceptable: it is written only by
// install/clear, before a battle exists and after it ends, and read by every sample. The default
// is "nothing installed", in which case a sample costs one relaxed load and answers as it always
// did. NOLINT: a non-const global is the point -- the sim and the renderer both ask the world by
// seed alone, and threading a ground handle through every one of those calls was the alternative.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::unique_ptr<const MapGround> g_owner;
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<const MapGround*> g_ground{nullptr};
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<std::uint64_t> g_epoch{0};

[[nodiscard]] std::size_t cell(const MapGround& g, int x, int y) noexcept {
    return (static_cast<std::size_t>(y) * static_cast<std::size_t>(g.width))
           + static_cast<std::size_t>(x);
}

[[nodiscard]] bool inside(const MapGround& g, int x, int y) noexcept {
    return x >= 0 && y >= 0 && x < g.width && y < g.height;
}

} // namespace

void install_ground(MapGround ground) {
    g_ground.store(nullptr);
    g_owner = std::make_unique<const MapGround>(std::move(ground));
    g_ground.store(g_owner.get());
    g_epoch.fetch_add(1);
}

void clear_ground() noexcept {
    g_ground.store(nullptr);
    g_owner.reset();
    g_epoch.fetch_add(1);
}

std::uint64_t ground_epoch() noexcept { return g_epoch.load(); }

bool ground_installed() noexcept { return g_ground.load() != nullptr; }

bool ground_solid(int world_x, int world_y) noexcept {
    const MapGround* g = g_ground.load(std::memory_order_relaxed);
    return g != nullptr && inside(*g, world_x, world_y)
           && g->solid[cell(*g, world_x, world_y)] != 0;
}

namespace detail {

bool ground_tile(int world_x, int world_y, Tile& tile) noexcept {
    const MapGround* g = g_ground.load(std::memory_order_relaxed);
    if (g == nullptr) {
        return false;
    }
    if (inside(*g, world_x, world_y)) {
        tile = g->tiles[cell(*g, world_x, world_y)];
    } else {
        tile = Tile{0, Terrain::Crater};
    }
    return true;
}

} // namespace detail

} // namespace protekt::world
