// The only translation unit in the project that includes <vulkan/...>.
//
// Headless: one colour image and one depth image, a render pass, one draw call, and a copy of the
// colour image into a host-visible buffer. No window, no surface, no swapchain.
#include "protekt/render/renderer.hpp"

#include "protekt/math/math.hpp"

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
};

constexpr std::uint32_t kUnitVertices = 36;
constexpr std::uint32_t kGroundVertices = 6;
constexpr std::uint32_t kVertexCount = kUnitVertices + kGroundVertices;

constexpr Vec3 kSky{0.53F, 0.81F, 0.92F};
constexpr Vec3 kUnit{0.85F, 0.15F, 0.15F};

std::uint64_t splitmix64(std::uint64_t& state) {
    state += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
}

/// Green-dominant, so the ground can never collide with the (red) unit or the (blue) sky.
Vec3 ground_colour(std::uint64_t seed) {
    std::uint64_t state = seed;
    const auto unit_float = [&state] {
        return static_cast<float>(splitmix64(state) >> 40U) / static_cast<float>(1U << 24U);
    };
    return {0.10F + 0.25F * unit_float(), 0.35F + 0.40F * unit_float(),
            0.10F + 0.25F * unit_float()};
}

void emit_triangle(std::vector<Vertex>& out, const Vec3& a, const Vec3& b, const Vec3& c,
                   const Vec3& colour) {
    const auto emit
        = [&out, &colour](const Vec3& position) { out.push_back(Vertex{position, colour}); };
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
Mat4 view_projection(Extent extent) {
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
        return create_device(error) && create_render_pass(error) && create_pipeline(error)
               && create_frame_objects(error) && create_target(options.extent, error);
    }

    [[nodiscard]] Extent extent() const noexcept override { return extent_; }
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
        Extent extent{};
        VkImage colour{};
        VkDeviceMemory colour_memory{};
        VkImageView colour_view{};
        VkImage depth{};
        VkDeviceMemory depth_memory{};
        VkImageView depth_view{};
        VkFramebuffer framebuffer{};
        VkBuffer readback{};
        VkDeviceMemory readback_memory{};
        void* mapped{};
    };

    static constexpr VkFormat kColourFormat = VK_FORMAT_R8G8B8A8_UNORM;

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

    bool create_render_pass(std::string& error) {
        std::array<VkAttachmentDescription, 2> attachments{};
        attachments[0].format = kColourFormat;
        attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        attachments[1].format = depth_format_;
        attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkAttachmentReference colour_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference depth_ref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colour_ref;
        subpass.pDepthStencilAttachment = &depth_ref;

        std::array<VkSubpassDependency, 2> deps{};
        // Before: the previous frame's copy must be finished before this frame's clear.
        deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass = 0;
        deps[0].srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                               | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        deps[0].srcAccessMask = 0;
        deps[0].dstAccessMask
            = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        // After: the colour writes must be visible to the copy.
        deps[1].srcSubpass = 0;
        deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

        VkRenderPassCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        info.attachmentCount = static_cast<std::uint32_t>(attachments.size());
        info.pAttachments = attachments.data();
        info.subpassCount = 1;
        info.pSubpasses = &subpass;
        info.dependencyCount = static_cast<std::uint32_t>(deps.size());
        info.pDependencies = deps.data();
        if (vkCreateRenderPass(device_, &info, nullptr, &render_pass_) != VK_SUCCESS) {
            render_pass_ = VK_NULL_HANDLE;
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

        VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Mat4)};
        VkPipelineLayoutCreateInfo layout{};
        layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout.pushConstantRangeCount = 1;
        layout.pPushConstantRanges = &push;
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
        std::array<VkVertexInputAttributeDescription, 2> attributes{{
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
            {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, colour)},
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

    bool make_image(Extent extent, VkFormat format, VkImageUsageFlags usage,
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
        vkDestroyFramebuffer(device_, target_.framebuffer, nullptr);
        vkDestroyImageView(device_, target_.colour_view, nullptr);
        vkDestroyImageView(device_, target_.depth_view, nullptr);
        vkDestroyImage(device_, target_.colour, nullptr);
        vkDestroyImage(device_, target_.depth, nullptr);
        vkFreeMemory(device_, target_.colour_memory, nullptr);
        vkFreeMemory(device_, target_.depth_memory, nullptr);
        vkDestroyBuffer(device_, target_.readback, nullptr);
        vkFreeMemory(device_, target_.readback_memory, nullptr);
        target_ = Target{};
    }

    bool create_target(Extent extent, std::string& error) {
        destroy_target();
        target_.extent = extent;
        const bool has_stencil = depth_format_ == VK_FORMAT_D24_UNORM_S8_UINT;
        VkImageAspectFlags depth_aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
        if (has_stencil) {
            depth_aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
        }
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(extent.width) * extent.height * 4U;
        if (!make_image(extent, kColourFormat,
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
        const std::array<VkImageView, 2> views{target_.colour_view, target_.depth_view};
        VkFramebufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        info.renderPass = render_pass_;
        info.attachmentCount = static_cast<std::uint32_t>(views.size());
        info.pAttachments = views.data();
        info.width = extent.width;
        info.height = extent.height;
        info.layers = 1;
        if (vkCreateFramebuffer(device_, &info, nullptr, &target_.framebuffer) != VK_SUCCESS) {
            target_.framebuffer = VK_NULL_HANDLE;
            error = "vkCreateFramebuffer failed";
            destroy_target();
            return false;
        }
        return true;
    }

    // ---- per frame

    std::vector<std::uint8_t> render_frame(const FrameRequest& request, FrameStats& stats) {
        const Extent extent = request.extent;
        if (extent.width == 0 || extent.height == 0 || extent.width > max_dimension_
            || extent.height > max_dimension_) {
            std::cerr << "protekt render: unusable extent " << extent.width << "x" << extent.height
                      << '\n';
            return {};
        }
        if (target_.framebuffer == VK_NULL_HANDLE || target_.extent.width != extent.width
            || target_.extent.height != extent.height) {
            std::string error;
            if (!create_target(extent, error)) {
                std::cerr << "protekt render: " << error << '\n';
                return {};
            }
        }

        static_assert(sizeof(Vertex) == sizeof(SceneVertex),
                      "a caller-supplied Scene's vertices are uploaded byte-for-byte as Vertex");

        std::uint32_t vertex_count = 0;
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
        } else {
            const std::vector<Vertex> built_in = build_scene(request.seed);
            vertex_count = static_cast<std::uint32_t>(built_in.size());
            std::memcpy(vertex_mapped_, built_in.data(), built_in.size() * sizeof(Vertex));
            mvp = view_projection(extent);
        }

        vkResetCommandBuffer(cmd_, 0);
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd_, &begin);

        std::array<VkClearValue, 2> clears{};
        clears[0].color = {{kSky[0], kSky[1], kSky[2], 1.0F}};
        clears[1].depthStencil = {1.0F, 0};
        VkRenderPassBeginInfo pass{};
        pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        pass.renderPass = render_pass_;
        pass.framebuffer = target_.framebuffer;
        pass.renderArea = {{0, 0}, {extent.width, extent.height}};
        pass.clearValueCount = static_cast<std::uint32_t>(clears.size());
        pass.pClearValues = clears.data();
        vkCmdBeginRenderPass(cmd_, &pass, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        const VkViewport viewport{
            0.0F, 0.0F, static_cast<float>(extent.width), static_cast<float>(extent.height),
            0.0F, 1.0F};
        const VkRect2D scissor{{0, 0}, {extent.width, extent.height}};
        vkCmdSetViewport(cmd_, 0, 1, &viewport);
        vkCmdSetScissor(cmd_, 0, 1, &scissor);
        vkCmdPushConstants(cmd_, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Mat4),
                           mvp.data());
        if (vertex_count > 0) {
            const VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd_, 0, 1, &vertex_buffer_, &offset);
            vkCmdDraw(cmd_, vertex_count, 1, 0, 0);
        }
        vkCmdEndRenderPass(cmd_);

        // The render pass leaves the colour image in TRANSFER_SRC_OPTIMAL.
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
        stats.draw_calls = 1;
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
            vkDestroyFence(device_, fence_, nullptr);
            vkDestroyCommandPool(device_, pool_, nullptr);
            vkDestroyPipeline(device_, pipeline_, nullptr);
            vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
            vkDestroyShaderModule(device_, vert_, nullptr);
            vkDestroyShaderModule(device_, frag_, nullptr);
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

    Extent extent_{};
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

    VkRenderPass render_pass_{};
    VkShaderModule vert_{};
    VkShaderModule frag_{};
    VkPipelineLayout pipeline_layout_{};
    VkPipeline pipeline_{};

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
