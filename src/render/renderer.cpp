// The only translation unit in the project that includes <vulkan/...>.
//
// Headless: a scene colour image and a depth image, three render passes, and a copy of the result
// into a host-visible buffer. No window, no surface, no swapchain.
//
// THE THREE PASSES. (1) The WORLD is drawn into an offscreen colour image with the depth buffer.
// (2) One fullscreen pass reads that image and writes an FXAA-smoothed copy (post.frag). (3) The
// HUD is drawn on top of the smoothed copy, against the world's depth buffer reloaded, so a panel,
// glyph, dot or reticle is never softened and depth between the HUD and the world behaves exactly
// as it did when both were one draw. The boundary is `Scene::world_vertex_count`.
#include "protekt/render/renderer.hpp"

#include "protekt/game/scene.hpp"
#include "protekt/math/math.hpp"
#include "protekt/rng.hpp"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "post_frag.h"
#include "post_vert.h"
#include "scene_frag.h"
#include "scene_vert.h"

namespace protekt::render {

namespace {

// ================================ maths ================================
// The camera convention is NOT here: it lives in protekt/math/math.hpp, because the picker has to
// agree with the renderer about where the camera looks and two copies only agree by accident.

using math::add;
using math::cross;
using math::dot;
using math::look_at;
using math::Mat4;
using math::multiply;
using math::normalise;
using math::perspective;
using math::Vec3;

// ================================ the scene ================================

struct Vertex {
    Vec3 position;
    Vec3 colour;
    float alpha{1.0F};
    float u{};
    float v{};
    float fog{};
};

constexpr std::uint32_t kUnitVertices = 36;
constexpr std::uint32_t kGroundVertices = 6;
constexpr std::uint32_t kVertexCount = kUnitVertices + kGroundVertices;

/// THE FXAA TUNING, all of it, in one place. These go to post.frag as push constants.
/// Luma is measured on the 8-bit, non-linear colour as written, 0 to 1.
struct FxaaSettings {
    /// A pixel is an edge only if its local luma contrast (brightest minus darkest of itself and
    /// its four neighbours) is at least this fraction of the brightest of them. Lower smooths more
    /// edges, and starts softening texture detail and flat-shaded face boundaries; higher leaves
    /// only the sharpest silhouettes smoothed. FXAA 3.11's own default is 1/8.
    float edge_threshold;
    /// The same test's floor, as an absolute luma, so the dark parts of the frame (where a
    /// fraction of almost nothing is almost nothing) are not smoothed for noise. 1/16 is the
    /// quality preset; raise it to leave dark areas alone.
    float edge_threshold_min;
    /// How hard an isolated high-contrast pixel (a lone bright speck, a one-pixel line) is pulled
    /// toward its neighbours, 0 for not at all to 1 for fully. Edges are blended by the edge walk
    /// regardless of this. FXAA 3.11's own default is 3/4.
    float subpixel_blend;
};
constexpr FxaaSettings kFxaaSettings{0.125F, 0.0625F, 0.75F};

constexpr Vec3 kSky{0.53F, 0.81F, 0.92F};
constexpr Vec3 kUnit{0.85F, 0.15F, 0.15F};

/// Green-dominant, so the ground can never collide with the (red) unit or the (blue) sky.
Vec3 ground_colour(std::uint64_t seed) {
    rng::SplitMix64 stream(seed);
    const auto unit_float = [&stream] {
        return static_cast<float>(stream.next() >> 40U) / static_cast<float>(1U << 24U);
    };
    return {0.10F + 0.25F * unit_float(), 0.35F + 0.40F * unit_float(),
            0.10F + 0.25F * unit_float()};
}

void emit_triangle(std::vector<Vertex>& out, const Vec3& a, const Vec3& b, const Vec3& c,
                   const Vec3& colour) {
    const auto emit = [&out, &colour](const Vec3& position) {
        out.push_back(Vertex{position, colour, 1.0F, 0.0F, 0.0F, 0.0F});
    };
    emit(a);
    emit(b);
    emit(c);
}

void emit_quad(std::vector<Vertex>& out, const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d,
               const Vec3& colour) {
    emit_triangle(out, a, b, c, colour);
    emit_triangle(out, a, c, d, colour);
}

/// The unit's 12 triangles FIRST, the ground's 2 second: the depth test is what keeps the unit
/// visible, not the draw order.
std::vector<Vertex> build_scene(std::uint64_t seed) {
    std::vector<Vertex> v;
    v.reserve(kVertexCount);

    constexpr float x0 = -0.5F;
    constexpr float x1 = 0.5F;
    constexpr float y0 = 0.0F;
    constexpr float y1 = 1.8F;
    constexpr float z0 = 5.5F;
    constexpr float z1 = 6.5F;
    const std::array<Vec3, 8> p{{{x0, y0, z0},
                                 {x1, y0, z0},
                                 {x1, y1, z0},
                                 {x0, y1, z0},
                                 {x0, y0, z1},
                                 {x1, y0, z1},
                                 {x1, y1, z1},
                                 {x0, y1, z1}}};
    emit_quad(v, p[0], p[1], p[2], p[3], kUnit); // near
    emit_quad(v, p[5], p[4], p[7], p[6], kUnit); // far
    emit_quad(v, p[4], p[0], p[3], p[7], kUnit); // left
    emit_quad(v, p[1], p[5], p[6], p[2], kUnit); // right
    emit_quad(v, p[3], p[2], p[6], p[7], kUnit); // top
    emit_quad(v, p[4], p[5], p[1], p[0], kUnit); // bottom

    const Vec3 g = ground_colour(seed);
    constexpr float gx = 150.0F;
    constexpr float gz0 = -20.0F;
    constexpr float gz1 = 150.0F;
    emit_quad(v, {-gx, 0.0F, gz0}, {gx, 0.0F, gz0}, {gx, 0.0F, gz1}, {-gx, 0.0F, gz1}, g);
    return v;
}

/// Eye at 1.6 m, looking forward and pitched down 10 degrees so that the horizon sits above the
/// middle of the frame and the unit stands on the ground below it.
Mat4 view_projection(game::Extent extent) {
    const float pitch = 10.0F * math::kPi / 180.0F;
    const Vec3 eye{0.0F, 1.6F, 0.0F};
    const Vec3 target{0.0F, 1.6F - 10.0F * std::sin(pitch), 10.0F * std::cos(pitch)};
    const float aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);
    return multiply(perspective(math::kFovRadians, aspect, math::kNearPlane, math::kFarPlane),
                    look_at(eye, target, {0.0F, 1.0F, 0.0F}));
}

// ================================ devices ================================

std::string lowercase(std::string_view s) {
    std::string r(s);
    std::transform(r.begin(), r.end(), r.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return r;
}

struct Candidate {
    VkPhysicalDevice handle{};
    DeviceInfo info;
    std::uint32_t queue_family{};
};

/// Every device with a graphics queue, discrete first, then integrated, then everything else.
std::vector<Candidate> enumerate_candidates(VkInstance instance, std::string& error) {
    std::uint32_t count = 0;
    VkResult r = vkEnumeratePhysicalDevices(instance, &count, nullptr);
    if (r != VK_SUCCESS) {
        error = "vkEnumeratePhysicalDevices failed (" + std::to_string(r) + ")";
        return {};
    }
    std::vector<VkPhysicalDevice> handles(count);
    r = vkEnumeratePhysicalDevices(instance, &count, handles.data());
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) {
        error = "vkEnumeratePhysicalDevices failed (" + std::to_string(r) + ")";
        return {};
    }
    handles.resize(count);

    std::vector<Candidate> found;
    for (VkPhysicalDevice handle : handles) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(handle, &props);

        std::uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(handle, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(handle, &family_count, families.data());
        std::uint32_t family = family_count;
        for (std::uint32_t i = 0; i < family_count; ++i) {
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0U) {
                family = i;
                break;
            }
        }
        if (family == family_count) {
            continue;
        }

        Candidate c;
        c.handle = handle;
        c.queue_family = family;
        c.info.name = static_cast<const char*>(props.deviceName);
        c.info.vendor_id = props.vendorID;
        c.info.device_id = props.deviceID;
        c.info.api_version = props.apiVersion;
        c.info.discrete = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
        c.info.integrated = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
        c.info.cpu = !c.info.discrete && !c.info.integrated;
        found.push_back(std::move(c));
    }
    const auto rank = [](const Candidate& c) {
        return c.info.discrete ? 0 : c.info.integrated ? 1 : 2;
    };
    std::stable_sort(found.begin(), found.end(),
                     [&rank](const Candidate& a, const Candidate& b) { return rank(a) < rank(b); });
    if (found.empty()) {
        error = "no Vulkan device with a graphics queue";
    }
    return found;
}

/// The shaders are built for Vulkan 1.3 (SPIR-V 1.6), so ask for 1.3 when the loader has it.
std::uint32_t instance_api_version() {
    std::uint32_t version = VK_API_VERSION_1_0;
    // vkGetInstanceProcAddr returns PFN_vkVoidFunction, so naming a typed entry point IS a cast.
    // That is the loader's contract and there is no portable alternative (rule 2's one exception).
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto fn = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
        vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
    if (fn != nullptr && fn(&version) != VK_SUCCESS) {
        version = VK_API_VERSION_1_0;
    }
    return std::min<std::uint32_t>(version, VK_API_VERSION_1_3);
}

VkInstance make_plain_instance(std::string& error) {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "protekt";
    app.apiVersion = instance_api_version();
    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    const VkResult r = vkCreateInstance(&info, nullptr, &instance);
    if (r != VK_SUCCESS) {
        error = "vkCreateInstance failed (" + std::to_string(r)
                + "): no Vulkan loader or driver is usable";
        return VK_NULL_HANDLE;
    }
    return instance;
}

/// True for a finding the VALIDATION LAYER raises about our use of the API, which is what this
/// project counts and asserts on.
///
/// Everything else that arrives through this callback is somebody else's news: the loader reports
/// an ICD it cannot load ("loader_icd_scan", "terminator_CreateInstance ... Skipping this driver")
/// and checks the extension names in a VkDeviceCreateInfo it is about to pass on -- where an
/// implicit layer such as Mesa's VK_LAYER_MESA_device_select may have ADDED an extension the
/// application never asked for (measured on nobara-pc: the loader complained that
/// VK_KHR_get_physical_device_properties2 is an instance extension, in a vkCreateDevice call this
/// renderer makes with no device extensions at all). Counting those would make the suite fail on a
/// machine whose driver stack is unhappy, which says nothing about this renderer.
///
/// Validation findings carry a "VUID-" message id; loader and ICD diagnostics do not.
/// Loader and ICD diagnostics are off unless asked for. They are the MACHINE's news, not this
/// renderer's, and a player met them as four lines that read like a failure before the window even
/// opened (measured on nobara-pc: a broken `dzn` ICD and Mesa's device_select layer). Set
/// PROTEKT_VULKAN_VERBOSE=1 to see them.
[[nodiscard]] bool verbose_diagnostics() {
    static const bool enabled = std::getenv("PROTEKT_VULKAN_VERBOSE") != nullptr;
    return enabled;
}

[[nodiscard]] bool is_validation_finding(const VkDebugUtilsMessengerCallbackDataEXT* data) {
    if (data == nullptr || data->pMessageIdName == nullptr) {
        return false;
    }
    return std::string_view(data->pMessageIdName).rfind("VUID-", 0) == 0;
}

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                              VkDebugUtilsMessageTypeFlagsEXT /*types*/,
                                              const VkDebugUtilsMessengerCallbackDataEXT* data,
                                              void* user) {
    if ((severity
         & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT
            | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT))
        != 0U) {
        const bool ours = is_validation_finding(data);
        if (ours) {
            static_cast<std::atomic<std::uint32_t>*>(user)->fetch_add(1U);
        }
        // Ours are always printed -- they are bugs in this renderer. Other people's need the
        // opt-in.
        if (!ours && !verbose_diagnostics()) {
            return VK_FALSE;
        }
        std::cerr << "[vulkan validation] "
                  << ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0U
                          ? "error: "
                          : "warning: ")
                  << (data != nullptr && data->pMessageIdName != nullptr ? data->pMessageIdName
                                                                         : "(no id)")
                  << " "
                  << (data != nullptr && data->pMessage != nullptr ? data->pMessage
                                                                   : "(no message)")
                  << '\n';
    }
    return VK_FALSE;
}

// ================================ the renderer ================================

class VulkanRenderer final : public Renderer {
public:
    VulkanRenderer() = default;
    ~VulkanRenderer() override { destroy(); }
    VulkanRenderer(const VulkanRenderer&) = delete;
    VulkanRenderer& operator=(const VulkanRenderer&) = delete;
    VulkanRenderer(VulkanRenderer&&) = delete;
    VulkanRenderer& operator=(VulkanRenderer&&) = delete;

    bool init(const CreateOptions& options, std::string& error) {
        extent_ = options.extent;
        atlas_rgba_ = options.atlas_rgba;
        atlas_side_ = options.atlas_side;
        fxaa_ = options.fxaa;
        if (options.extent.width == 0 || options.extent.height == 0) {
            error = "refusing to create a renderer with an empty extent ("
                    + std::to_string(options.extent.width) + "x"
                    + std::to_string(options.extent.height) + ")";
            return false;
        }
        if (!create_instance(options.validation, error)) {
            return false;
        }
        std::string enumerate_error;
        const std::vector<Candidate> candidates = enumerate_candidates(instance_, enumerate_error);
        if (candidates.empty()) {
            error = enumerate_error;
            return false;
        }
        const Candidate* chosen = &candidates.front();
        if (!options.device_name.empty()) {
            chosen = nullptr;
            const std::string wanted = lowercase(options.device_name);
            for (const Candidate& c : candidates) {
                if (lowercase(c.info.name).find(wanted) != std::string::npos) {
                    chosen = &c;
                    break;
                }
            }
            if (chosen == nullptr) {
                error = "no Vulkan device matches '" + options.device_name + "'; available:";
                for (const Candidate& c : candidates) {
                    error += " '" + c.info.name + "'";
                }
                return false;
            }
        }
        physical_ = chosen->handle;
        queue_family_ = chosen->queue_family;
        device_name_ = chosen->info.name;
        vkGetPhysicalDeviceMemoryProperties(physical_, &memory_);

        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(physical_, &props);
        max_dimension_ = std::min(
            props.limits.maxImageDimension2D,
            std::min(props.limits.maxFramebufferWidth, props.limits.maxFramebufferHeight));
        if (options.extent.width > max_dimension_ || options.extent.height > max_dimension_) {
            error = "extent exceeds the device limit of " + std::to_string(max_dimension_);
            return false;
        }
        return create_device(error) && create_render_passes(error) && create_pipeline(error)
               && create_post_pipeline(error) && create_frame_objects(error)
               && create_texture(error) && create_target(options.extent, error);
    }

    [[nodiscard]] game::Extent extent() const noexcept override { return extent_; }
    [[nodiscard]] std::string_view device_name() const noexcept override { return device_name_; }
    [[nodiscard]] std::uint32_t validation_errors() const noexcept override {
        return validation_count_.load();
    }

    std::vector<std::uint8_t> render(const FrameRequest& request, FrameStats& stats) override {
        stats = FrameStats{};
        try {
            return render_frame(request, stats);
        } catch (const std::exception& e) {
            std::cerr << "protekt render: " << e.what() << '\n';
        } catch (...) {
            std::cerr << "protekt render: unknown exception\n";
        }
        stats = FrameStats{};
        return {};
    }

private:
    struct Target {
        game::Extent extent{};
        /// Where the world is drawn, and what the FXAA pass samples.
        VkImage scene{};
        VkDeviceMemory scene_memory{};
        VkImageView scene_view{};
        /// The smoothed copy the HUD is drawn onto, and what is read back.
        VkImage colour{};
        VkDeviceMemory colour_memory{};
        VkImageView colour_view{};
        VkImage depth{};
        VkDeviceMemory depth_memory{};
        VkImageView depth_view{};
        VkFramebuffer scene_framebuffer{};
        VkFramebuffer post_framebuffer{};
        VkFramebuffer hud_framebuffer{};
        VkBuffer readback{};
        VkDeviceMemory readback_memory{};
        void* mapped{};
    };

    static constexpr VkFormat kColourFormat = VK_FORMAT_R8G8B8A8_UNORM;

    /// post.frag's push constants, in its order. `enabled` is 0 for an exact copy.
    struct PostPush {
        float edge_threshold;
        float edge_threshold_min;
        float subpixel_blend;
        std::uint32_t enabled;
    };

    // ---- setup

    bool create_instance(bool want_validation, std::string& error) {
        std::uint32_t layer_count = 0;
        vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
        std::vector<VkLayerProperties> layers(layer_count);
        vkEnumerateInstanceLayerProperties(&layer_count, layers.data());
        std::uint32_t ext_count = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &ext_count, nullptr);
        std::vector<VkExtensionProperties> exts(ext_count);
        vkEnumerateInstanceExtensionProperties(nullptr, &ext_count, exts.data());

        const char* const validation_layer = "VK_LAYER_KHRONOS_validation";
        const bool have_layer = std::any_of(layers.begin(), layers.end(), [&](const auto& l) {
            return std::strcmp(static_cast<const char*>(l.layerName), validation_layer) == 0;
        });
        const bool have_utils = std::any_of(exts.begin(), exts.end(), [](const auto& e) {
            return std::strcmp(static_cast<const char*>(e.extensionName),
                               VK_EXT_DEBUG_UTILS_EXTENSION_NAME)
                   == 0;
        });
        const bool validate = want_validation && have_layer && have_utils;

        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "protekt";
        app.apiVersion = instance_api_version();

        VkDebugUtilsMessengerCreateInfoEXT messenger{};
        messenger.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        messenger.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                                    | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        messenger.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                                | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                                | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        messenger.pfnUserCallback = debug_callback;
        messenger.pUserData = &validation_count_;

        VkInstanceCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        info.pApplicationInfo = &app;
        const char* const extension = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        if (validate) {
            info.enabledLayerCount = 1;
            info.ppEnabledLayerNames = &validation_layer;
            info.enabledExtensionCount = 1;
            info.ppEnabledExtensionNames = &extension;
            info.pNext = &messenger;
        }
        const VkResult r = vkCreateInstance(&info, nullptr, &instance_);
        if (r != VK_SUCCESS) {
            instance_ = VK_NULL_HANDLE;
            error = "vkCreateInstance failed (" + std::to_string(r)
                    + "): no Vulkan loader or driver is usable";
            return false;
        }
        if (validate) {
            // The same loader contract as above: there is no portable cast from PFN_vkVoidFunction.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
            const auto create_fn = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
            destroy_messenger_ = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
            if (create_fn != nullptr && destroy_messenger_ != nullptr
                && create_fn(instance_, &messenger, nullptr, &messenger_) != VK_SUCCESS) {
                messenger_ = VK_NULL_HANDLE;
            }
        }
        return true;
    }

    bool create_device(std::string& error) {
        const float priority = 1.0F;
        VkDeviceQueueCreateInfo queue{};
        queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue.queueFamilyIndex = queue_family_;
        queue.queueCount = 1;
        queue.pQueuePriorities = &priority;
        VkDeviceCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        info.queueCreateInfoCount = 1;
        info.pQueueCreateInfos = &queue;
        if (const VkResult r = vkCreateDevice(physical_, &info, nullptr, &device_);
            r != VK_SUCCESS) {
            device_ = VK_NULL_HANDLE;
            error = "vkCreateDevice failed (" + std::to_string(r) + ") for " + device_name_;
            return false;
        }
        vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

        for (const VkFormat candidate :
             {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D16_UNORM}) {
            VkFormatProperties fp{};
            vkGetPhysicalDeviceFormatProperties(physical_, candidate, &fp);
            if ((fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0U) {
                depth_format_ = candidate;
                break;
            }
        }
        if (depth_format_ == VK_FORMAT_UNDEFINED) {
            error = device_name_ + " supports no usable depth format";
            return false;
        }
        return true;
    }

    /// The three passes (see the top of the file). The world and HUD passes are COMPATIBLE -- the
    /// same attachment formats in the same one subpass, differing only in load/store and layouts --
    /// so the one scene pipeline draws in both.
    bool create_render_passes(std::string& error) {
        VkAttachmentDescription colour{};
        colour.format = kColourFormat;
        colour.samples = VK_SAMPLE_COUNT_1_BIT;
        colour.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colour.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        VkAttachmentDescription depth{};
        depth.format = depth_format_;
        depth.samples = VK_SAMPLE_COUNT_1_BIT;
        depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        const VkAttachmentReference colour_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        const VkAttachmentReference depth_ref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription with_depth{};
        with_depth.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        with_depth.colorAttachmentCount = 1;
        with_depth.pColorAttachments = &colour_ref;
        with_depth.pDepthStencilAttachment = &depth_ref;
        VkSubpassDescription colour_only = with_depth;
        colour_only.pDepthStencilAttachment = nullptr;

        const auto dependency = [](std::uint32_t src, std::uint32_t dst,
                                   VkPipelineStageFlags src_stage, VkAccessFlags src_access,
                                   VkPipelineStageFlags dst_stage, VkAccessFlags dst_access) {
            VkSubpassDependency d{};
            d.srcSubpass = src;
            d.dstSubpass = dst;
            d.srcStageMask = src_stage;
            d.srcAccessMask = src_access;
            d.dstStageMask = dst_stage;
            d.dstAccessMask = dst_access;
            return d;
        };
        constexpr VkPipelineStageFlags kColourOut = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        constexpr VkAccessFlags kColourWrite = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        constexpr VkAccessFlags kColourReadWrite
            = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        constexpr VkPipelineStageFlags kFragmentTests = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
                                                        | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        constexpr VkAccessFlags kDepthReadWrite = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                                                  | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

        const auto make = [this](std::span<const VkAttachmentDescription> attachments,
                                 const VkSubpassDescription& subpass,
                                 std::span<const VkSubpassDependency> deps, VkRenderPass& out) {
            VkRenderPassCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
            info.attachmentCount = static_cast<std::uint32_t>(attachments.size());
            info.pAttachments = attachments.data();
            info.subpassCount = 1;
            info.pSubpasses = &subpass;
            info.dependencyCount = static_cast<std::uint32_t>(deps.size());
            info.pDependencies = deps.data();
            if (vkCreateRenderPass(device_, &info, nullptr, &out) != VK_SUCCESS) {
                out = VK_NULL_HANDLE;
                return false;
            }
            return true;
        };

        // 1. The world. Colour ends SHADER_READ_ONLY for the filter; depth is KEPT for the HUD.
        std::array<VkAttachmentDescription, 2> world{colour, depth};
        world[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        world[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        world[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        world[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        world[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        world[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        world[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        // The world and HUD passes share ONE dependency list: render-pass compatibility compares
        // the dependencies too, and the scene pipeline has to draw in both. Each pass takes what it
        // needs from it (the filter reads the colour; the HUD pass's copy reads the transfer).
        // Before: the previous frame's copy must be finished before this pass writes. The writes of
        // the pass before are made visible by THAT pass's own "after" dependency.
        // After: colour and depth are visible to whatever reads or blends them next.
        constexpr VkPipelineStageFlags kReaders = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                                  | VK_PIPELINE_STAGE_TRANSFER_BIT | kColourOut
                                                  | kFragmentTests;
        const std::array<VkSubpassDependency, 2> scene_deps{
            dependency(VK_SUBPASS_EXTERNAL, 0,
                       VK_PIPELINE_STAGE_TRANSFER_BIT | kColourOut | kFragmentTests, 0,
                       kColourOut | kFragmentTests, kColourReadWrite | kDepthReadWrite),
            dependency(0, VK_SUBPASS_EXTERNAL,
                       kColourOut | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                       kColourWrite | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, kReaders,
                       VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | kColourReadWrite
                           | kDepthReadWrite),
        };
        // 2. The filter: every pixel is written, so nothing needs loading.
        std::array<VkAttachmentDescription, 1> post{colour};
        post[0].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        post[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        post[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        post[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        const std::array<VkSubpassDependency, 2> post_deps{
            // Before: the previous frame's readback of this image must be finished.
            dependency(VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, kColourOut,
                       kColourWrite),
            // After: the HUD blends over these pixels, so it reads them as well as writes.
            dependency(0, VK_SUBPASS_EXTERNAL, kColourOut, kColourWrite, kColourOut,
                       kColourReadWrite),
        };
        // 3. The HUD, over the filter's output and against the world's depth. Ends TRANSFER_SRC.
        std::array<VkAttachmentDescription, 2> hud{colour, depth};
        hud[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        hud[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        hud[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        hud[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        hud[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        hud[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        if (!make(world, with_depth, scene_deps, render_pass_)
            || !make(post, colour_only, post_deps, post_pass_)
            || !make(hud, with_depth, scene_deps, hud_pass_)) {
            error = "vkCreateRenderPass failed";
            return false;
        }
        return true;
    }

    VkShaderModule make_shader(std::span<const std::uint32_t> code) {
        VkShaderModuleCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = code.size_bytes();
        info.pCode = code.data();
        VkShaderModule module = VK_NULL_HANDLE;
        if (vkCreateShaderModule(device_, &info, nullptr, &module) != VK_SUCCESS) {
            return VK_NULL_HANDLE;
        }
        return module;
    }

    bool create_pipeline(std::string& error) {
        vert_ = make_shader(std::span<const std::uint32_t>{scene_vert_spv});
        frag_ = make_shader(std::span<const std::uint32_t>{scene_frag_spv});
        if (vert_ == VK_NULL_HANDLE || frag_ == VK_NULL_HANDLE) {
            error = "vkCreateShaderModule failed";
            return false;
        }

        VkDescriptorSetLayoutBinding texture_binding{};
        texture_binding.binding = 0;
        texture_binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        texture_binding.descriptorCount = 1;
        texture_binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo set_layout{};
        set_layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        set_layout.bindingCount = 1;
        set_layout.pBindings = &texture_binding;
        if (vkCreateDescriptorSetLayout(device_, &set_layout, nullptr, &set_layout_)
            != VK_SUCCESS) {
            set_layout_ = VK_NULL_HANDLE;
            error = "vkCreateDescriptorSetLayout failed";
            return false;
        }

        // The matrix for the vertex stage, then the fog colour for the fragment stage after it.
        const std::array<VkPushConstantRange, 2> push{{
            {VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Mat4)},
            {VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(Mat4), sizeof(Vec3)},
        }};
        VkPipelineLayoutCreateInfo layout{};
        layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout.setLayoutCount = 1;
        layout.pSetLayouts = &set_layout_;
        layout.pushConstantRangeCount = static_cast<std::uint32_t>(push.size());
        layout.pPushConstantRanges = push.data();
        if (vkCreatePipelineLayout(device_, &layout, nullptr, &pipeline_layout_) != VK_SUCCESS) {
            pipeline_layout_ = VK_NULL_HANDLE;
            error = "vkCreatePipelineLayout failed";
            return false;
        }

        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vert_;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = frag_;
        stages[1].pName = "main";

        VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
        std::array<VkVertexInputAttributeDescription, 5> attributes{{
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
            {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, colour)},
            {2, 0, VK_FORMAT_R32_SFLOAT, offsetof(Vertex, alpha)},
            {3, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, u)},
            {4, 0, VK_FORMAT_R32_SFLOAT, offsetof(Vertex, fog)},
        }};
        VkPipelineVertexInputStateCreateInfo vertex_input{};
        vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertex_input.vertexBindingDescriptionCount = 1;
        vertex_input.pVertexBindingDescriptions = &binding;
        vertex_input.vertexAttributeDescriptionCount
            = static_cast<std::uint32_t>(attributes.size());
        vertex_input.pVertexAttributeDescriptions = attributes.data();

        VkPipelineInputAssemblyStateCreateInfo assembly{};
        assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo viewport{};
        viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo raster{};
        raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0F;

        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depth{};
        depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depth.depthTestEnable = VK_TRUE;
        depth.depthWriteEnable = VK_TRUE;
        depth.depthCompareOp = VK_COMPARE_OP_LESS;

        // Straight alpha: source over destination. World geometry carries alpha 1, so
        // src*1 + dst*(1-1) = src and nothing opaque changes. Only the HUD's panel faces opt in.
        VkPipelineColorBlendAttachmentState blend_attachment{};
        blend_attachment.blendEnable = VK_TRUE;
        blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
        blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
        blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                                          | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{};
        blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blend.attachmentCount = 1;
        blend.pAttachments = &blend_attachment;

        const std::array<VkDynamicState, 2> dynamic_states{VK_DYNAMIC_STATE_VIEWPORT,
                                                           VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{};
        dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamic_states.size());
        dynamic.pDynamicStates = dynamic_states.data();

        VkGraphicsPipelineCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        info.stageCount = static_cast<std::uint32_t>(stages.size());
        info.pStages = stages.data();
        info.pVertexInputState = &vertex_input;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pDepthStencilState = &depth;
        info.pColorBlendState = &blend;
        info.pDynamicState = &dynamic;
        info.layout = pipeline_layout_;
        info.renderPass = render_pass_;
        info.subpass = 0;
        if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline_)
            != VK_SUCCESS) {
            pipeline_ = VK_NULL_HANDLE;
            error = "vkCreateGraphicsPipelines failed";
            return false;
        }
        return true;
    }

    /// The fullscreen pass: the filter's pipeline, the layout of the one image it reads, and the
    /// descriptor set that points at it (pointed at the real image by `create_target`, which makes
    /// a new image on every resize). Nothing here is FXAA-specific except the fragment shader and
    /// its push constants, so a later pass reuses the rest.
    bool create_post_pipeline(std::string& error) {
        post_vert_ = make_shader(std::span<const std::uint32_t>{post_vert_spv});
        post_frag_ = make_shader(std::span<const std::uint32_t>{post_frag_spv});
        if (post_vert_ == VK_NULL_HANDLE || post_frag_ == VK_NULL_HANDLE) {
            error = "vkCreateShaderModule failed for the post pass";
            return false;
        }

        // The scene image is read by integer texel fetch alone, so it is a sampled image with no
        // sampler: there is no filter state that could differ between drivers.
        VkDescriptorSetLayoutBinding image_binding{};
        image_binding.binding = 0;
        image_binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        image_binding.descriptorCount = 1;
        image_binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo set_layout{};
        set_layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        set_layout.bindingCount = 1;
        set_layout.pBindings = &image_binding;
        if (vkCreateDescriptorSetLayout(device_, &set_layout, nullptr, &post_set_layout_)
            != VK_SUCCESS) {
            post_set_layout_ = VK_NULL_HANDLE;
            error = "vkCreateDescriptorSetLayout failed for the post pass";
            return false;
        }
        const VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PostPush)};
        VkPipelineLayoutCreateInfo layout{};
        layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout.setLayoutCount = 1;
        layout.pSetLayouts = &post_set_layout_;
        layout.pushConstantRangeCount = 1;
        layout.pPushConstantRanges = &push;
        if (vkCreatePipelineLayout(device_, &layout, nullptr, &post_layout_) != VK_SUCCESS) {
            post_layout_ = VK_NULL_HANDLE;
            error = "vkCreatePipelineLayout failed for the post pass";
            return false;
        }

        VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1};
        VkDescriptorPoolCreateInfo pool{};
        pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool.maxSets = 1;
        pool.poolSizeCount = 1;
        pool.pPoolSizes = &pool_size;
        if (vkCreateDescriptorPool(device_, &pool, nullptr, &post_pool_) != VK_SUCCESS) {
            post_pool_ = VK_NULL_HANDLE;
            error = "vkCreateDescriptorPool failed for the post pass";
            return false;
        }
        VkDescriptorSetAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc.descriptorPool = post_pool_;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &post_set_layout_;
        if (vkAllocateDescriptorSets(device_, &alloc, &post_set_) != VK_SUCCESS) {
            error = "vkAllocateDescriptorSets failed for the post pass";
            return false;
        }

        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = post_vert_;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = post_frag_;
        stages[1].pName = "main";

        VkPipelineVertexInputStateCreateInfo vertex_input{};
        vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo assembly{};
        assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{};
        viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{};
        raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0F;
        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        // No blending: the pass REPLACES every pixel with the filtered one.
        VkPipelineColorBlendAttachmentState blend_attachment{};
        blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                                          | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{};
        blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blend.attachmentCount = 1;
        blend.pAttachments = &blend_attachment;
        const std::array<VkDynamicState, 2> dynamic_states{VK_DYNAMIC_STATE_VIEWPORT,
                                                           VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{};
        dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamic_states.size());
        dynamic.pDynamicStates = dynamic_states.data();

        VkGraphicsPipelineCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        info.stageCount = static_cast<std::uint32_t>(stages.size());
        info.pStages = stages.data();
        info.pVertexInputState = &vertex_input;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pColorBlendState = &blend;
        info.pDynamicState = &dynamic;
        info.layout = post_layout_;
        info.renderPass = post_pass_;
        info.subpass = 0;
        if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &post_pipeline_)
            != VK_SUCCESS) {
            post_pipeline_ = VK_NULL_HANDLE;
            error = "vkCreateGraphicsPipelines failed for the post pass";
            return false;
        }
        return true;
    }

    std::uint32_t find_memory_type(std::uint32_t type_bits, VkMemoryPropertyFlags wanted) const {
        // std::span has no at() until C++26, and a range-for needs no subscript at all.
        const std::span<const VkMemoryType> types{
            static_cast<const VkMemoryType*>(memory_.memoryTypes), memory_.memoryTypeCount};
        std::uint32_t index = 0;
        for (const VkMemoryType& type : types) {
            if ((type_bits & (1U << index)) != 0U && (type.propertyFlags & wanted) == wanted) {
                return index;
            }
            ++index;
        }
        return UINT32_MAX;
    }

    /// Allocates memory for `requirements`, trying `preferred` first and then `required`.
    VkDeviceMemory allocate(const VkMemoryRequirements& requirements,
                            VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred) const {
        std::uint32_t type = find_memory_type(requirements.memoryTypeBits, required | preferred);
        if (type == UINT32_MAX) {
            type = find_memory_type(requirements.memoryTypeBits, required);
        }
        if (type == UINT32_MAX) {
            return VK_NULL_HANDLE;
        }
        VkMemoryAllocateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        info.allocationSize = requirements.size;
        info.memoryTypeIndex = type;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        if (vkAllocateMemory(device_, &info, nullptr, &memory) != VK_SUCCESS) {
            return VK_NULL_HANDLE;
        }
        return memory;
    }

    bool make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buffer,
                     VkDeviceMemory& memory, void*& mapped) {
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = size;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device_, &info, nullptr, &buffer) != VK_SUCCESS) {
            buffer = VK_NULL_HANDLE;
            return false;
        }
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device_, buffer, &req);
        memory = allocate(
            req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
        if (memory == VK_NULL_HANDLE || vkBindBufferMemory(device_, buffer, memory, 0) != VK_SUCCESS
            || vkMapMemory(device_, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
            return false;
        }
        return true;
    }

    bool make_image(game::Extent extent, VkFormat format, VkImageUsageFlags usage,
                    VkImageAspectFlags aspect, VkImage& image, VkDeviceMemory& memory,
                    VkImageView& view) {
        VkImageCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = {extent.width, extent.height, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(device_, &info, nullptr, &image) != VK_SUCCESS) {
            image = VK_NULL_HANDLE;
            return false;
        }
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(device_, image, &req);
        memory = allocate(req, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (memory == VK_NULL_HANDLE
            || vkBindImageMemory(device_, image, memory, 0) != VK_SUCCESS) {
            return false;
        }
        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = format;
        view_info.subresourceRange = {aspect, 0, 1, 0, 1};
        if (vkCreateImageView(device_, &view_info, nullptr, &view) != VK_SUCCESS) {
            view = VK_NULL_HANDLE;
            return false;
        }
        return true;
    }

    bool create_frame_objects(std::string& error) {
        VkCommandPoolCreateInfo pool{};
        pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool.queueFamilyIndex = queue_family_;
        if (vkCreateCommandPool(device_, &pool, nullptr, &pool_) != VK_SUCCESS) {
            pool_ = VK_NULL_HANDLE;
            error = "vkCreateCommandPool failed";
            return false;
        }
        VkCommandBufferAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc.commandPool = pool_;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(device_, &alloc, &cmd_) != VK_SUCCESS) {
            error = "vkAllocateCommandBuffers failed";
            return false;
        }
        VkFenceCreateInfo fence{};
        fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (vkCreateFence(device_, &fence, nullptr, &fence_) != VK_SUCCESS) {
            fence_ = VK_NULL_HANDLE;
            error = "vkCreateFence failed";
            return false;
        }
        if (!make_buffer(sizeof(Vertex) * kVertexCount, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                         vertex_buffer_, vertex_memory_, vertex_mapped_)) {
            error = "could not allocate the vertex buffer on " + device_name_;
            return false;
        }
        vertex_capacity_bytes_ = sizeof(Vertex) * kVertexCount;
        return true;
    }

    /// The one texture, uploaded once: an in-memory 2x2 opaque white RGBA image, and the one
    /// sampler that reads it. The sampler is the determinism contract -- NEAREST, no mipmaps, no
    /// anisotropy -- because integer texel fetches are what keep frames byte-identical across
    /// drivers. UNORM, not sRGB, so a texel of 255 reads back as exactly 1.0.
    bool create_texture(std::string& error) {
        constexpr std::size_t kSide = 2;
        const std::vector<std::uint8_t> white(kSide * kSide * 4U, 255);
        const bool supplied
            = atlas_side_ > 0
              && atlas_rgba_.size() == static_cast<std::size_t>(atlas_side_) * atlas_side_ * 4U;
        VkBuffer staging{};
        VkDeviceMemory staging_memory{};
        void* staging_mapped = nullptr;
        const bool ok = upload_texture(supplied ? atlas_rgba_ : white,
                                       supplied ? atlas_side_ : static_cast<std::uint32_t>(kSide),
                                       staging, staging_memory, staging_mapped);
        if (staging_mapped != nullptr) {
            vkUnmapMemory(device_, staging_memory);
        }
        vkDestroyBuffer(device_, staging, nullptr);
        vkFreeMemory(device_, staging_memory, nullptr);
        if (!ok) {
            error = "could not create the scene texture on " + device_name_;
            return false;
        }

        VkSamplerCreateInfo sampler{};
        sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler.magFilter = VK_FILTER_NEAREST;
        sampler.minFilter = VK_FILTER_NEAREST;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.anisotropyEnable = VK_FALSE;
        sampler.maxAnisotropy = 1.0F;
        sampler.minLod = 0.0F;
        sampler.maxLod = 0.0F;
        if (vkCreateSampler(device_, &sampler, nullptr, &sampler_) != VK_SUCCESS) {
            sampler_ = VK_NULL_HANDLE;
            error = "vkCreateSampler failed";
            return false;
        }

        VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
        VkDescriptorPoolCreateInfo pool{};
        pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool.maxSets = 1;
        pool.poolSizeCount = 1;
        pool.pPoolSizes = &pool_size;
        if (vkCreateDescriptorPool(device_, &pool, nullptr, &descriptor_pool_) != VK_SUCCESS) {
            descriptor_pool_ = VK_NULL_HANDLE;
            error = "vkCreateDescriptorPool failed";
            return false;
        }
        VkDescriptorSetAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc.descriptorPool = descriptor_pool_;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &set_layout_;
        if (vkAllocateDescriptorSets(device_, &alloc, &descriptor_set_) != VK_SUCCESS) {
            error = "vkAllocateDescriptorSets failed";
            return false;
        }
        VkDescriptorImageInfo image_info{sampler_, texture_view_,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = descriptor_set_;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &image_info;
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
        return true;
    }

    /// Creates the texture image, copies `pixels` (tightly packed RGBA8, `side` x `side`) into it
    /// through the caller's staging buffer, and leaves it in SHADER_READ_ONLY_OPTIMAL.
    bool upload_texture(std::span<const std::uint8_t> pixels, std::uint32_t side, VkBuffer& staging,
                        VkDeviceMemory& staging_memory, void*& staging_mapped) {
        if (!make_buffer(pixels.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging, staging_memory,
                         staging_mapped)
            || !make_image({side, side}, VK_FORMAT_R8G8B8A8_UNORM,
                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT, texture_, texture_memory_, texture_view_)) {
            return false;
        }
        std::memcpy(staging_mapped, pixels.data(), pixels.size());

        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd_, &begin);

        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = texture_;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &barrier);

        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {side, side, 1};
        vkCmdCopyBufferToImage(cmd_, staging, texture_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                               &copy);

        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &barrier);
        if (vkEndCommandBuffer(cmd_) != VK_SUCCESS) {
            return false;
        }

        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd_;
        vkResetFences(device_, 1, &fence_);
        return vkQueueSubmit(queue_, 1, &submit, fence_) == VK_SUCCESS
               && vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
    }

    /// Grows the vertex buffer to hold at least `bytes`, if it does not already: a caller-supplied
    /// Scene has no fixed size, unlike the built-in test scene.
    bool ensure_vertex_capacity(VkDeviceSize bytes, std::string& error) {
        if (bytes <= vertex_capacity_bytes_) {
            return true;
        }
        if (vertex_mapped_ != nullptr) {
            vkUnmapMemory(device_, vertex_memory_);
            vertex_mapped_ = nullptr;
        }
        vkDestroyBuffer(device_, vertex_buffer_, nullptr);
        vkFreeMemory(device_, vertex_memory_, nullptr);
        vertex_buffer_ = VK_NULL_HANDLE;
        vertex_memory_ = VK_NULL_HANDLE;
        vertex_capacity_bytes_ = 0;
        if (!make_buffer(bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertex_buffer_, vertex_memory_,
                         vertex_mapped_)) {
            error = "could not grow the vertex buffer to " + std::to_string(bytes) + " bytes on "
                    + device_name_;
            return false;
        }
        vertex_capacity_bytes_ = bytes;
        return true;
    }

    void destroy_target() {
        if (device_ == VK_NULL_HANDLE) {
            return;
        }
        vkDeviceWaitIdle(device_);
        if (target_.mapped != nullptr) {
            vkUnmapMemory(device_, target_.readback_memory);
        }
        vkDestroyFramebuffer(device_, target_.scene_framebuffer, nullptr);
        vkDestroyFramebuffer(device_, target_.post_framebuffer, nullptr);
        vkDestroyFramebuffer(device_, target_.hud_framebuffer, nullptr);
        vkDestroyImageView(device_, target_.scene_view, nullptr);
        vkDestroyImageView(device_, target_.colour_view, nullptr);
        vkDestroyImageView(device_, target_.depth_view, nullptr);
        vkDestroyImage(device_, target_.scene, nullptr);
        vkDestroyImage(device_, target_.colour, nullptr);
        vkDestroyImage(device_, target_.depth, nullptr);
        vkFreeMemory(device_, target_.scene_memory, nullptr);
        vkFreeMemory(device_, target_.colour_memory, nullptr);
        vkFreeMemory(device_, target_.depth_memory, nullptr);
        vkDestroyBuffer(device_, target_.readback, nullptr);
        vkFreeMemory(device_, target_.readback_memory, nullptr);
        target_ = Target{};
    }

    bool create_target(game::Extent extent, std::string& error) {
        destroy_target();
        target_.extent = extent;
        const bool has_stencil = depth_format_ == VK_FORMAT_D24_UNORM_S8_UINT;
        VkImageAspectFlags depth_aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
        if (has_stencil) {
            depth_aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
        }
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(extent.width) * extent.height * 4U;
        if (!make_image(extent, kColourFormat,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT, target_.scene, target_.scene_memory,
                        target_.scene_view)
            || !make_image(extent, kColourFormat,
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT, target_.colour, target_.colour_memory,
                           target_.colour_view)
            || !make_image(extent, depth_format_, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                           depth_aspect, target_.depth, target_.depth_memory, target_.depth_view)
            || !make_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, target_.readback,
                            target_.readback_memory, target_.mapped)) {
            error = "could not allocate a " + std::to_string(extent.width) + "x"
                    + std::to_string(extent.height) + " render target on " + device_name_;
            destroy_target();
            return false;
        }
        const auto framebuffer
            = [&](VkRenderPass pass, std::span<const VkImageView> views, VkFramebuffer& out) {
                  VkFramebufferCreateInfo info{};
                  info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
                  info.renderPass = pass;
                  info.attachmentCount = static_cast<std::uint32_t>(views.size());
                  info.pAttachments = views.data();
                  info.width = extent.width;
                  info.height = extent.height;
                  info.layers = 1;
                  if (vkCreateFramebuffer(device_, &info, nullptr, &out) != VK_SUCCESS) {
                      out = VK_NULL_HANDLE;
                      return false;
                  }
                  return true;
              };
        const std::array<VkImageView, 2> world_views{target_.scene_view, target_.depth_view};
        const std::array<VkImageView, 1> post_views{target_.colour_view};
        const std::array<VkImageView, 2> hud_views{target_.colour_view, target_.depth_view};
        if (!framebuffer(render_pass_, world_views, target_.scene_framebuffer)
            || !framebuffer(post_pass_, post_views, target_.post_framebuffer)
            || !framebuffer(hud_pass_, hud_views, target_.hud_framebuffer)) {
            error = "vkCreateFramebuffer failed";
            destroy_target();
            return false;
        }

        // Point the filter at the new scene image. Nothing is in flight: destroy_target waited.
        const VkDescriptorImageInfo scene_info{VK_NULL_HANDLE, target_.scene_view,
                                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = post_set_;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        write.pImageInfo = &scene_info;
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
        return true;
    }

    // ---- per frame

    std::vector<std::uint8_t> render_frame(const FrameRequest& request, FrameStats& stats) {
        const game::Extent extent = request.extent;
        if (extent.width == 0 || extent.height == 0 || extent.width > max_dimension_
            || extent.height > max_dimension_) {
            std::cerr << "protekt render: unusable extent " << extent.width << "x" << extent.height
                      << '\n';
            return {};
        }
        if (target_.scene_framebuffer == VK_NULL_HANDLE || target_.extent.width != extent.width
            || target_.extent.height != extent.height) {
            std::string error;
            if (!create_target(extent, error)) {
                std::cerr << "protekt render: " << error << '\n';
                return {};
            }
        }

        static_assert(sizeof(Vertex) == sizeof(game::SceneVertex),
                      "a caller-supplied Scene's vertices are uploaded byte-for-byte as Vertex");

        std::uint32_t vertex_count = 0;
        std::uint32_t world_count = 0; ///< the leading vertices drawn before the filter
        Mat4 mvp{};
        if (request.scene != nullptr) {
            vertex_count = static_cast<std::uint32_t>(request.scene->vertices.size());
            if (vertex_count > 0) {
                std::string resize_error;
                if (!ensure_vertex_capacity(sizeof(Vertex) * vertex_count, resize_error)) {
                    std::cerr << "protekt render: " << resize_error << '\n';
                    return {};
                }
                std::memcpy(vertex_mapped_, request.scene->vertices.data(),
                            sizeof(Vertex) * vertex_count);
            }
            mvp = request.scene->view_projection;
            // Whole triangles only: a boundary that cuts one would regroup every vertex after it.
            world_count = static_cast<std::uint32_t>(
                std::min<std::size_t>(request.scene->world_vertex_count, vertex_count) / 3U * 3U);
        } else {
            const std::vector<Vertex> built_in = build_scene(request.seed);
            vertex_count = static_cast<std::uint32_t>(built_in.size());
            std::memcpy(vertex_mapped_, built_in.data(), built_in.size() * sizeof(Vertex));
            mvp = view_projection(extent);
            world_count = vertex_count;
        }

        vkResetCommandBuffer(cmd_, 0);
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd_, &begin);

        const VkViewport viewport{
            0.0F, 0.0F, static_cast<float>(extent.width), static_cast<float>(extent.height),
            0.0F, 1.0F};
        const VkRect2D scissor{{0, 0}, {extent.width, extent.height}};
        const auto begin_pass = [&](VkRenderPass render_pass, VkFramebuffer framebuffer,
                                    std::span<const VkClearValue> clears) {
            VkRenderPassBeginInfo pass{};
            pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            pass.renderPass = render_pass;
            pass.framebuffer = framebuffer;
            pass.renderArea = {{0, 0}, {extent.width, extent.height}};
            pass.clearValueCount = static_cast<std::uint32_t>(clears.size());
            pass.pClearValues = clears.data();
            vkCmdBeginRenderPass(cmd_, &pass, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdSetViewport(cmd_, 0, 1, &viewport);
            vkCmdSetScissor(cmd_, 0, 1, &scissor);
        };
        // Both scene passes draw with the one pipeline, matrix and fog; only the vertex range
        // differs.
        const auto draw_scene = [&](std::uint32_t first, std::uint32_t count) {
            vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
            vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1,
                                    &descriptor_set_, 0, nullptr);
            vkCmdPushConstants(cmd_, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Mat4),
                               mvp.data());
            // The fog is the sky's own horizon ring: one source, never restated here.
            const Vec3& fog_colour = game::kSkyRingColours.at(game::kSkyHorizonRing);
            vkCmdPushConstants(cmd_, pipeline_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(Mat4),
                               sizeof(Vec3), fog_colour.data());
            const VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd_, 0, 1, &vertex_buffer_, &offset);
            vkCmdDraw(cmd_, count, 1, first, 0);
            ++stats.draw_calls;
        };

        // 1. The world, into the scene image.
        std::array<VkClearValue, 2> clears{};
        clears[0].color = {{kSky[0], kSky[1], kSky[2], 1.0F}};
        clears[1].depthStencil = {1.0F, 0};
        begin_pass(render_pass_, target_.scene_framebuffer, clears);
        if (world_count > 0) {
            draw_scene(0, world_count);
        }
        vkCmdEndRenderPass(cmd_);

        // 2. The filter: one fullscreen triangle that reads the scene image and writes the frame.
        begin_pass(post_pass_, target_.post_framebuffer, {});
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, post_pipeline_);
        vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, post_layout_, 0, 1,
                                &post_set_, 0, nullptr);
        const PostPush post_push{kFxaaSettings.edge_threshold, kFxaaSettings.edge_threshold_min,
                                 kFxaaSettings.subpixel_blend, fxaa_ ? 1U : 0U};
        vkCmdPushConstants(cmd_, post_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PostPush),
                           &post_push);
        vkCmdDraw(cmd_, 3, 1, 0, 0);
        ++stats.draw_calls;
        vkCmdEndRenderPass(cmd_);

        // 3. The HUD, over the filtered frame and against the world's depth.
        begin_pass(hud_pass_, target_.hud_framebuffer, {});
        if (vertex_count > world_count) {
            draw_scene(world_count, vertex_count - world_count);
        }
        vkCmdEndRenderPass(cmd_);

        // The HUD pass leaves the colour image in TRANSFER_SRC_OPTIMAL.
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {extent.width, extent.height, 1};
        vkCmdCopyImageToBuffer(cmd_, target_.colour, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               target_.readback, 1, &copy);
        VkBufferMemoryBarrier to_host{};
        to_host.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        to_host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        to_host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_host.buffer = target_.readback;
        to_host.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0,
                             nullptr, 1, &to_host, 0, nullptr);
        if (vkEndCommandBuffer(cmd_) != VK_SUCCESS) {
            return {};
        }

        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd_;
        vkResetFences(device_, 1, &fence_);
        if (vkQueueSubmit(queue_, 1, &submit, fence_) != VK_SUCCESS
            || vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
            return {};
        }

        const std::size_t size = static_cast<std::size_t>(extent.width) * extent.height * 4U;
        std::vector<std::uint8_t> pixels(size);
        std::memcpy(pixels.data(), target_.mapped, size);
        stats.triangles = kVertexCount / 3U;
        return pixels;
    }

    void destroy() noexcept {
        if (device_ != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device_);
            destroy_target();
            if (vertex_mapped_ != nullptr) {
                vkUnmapMemory(device_, vertex_memory_);
            }
            vkDestroyBuffer(device_, vertex_buffer_, nullptr);
            vkFreeMemory(device_, vertex_memory_, nullptr);
            vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
            vkDestroySampler(device_, sampler_, nullptr);
            vkDestroyImageView(device_, texture_view_, nullptr);
            vkDestroyImage(device_, texture_, nullptr);
            vkFreeMemory(device_, texture_memory_, nullptr);
            vkDestroyFence(device_, fence_, nullptr);
            vkDestroyCommandPool(device_, pool_, nullptr);
            vkDestroyPipeline(device_, pipeline_, nullptr);
            vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
            vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
            vkDestroyShaderModule(device_, vert_, nullptr);
            vkDestroyShaderModule(device_, frag_, nullptr);
            vkDestroyPipeline(device_, post_pipeline_, nullptr);
            vkDestroyPipelineLayout(device_, post_layout_, nullptr);
            vkDestroyDescriptorPool(device_, post_pool_, nullptr);
            vkDestroyDescriptorSetLayout(device_, post_set_layout_, nullptr);
            vkDestroyShaderModule(device_, post_vert_, nullptr);
            vkDestroyShaderModule(device_, post_frag_, nullptr);
            vkDestroyRenderPass(device_, hud_pass_, nullptr);
            vkDestroyRenderPass(device_, post_pass_, nullptr);
            vkDestroyRenderPass(device_, render_pass_, nullptr);
            vkDestroyDevice(device_, nullptr);
            device_ = VK_NULL_HANDLE;
        }
        if (messenger_ != VK_NULL_HANDLE) {
            destroy_messenger_(instance_, messenger_, nullptr);
            messenger_ = VK_NULL_HANDLE;
        }
        if (instance_ != VK_NULL_HANDLE) {
            vkDestroyInstance(instance_, nullptr);
            instance_ = VK_NULL_HANDLE;
        }
    }

    game::Extent extent_{};
    std::string device_name_;
    std::atomic<std::uint32_t> validation_count_{0};

    VkInstance instance_{};
    VkDebugUtilsMessengerEXT messenger_{};
    PFN_vkDestroyDebugUtilsMessengerEXT destroy_messenger_{};
    VkPhysicalDevice physical_{};
    VkPhysicalDeviceMemoryProperties memory_{};
    std::uint32_t queue_family_{};
    std::uint32_t max_dimension_{};
    VkDevice device_{};
    VkQueue queue_{};
    VkFormat depth_format_{VK_FORMAT_UNDEFINED};

    bool fxaa_{true};
    VkRenderPass render_pass_{}; ///< the world
    VkRenderPass post_pass_{};   ///< the filter
    VkRenderPass hud_pass_{};    ///< the HUD, compatible with `render_pass_`
    VkShaderModule post_vert_{};
    VkShaderModule post_frag_{};
    VkDescriptorSetLayout post_set_layout_{};
    VkPipelineLayout post_layout_{};
    VkPipeline post_pipeline_{};
    VkDescriptorPool post_pool_{};
    VkDescriptorSet post_set_{};
    VkShaderModule vert_{};
    VkShaderModule frag_{};
    VkDescriptorSetLayout set_layout_{};
    VkPipelineLayout pipeline_layout_{};
    VkPipeline pipeline_{};

    std::vector<std::uint8_t> atlas_rgba_;
    std::uint32_t atlas_side_{0};
    VkImage texture_{};
    VkDeviceMemory texture_memory_{};
    VkImageView texture_view_{};
    VkSampler sampler_{};
    VkDescriptorPool descriptor_pool_{};
    VkDescriptorSet descriptor_set_{};

    VkCommandPool pool_{};
    VkCommandBuffer cmd_{};
    VkFence fence_{};
    VkBuffer vertex_buffer_{};
    VkDeviceMemory vertex_memory_{};
    void* vertex_mapped_{};
    VkDeviceSize vertex_capacity_bytes_{}; ///< how much the buffer holds, so it can grow
    Target target_;
};

} // namespace

std::vector<DeviceInfo> devices(std::string& error) {
    try {
        VkInstance instance = make_plain_instance(error);
        if (instance == VK_NULL_HANDLE) {
            return {};
        }
        std::vector<DeviceInfo> list;
        for (const Candidate& c : enumerate_candidates(instance, error)) {
            list.push_back(c.info);
        }
        vkDestroyInstance(instance, nullptr);
        return list;
    } catch (const std::exception& e) {
        error = e.what();
    } catch (...) {
        error = "unknown exception while enumerating Vulkan devices";
    }
    return {};
}

std::unique_ptr<Renderer> create(const CreateOptions& options, std::string& error) {
    try {
        auto renderer = std::make_unique<VulkanRenderer>();
        if (!renderer->init(options, error)) {
            return nullptr;
        }
        return renderer;
    } catch (const std::exception& e) {
        error = e.what();
    } catch (...) {
        error = "unknown exception while creating the Vulkan renderer";
    }
    return nullptr;
}

} // namespace protekt::render
