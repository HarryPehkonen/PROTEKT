#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "protekt/game/scene_data.hpp"
#include "protekt/sim/sim.hpp"

/// The renderer, as a client of the simulation.
///
/// This header is the WHOLE contract between the game and the GPU. Nothing below it may leak a
/// Vulkan type: the sim must stay buildable, testable and reviewable on a machine with no Vulkan
/// driver at all, and another backend must be addable later without touching a caller.
/// `src/render/*.cpp` is the only place in this project that includes <vulkan/...>.
namespace protekt::render {

/// One physical device, described in terms a game cares about rather than Vulkan enums.
struct DeviceInfo {
    std::string name;
    std::uint32_t vendor_id{};
    std::uint32_t device_id{};
    /// Packed as Vulkan packs it: major << 22 | minor << 12 | patch.
    std::uint32_t api_version{};
    bool discrete{};
    bool integrated{};
    bool cpu{};
};

/// What to draw. With `scene == nullptr` the renderer draws the built-in test scene M3a shipped
/// (one ground plane and one unit box, coloured from `seed`); with a scene, that scene.
struct FrameRequest {
    game::Extent extent{};
    std::uint64_t seed{};
    sim::Tick tick{};
    const game::Scene* scene{};
};

struct FrameStats {
    std::uint32_t draw_calls{};
    std::uint32_t triangles{};
};

class Renderer {
public:
    Renderer() = default;
    virtual ~Renderer() = default;
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer(Renderer&&) = delete;
    Renderer& operator=(Renderer&&) = delete;

    [[nodiscard]] virtual game::Extent extent() const noexcept = 0;
    [[nodiscard]] virtual std::string_view device_name() const noexcept = 0;

    /// Renders one frame and returns RGBA8, row-major, with the top-left texel first: exactly
    /// extent().width * extent().height * 4 bytes.
    ///
    /// DETERMINISM, STATED HONESTLY: two calls with the same request on the same device and
    /// driver return identical bytes. A different device may legitimately differ by a texel on
    /// the same geometry, so an exact image hash is only meaningful on one pinned device. The
    /// suite therefore asserts structure on any device and byte equality only against a second
    /// call, never against a checked-in golden.
    virtual std::vector<std::uint8_t> render(const FrameRequest& request, FrameStats& stats) = 0;

    /// How many findings the VALIDATION LAYER has raised about this renderer's use of the API
    /// since creation -- findings with a "VUID-" id, and only those. A test asserts this stays
    /// zero: it is the cheapest correctness oracle a Vulkan renderer can have.
    ///
    /// Deliberately not a count of everything the debug messenger sees. The loader also reports
    /// ICDs it cannot load and checks the extension names in a VkDeviceCreateInfo it is about to
    /// pass on, where an implicit layer may have added an extension this code never requested --
    /// real news about the machine, and not this renderer's business. Those are printed and not
    /// counted, so a broken driver stack cannot fail a test that is about this code.
    [[nodiscard]] virtual std::uint32_t validation_errors() const noexcept = 0;
};

struct CreateOptions {
    /// Empty selects automatically: a discrete GPU if there is one, else integrated, else CPU.
    /// On a laptop with NVIDIA Optimus this matters -- the discrete GPU is NOT device 0.
    std::string device_name;
    bool validation{true};
    game::Extent extent{320, 200};
    /// The scene texture: tightly packed RGBA8, `atlas_side` x `atlas_side`. Empty (or a size that
    /// does not match) keeps the renderer's own 2x2 white texture, under which every UV is an
    /// identity. The renderer does not know what the tiles are; the game hands it pixels.
    std::vector<std::uint8_t> atlas_rgba;
    std::uint32_t atlas_side{0};
};

/// Every physical device that can be enumerated. Empty when there is no loader or no driver;
/// `error` then says why. Headless needs no surface, so any renderable device qualifies.
std::vector<DeviceInfo> devices(std::string& error);

/// Returns nullptr on failure and puts a human-readable reason in `error`. Never throws: a
/// missing driver is a normal condition in this project, not an exceptional one.
std::unique_ptr<Renderer> create(const CreateOptions& options, std::string& error);

} // namespace protekt::render
