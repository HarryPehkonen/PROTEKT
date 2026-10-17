// The window: a swapchain over the SAME renderer the gate box tests.
//
// This translation unit is the only one that includes <SDL3/SDL.h>.
//
// WHY IT PRESENTS A BLIT RATHER THAN DRAWING STRAIGHT INTO THE SWAPCHAIN. The frame you see is
// produced by `render::create()` -- the offscreen renderer, the one with 19 tests, the one the
// whole milestone is verified against -- and copied into the swapchain image. That is deliberate,
// and it buys three things at the cost of one memcpy per frame:
//
//   1. what is on screen is provably what the suite checks, so a captured window frame and a
//      captured headless frame are the same bytes by construction, not by coincidence;
//   2. the swapchain code carries no pipeline, no render pass and no vertex buffers of its own,
//      so there is exactly one place in the project that draws geometry;
//   3. a window that cannot open is a clean refusal, because the failure paths are all on the
//      window's side of the seam.
//
// The cost is real: a full CPU readback plus re-upload each frame, and the window does not
// exercise the shaders directly. Rendering into the swapchain images with their own pipeline is
// the follow-up (docs/PLAN.md, M3b) -- an optimisation, not a correctness fix.
#include "protekt/render/window.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace protekt::render {

namespace {

/// SDL is process-wide, so its lifetime is refcounted rather than owned by any one window.
class SdlLifetime {
public:
    SdlLifetime() noexcept : active_(SDL_Init(SDL_INIT_VIDEO)) {}
    ~SdlLifetime() {
        if (active_) {
            SDL_Quit();
        }
    }
    SdlLifetime(const SdlLifetime&) = delete;
    SdlLifetime& operator=(const SdlLifetime&) = delete;
    SdlLifetime(SdlLifetime&&) = delete;
    SdlLifetime& operator=(SdlLifetime&&) = delete;
    [[nodiscard]] bool active() const noexcept { return active_; }

private:
    bool active_{false};
};

/// The validation findings this process has seen. A function-local static rather than a global:
/// the counter is an implementation detail of this translation unit, and nothing outside it has
/// any business reaching in.
std::atomic<std::uint32_t>& validation_counter() noexcept {
    static std::atomic<std::uint32_t> counter{0};
    return counter;
}

VKAPI_ATTR VkBool32 VKAPI_CALL window_debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT /*types*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void* /*user*/) {
    if ((severity
         & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT
            | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT))
        != 0U) {
        validation_counter().fetch_add(1U);
    }
    (void)data;
    return VK_FALSE;
}

std::string lowercase(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

bool has_layer(std::string_view wanted) {
    std::uint32_t count = 0;
    if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS) {
        return false;
    }
    std::vector<VkLayerProperties> layers(count);
    if (vkEnumerateInstanceLayerProperties(&count, layers.data()) != VK_SUCCESS) {
        return false;
    }
    return std::any_of(layers.begin(), layers.end(), [wanted](const VkLayerProperties& layer) {
        return std::string_view{static_cast<const char*>(layer.layerName)} == wanted;
    });
}

/// One device that can both draw and present to our surface.
struct PresentDevice {
    VkPhysicalDevice device{VK_NULL_HANDLE};
    std::uint32_t queue_family{0};
    std::string name;
    bool discrete{false};
};

std::vector<PresentDevice> present_devices(VkInstance instance, VkSurfaceKHR surface) {
    std::vector<PresentDevice> found;
    std::uint32_t count = 0;
    if (vkEnumeratePhysicalDevices(instance, &count, nullptr) != VK_SUCCESS || count == 0) {
        return found;
    }
    std::vector<VkPhysicalDevice> handles(count);
    const VkResult r = vkEnumeratePhysicalDevices(instance, &count, handles.data());
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) {
        return found;
    }
    handles.resize(count);

    for (VkPhysicalDevice handle : handles) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(handle, &properties);

        std::uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(handle, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(handle, &family_count, families.data());

        for (std::uint32_t index = 0; index < family_count; ++index) {
            if ((families.at(index).queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0U) {
                continue;
            }
            VkBool32 present_supported = VK_FALSE;
            if (vkGetPhysicalDeviceSurfaceSupportKHR(handle, index, surface, &present_supported)
                    != VK_SUCCESS
                || present_supported == VK_FALSE) {
                continue;
            }
            found.push_back(PresentDevice{
                handle, index, std::string{static_cast<const char*>(properties.deviceName)},
                properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU});
            break; // one queue family per device is enough
        }
    }
    // Discrete first, then by name, so the choice does not depend on enumeration order -- on an
    // Optimus laptop the discrete GPU is NOT device 0.
    std::stable_sort(found.begin(), found.end(),
                     [](const PresentDevice& a, const PresentDevice& b) {
                         if (a.discrete != b.discrete) {
                             return a.discrete;
                         }
                         return a.name < b.name;
                     });
    return found;
}

} // namespace

namespace {

class VulkanWindow final : public Window {
public:
    VulkanWindow() = default;
    ~VulkanWindow() override { destroy(); }
    VulkanWindow(const VulkanWindow&) = delete;
    VulkanWindow& operator=(const VulkanWindow&) = delete;
    VulkanWindow(VulkanWindow&&) = delete;
    VulkanWindow& operator=(VulkanWindow&&) = delete;

    bool init(const WindowOptions& options, std::string& error);
    [[nodiscard]] bool pump_events() override;
    [[nodiscard]] const input::InputFrame& input() const noexcept override { return input_; }
    [[nodiscard]] Extent extent() const noexcept override { return extent_; }
    [[nodiscard]] std::string_view device_name() const noexcept override { return device_name_; }
    void present(const Scene& scene) override;
    [[nodiscard]] std::vector<std::uint8_t> capture() const override { return last_frame_; }
    [[nodiscard]] std::uint32_t validation_errors() const noexcept override {
        return validation_counter().load();
    }

private:
    void destroy();
    [[nodiscard]] bool create_instance(const WindowOptions& options, std::string& error);
    [[nodiscard]] bool create_surface(std::string& error);
    [[nodiscard]] bool create_device(const WindowOptions& options, std::string& error);
    [[nodiscard]] bool create_swapchain(std::string& error);
    [[nodiscard]] bool create_staging(std::string& error);
    void destroy_swapchain();
    [[nodiscard]] bool acquire_and_blit(const std::vector<std::uint8_t>& rgba, std::string& error);
    void rebuild_offscreen(std::string& error);

    SdlLifetime sdl_{};
    SDL_Window* window_{nullptr};
    VkInstance instance_{VK_NULL_HANDLE};
    VkDebugUtilsMessengerEXT messenger_{};
    PFN_vkDestroyDebugUtilsMessengerEXT destroy_messenger_{nullptr};
    VkSurfaceKHR surface_{VK_NULL_HANDLE};
    VkPhysicalDevice physical_{VK_NULL_HANDLE};
    VkDevice device_{VK_NULL_HANDLE};
    VkQueue queue_{VK_NULL_HANDLE};
    std::uint32_t queue_family_{0};
    std::string device_name_;
    VkSwapchainKHR swapchain_{VK_NULL_HANDLE};
    VkFormat format_{VK_FORMAT_UNDEFINED};
    Extent extent_{};
    std::vector<VkImage> images_{};
    std::vector<VkImageView> views_{};
    VkCommandPool command_pool_{VK_NULL_HANDLE};
    VkCommandBuffer command_{VK_NULL_HANDLE};
    VkFence frame_fence_{VK_NULL_HANDLE};
    VkSemaphore image_ready_{VK_NULL_HANDLE};
    std::vector<VkSemaphore> render_done_{};
    VkBuffer staging_{VK_NULL_HANDLE};
    VkDeviceMemory staging_memory_{VK_NULL_HANDLE};
    void* staging_mapped_{nullptr};
    VkDeviceSize staging_size_{0};
    std::unique_ptr<Renderer> offscreen_{};
    std::vector<std::uint8_t> last_frame_{};
    input::InputFrame input_{}; ///< what the user did this frame, cleared by pump_events
    std::uint64_t frames_presented_{0};
    bool validation_{true};
    bool resize_pending_{false};
};

bool VulkanWindow::init(const WindowOptions& options, std::string& error) {
    if (options.extent.width == 0 || options.extent.height == 0) {
        error = "refusing to create a window with an empty extent";
        return false;
    }
    validation_ = options.validation;
    extent_ = options.extent;

    if (!sdl_.active()) {
        error = std::string("SDL_Init failed: ") + SDL_GetError();
        return false;
    }
    window_ = SDL_CreateWindow(options.title.c_str(), static_cast<int>(options.extent.width),
                               static_cast<int>(options.extent.height),
                               SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (window_ == nullptr) {
        error = std::string("SDL_CreateWindow failed: ") + SDL_GetError();
        return false;
    }
    if (!create_instance(options, error) || !create_surface(error) || !create_device(options, error)
        || !create_swapchain(error) || !create_staging(error)) {
        return false;
    }
    rebuild_offscreen(error);
    if (offscreen_ == nullptr) {
        return false;
    }
    return true;
}

bool VulkanWindow::create_instance(const WindowOptions& options, std::string& error) {
    std::uint32_t sdl_count = 0;
    char const* const* sdl_extensions = SDL_Vulkan_GetInstanceExtensions(&sdl_count);
    if (sdl_extensions == nullptr || sdl_count == 0) {
        error = std::string("SDL_Vulkan_GetInstanceExtensions failed: ") + SDL_GetError();
        return false;
    }
    const std::span<char const* const> sdl_names{sdl_extensions, sdl_count};
    std::vector<const char*> extensions;
    extensions.reserve(sdl_count);
    for (std::uint32_t i = 0; i < sdl_count; ++i) {
        extensions.push_back(sdl_names[i]);
    }

    std::vector<const char*> layers;
    if (options.validation && has_layer(kValidationLayer)) {
        layers.push_back(kValidationLayer);
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "PROTEKT";
    app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app.pEngineName = "protekt";
    app.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    app.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    info.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
    info.ppEnabledLayerNames = layers.empty() ? nullptr : layers.data();

    const VkResult r = vkCreateInstance(&info, nullptr, &instance_);
    if (r != VK_SUCCESS) {
        error = "vkCreateInstance failed (" + std::to_string(r)
                + "): no Vulkan loader or driver is usable";
        return false;
    }

    if (!layers.empty()) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): see the note below.
        const auto create_fn = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): see above.
        destroy_messenger_ = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
        if (create_fn != nullptr) {
            VkDebugUtilsMessengerCreateInfoEXT messenger{};
            messenger.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            messenger.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT
                                        | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
            messenger.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                                    | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                                    | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            messenger.pfnUserCallback = window_debug_callback;
            if (create_fn(instance_, &messenger, nullptr, &messenger_) != VK_SUCCESS) {
                messenger_ = VK_NULL_HANDLE;
            }
        }
    }
    return true;
}

bool VulkanWindow::create_surface(std::string& error) {
    if (!SDL_Vulkan_CreateSurface(window_, instance_, nullptr, &surface_)) {
        error = std::string("SDL_Vulkan_CreateSurface failed: ") + SDL_GetError();
        return false;
    }
    return true;
}

bool VulkanWindow::create_device(const WindowOptions& options, std::string& error) {
    const std::vector<PresentDevice> devices = present_devices(instance_, surface_);
    if (devices.empty()) {
        error = "no Vulkan device can present to this window (none has a graphics queue with "
                "surface support)";
        return false;
    }
    const PresentDevice* chosen = &devices.front();
    if (!options.device_name.empty()) {
        const std::string wanted = lowercase(options.device_name);
        const auto match = std::find_if(
            devices.begin(), devices.end(), [&wanted](const PresentDevice& candidate) {
                return lowercase(candidate.name).find(wanted) != std::string::npos;
            });
        if (match == devices.end()) {
            error = "no present-capable device matches '" + options.device_name + "'; available: ";
            for (const PresentDevice& candidate : devices) {
                error += candidate.name + "; ";
            }
            return false;
        }
        chosen = &*match;
    }
    physical_ = chosen->device;
    queue_family_ = chosen->queue_family;
    device_name_ = chosen->name;

    const float priority = 1.0F;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = queue_family_;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;

    const char* swapchain_extension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue_info;
    info.enabledExtensionCount = 1;
    info.ppEnabledExtensionNames = &swapchain_extension;

    const VkResult r = vkCreateDevice(physical_, &info, nullptr, &device_);
    if (r != VK_SUCCESS) {
        error = "vkCreateDevice failed (" + std::to_string(r) + ") on " + device_name_;
        return false;
    }
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
    return true;
}

bool VulkanWindow::create_swapchain(std::string& error) {
    VkSurfaceCapabilitiesKHR caps{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps) != VK_SUCCESS) {
        error = "vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed";
        return false;
    }

    std::uint32_t format_count = 0;
    if (vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &format_count, nullptr)
            != VK_SUCCESS
        || format_count == 0) {
        error = "the surface offers no formats";
        return false;
    }
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &format_count, formats.data());
    VkSurfaceFormatKHR chosen_format = formats.front();
    for (const VkSurfaceFormatKHR& candidate : formats) {
        if ((candidate.format == VK_FORMAT_B8G8R8A8_UNORM
             || candidate.format == VK_FORMAT_R8G8B8A8_UNORM)
            && candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen_format = candidate;
            break;
        }
    }
    format_ = chosen_format.format;

    // The window's pixel size is the authority when the surface lets us choose; a compositor
    // under Wayland or a HiDPI scale can otherwise disagree with what we asked for.
    int pixel_width = 0;
    int pixel_height = 0;
    if (SDL_GetWindowSizeInPixels(window_, &pixel_width, &pixel_height) && pixel_width > 0
        && pixel_height > 0) {
        extent_.width = static_cast<std::uint32_t>(pixel_width);
        extent_.height = static_cast<std::uint32_t>(pixel_height);
    }
    if (caps.currentExtent.width != UINT32_MAX) {
        extent_.width = caps.currentExtent.width;
        extent_.height = caps.currentExtent.height;
    }
    extent_.width = std::clamp(extent_.width, caps.minImageExtent.width, caps.maxImageExtent.width);
    extent_.height
        = std::clamp(extent_.height, caps.minImageExtent.height, caps.maxImageExtent.height);

    std::uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && image_count > caps.maxImageCount) {
        image_count = caps.maxImageCount;
    }

    VkSwapchainCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = surface_;
    info.minImageCount = image_count;
    info.imageFormat = chosen_format.format;
    info.imageColorSpace = chosen_format.colorSpace;
    info.imageExtent = VkExtent2D{extent_.width, extent_.height};
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    info.clipped = VK_TRUE;

    const VkResult r = vkCreateSwapchainKHR(device_, &info, nullptr, &swapchain_);
    if (r != VK_SUCCESS) {
        error = "vkCreateSwapchainKHR failed (" + std::to_string(r) + ")";
        return false;
    }

    std::uint32_t actual = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &actual, nullptr);
    images_.resize(actual);
    vkGetSwapchainImagesKHR(device_, swapchain_, &actual, images_.data());

    views_.resize(images_.size(), VK_NULL_HANDLE);
    for (std::size_t i = 0; i < images_.size(); ++i) {
        VkImageViewCreateInfo view{};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image = images_.at(i);
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = format_;
        view.subresourceRange = VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(device_, &view, nullptr, &views_.at(i)) != VK_SUCCESS) {
            error = "vkCreateImageView failed for swapchain image " + std::to_string(i);
            return false;
        }
    }

    if (command_pool_ == VK_NULL_HANDLE) {
        VkCommandPoolCreateInfo pool{};
        pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool.queueFamilyIndex = queue_family_;
        if (vkCreateCommandPool(device_, &pool, nullptr, &command_pool_) != VK_SUCCESS) {
            error = "vkCreateCommandPool failed";
            return false;
        }
        VkCommandBufferAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocate.commandPool = command_pool_;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(device_, &allocate, &command_) != VK_SUCCESS) {
            error = "vkAllocateCommandBuffers failed";
            return false;
        }
    }

    if (frame_fence_ == VK_NULL_HANDLE) {
        VkFenceCreateInfo fence{};
        fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (vkCreateFence(device_, &fence, nullptr, &frame_fence_) != VK_SUCCESS) {
            error = "vkCreateFence failed";
            return false;
        }
        VkSemaphoreCreateInfo semaphore{};
        semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (vkCreateSemaphore(device_, &semaphore, nullptr, &image_ready_) != VK_SUCCESS) {
            error = "vkCreateSemaphore failed";
            return false;
        }
    }

    // One "render finished" semaphore per swapchain image: a semaphore may not be signalled again
    // while a previous present that used it is still pending.
    for (VkSemaphore& semaphore : render_done_) {
        vkDestroySemaphore(device_, semaphore, nullptr);
    }
    render_done_.assign(images_.size(), VK_NULL_HANDLE);
    for (VkSemaphore& semaphore : render_done_) {
        VkSemaphoreCreateInfo semaphore_info{};
        semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (vkCreateSemaphore(device_, &semaphore_info, nullptr, &semaphore) != VK_SUCCESS) {
            error = "vkCreateSemaphore failed for a swapchain image";
            return false;
        }
    }
    return true;
}

void VulkanWindow::destroy_swapchain() {
    for (VkSemaphore semaphore : render_done_) {
        if (semaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_, semaphore, nullptr);
        }
    }
    render_done_.clear();
    for (VkImageView view : views_) {
        if (view != VK_NULL_HANDLE) {
            vkDestroyImageView(device_, view, nullptr);
        }
    }
    views_.clear();
    images_.clear();
    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

bool VulkanWindow::create_staging(std::string& error) {
    staging_size_ = static_cast<VkDeviceSize>(extent_.width) * extent_.height * 4U;
    VkBufferCreateInfo buffer{};
    buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer.size = staging_size_;
    buffer.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device_, &buffer, nullptr, &staging_) != VK_SUCCESS) {
        error = "vkCreateBuffer failed for the staging buffer";
        return false;
    }
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device_, staging_, &requirements);

    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(physical_, &memory);
    // Copied out of the driver's fixed-size array into a std::array: the checks want checked
    // access, and std::span has no checked access before C++26.
    std::array<VkMemoryType, VK_MAX_MEMORY_TYPES> types{};
    std::copy(std::begin(memory.memoryTypes), std::end(memory.memoryTypes), types.begin());
    std::uint32_t type_index = UINT32_MAX;
    for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        const bool host_visible
            = (types.at(i).propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0U;
        const bool host_coherent
            = (types.at(i).propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0U;
        if ((requirements.memoryTypeBits & (1U << i)) != 0U && host_visible && host_coherent) {
            type_index = i;
            break;
        }
    }
    if (type_index == UINT32_MAX) {
        error = "no host-visible, host-coherent memory type for the staging buffer";
        return false;
    }
    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type_index;
    if (vkAllocateMemory(device_, &allocate, nullptr, &staging_memory_) != VK_SUCCESS) {
        error = "vkAllocateMemory failed for the staging buffer";
        return false;
    }
    vkBindBufferMemory(device_, staging_, staging_memory_, 0);
    if (vkMapMemory(device_, staging_memory_, 0, staging_size_, 0, &staging_mapped_)
        != VK_SUCCESS) {
        error = "vkMapMemory failed for the staging buffer";
        return false;
    }
    return true;
}

void VulkanWindow::rebuild_offscreen(std::string& error) {
    CreateOptions options;
    options.device_name = device_name_;
    options.validation = validation_;
    options.extent = extent_;
    // Qualified: inside this class, an unqualified `create` is Window::create.
    offscreen_ = protekt::render::create(options, error);
}

bool VulkanWindow::pump_events() {
    input_.clear();
    // SDL reports mouse positions in WINDOW coordinates; the framebuffer is in pixels. On a HiDPI
    // display the two differ, so scale by pixels-per-window-unit before anything leaves this file.
    int window_w = 0;
    int window_h = 0;
    SDL_GetWindowSize(window_, &window_w, &window_h);
    const float scale_x
        = window_w > 0 ? static_cast<float>(extent_.width) / static_cast<float>(window_w) : 1.0F;
    const float scale_y
        = window_h > 0 ? static_cast<float>(extent_.height) / static_cast<float>(window_h) : 1.0F;
    const auto button_of = [](std::uint8_t button) {
        switch (button) {
        case SDL_BUTTON_RIGHT:
            return input::MouseButton::Right;
        case SDL_BUTTON_MIDDLE:
            return input::MouseButton::Middle;
        default:
            return input::MouseButton::Left;
        }
    };

    SDL_Event event{};
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_EVENT_QUIT:
            input_.quit = true;
            return false;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            input_.clicks.push_back(input::Click{
                static_cast<int>(std::floor(event.button.x * scale_x)),
                static_cast<int>(std::floor(event.button.y * scale_y)),
                button_of(event.button.button), event.type == SDL_EVENT_MOUSE_BUTTON_DOWN,
                (SDL_GetModState() & SDL_KMOD_SHIFT) != 0});
            break;
        case SDL_EVENT_MOUSE_MOTION:
            input_.mouse_dx += event.motion.xrel * scale_x;
            input_.mouse_dy += event.motion.yrel * scale_y;
            break;
        case SDL_EVENT_KEY_UP:
            input_.keys.push_back(input::KeyEvent{static_cast<std::int32_t>(event.key.scancode),
                                                  false, (event.key.mod & SDL_KMOD_SHIFT) != 0});
            break;
        case SDL_EVENT_KEY_DOWN:
            // Escape is reported like any other key and NOT handled here: the window does not know
            // what Escape means to the game (deselect), and a player who pressed it to clear a
            // selection must not lose the battle.
            input_.keys.push_back(input::KeyEvent{static_cast<std::int32_t>(event.key.scancode),
                                                  true, (event.key.mod & SDL_KMOD_SHIFT) != 0});
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_RESIZED: {
            int width = 0;
            int height = 0;
            if (SDL_GetWindowSizeInPixels(window_, &width, &height) && width > 0 && height > 0) {
                extent_.width = static_cast<std::uint32_t>(width);
                extent_.height = static_cast<std::uint32_t>(height);
                resize_pending_ = true;
            }
            break;
        }
        default:
            break;
        }
    }
    return true;
}

bool VulkanWindow::acquire_and_blit(const std::vector<std::uint8_t>& rgba, std::string& error) {
    if (vkWaitForFences(device_, 1, &frame_fence_, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
        error = "vkWaitForFences failed";
        return false;
    }

    std::uint32_t index = 0;
    VkResult r = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, image_ready_,
                                       VK_NULL_HANDLE, &index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        destroy_swapchain();
        if (!create_swapchain(error) || !create_staging(error)) {
            return false;
        }
        rebuild_offscreen(error);
        return false; // this frame is dropped; the next one uses the new swapchain
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        error = "vkAcquireNextImageKHR failed (" + std::to_string(r) + ")";
        return false;
    }

    if (vkResetFences(device_, 1, &frame_fence_) != VK_SUCCESS) {
        error = "vkResetFences failed";
        return false;
    }

    std::memcpy(staging_mapped_, rgba.data(), static_cast<std::size_t>(staging_size_));

    if (vkResetCommandBuffer(command_, 0) != VK_SUCCESS) {
        error = "vkResetCommandBuffer failed";
        return false;
    }
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(command_, &begin) != VK_SUCCESS) {
        error = "vkBeginCommandBuffer failed";
        return false;
    }

    const auto barrier
        = [&](VkImageLayout from, VkImageLayout to, VkPipelineStageFlags src_stage,
              VkPipelineStageFlags dst_stage, VkAccessFlags src_access, VkAccessFlags dst_access) {
              VkImageMemoryBarrier transition{};
              transition.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
              transition.oldLayout = from;
              transition.newLayout = to;
              transition.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
              transition.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
              transition.image = images_.at(index);
              transition.subresourceRange
                  = VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
              transition.srcAccessMask = src_access;
              transition.dstAccessMask = dst_access;
              vkCmdPipelineBarrier(command_, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1,
                                   &transition);
          };

    // UNDEFINED on the first use of an image and PRESENT_SRC afterwards: telling the driver the
    // previous contents are not needed is what lets it discard them instead of preserving them.
    barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT);

    VkBufferImageCopy copy{};
    copy.bufferOffset = 0;
    copy.bufferRowLength = 0;
    copy.bufferImageHeight = 0;
    copy.imageSubresource = VkImageSubresourceLayers{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageOffset = VkOffset3D{0, 0, 0};
    copy.imageExtent = VkExtent3D{extent_.width, extent_.height, 1};
    vkCmdCopyBufferToImage(command_, staging_, images_.at(index),
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT, 0);

    if (vkEndCommandBuffer(command_) != VK_SUCCESS) {
        error = "vkEndCommandBuffer failed";
        return false;
    }

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    const std::array<VkSemaphore, 1> wait_semaphores{image_ready_};
    const std::array<VkPipelineStageFlags, 1> wait_stages{VK_PIPELINE_STAGE_TRANSFER_BIT};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = wait_semaphores.data();
    submit.pWaitDstStageMask = wait_stages.data();
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command_;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &render_done_.at(index);
    if (vkQueueSubmit(queue_, 1, &submit, frame_fence_) != VK_SUCCESS) {
        error = "vkQueueSubmit failed";
        return false;
    }

    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &render_done_.at(index);
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain_;
    present.pImageIndices = &index;
    r = vkQueuePresentKHR(queue_, &present);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        destroy_swapchain();
        if (!create_swapchain(error) || !create_staging(error)) {
            return false;
        }
        rebuild_offscreen(error);
        return true;
    }
    if (r != VK_SUCCESS) {
        error = "vkQueuePresentKHR failed (" + std::to_string(r) + ")";
        return false;
    }
    ++frames_presented_;
    return true;
}

void VulkanWindow::present(const Scene& scene) {
    if (resize_pending_) {
        vkDeviceWaitIdle(device_);
        destroy_swapchain();
        std::string error;
        if (!create_swapchain(error) || !create_staging(error)) {
            std::cerr << "protekt: resize failed: " << error << '\n';
            return;
        }
        rebuild_offscreen(error);
        resize_pending_ = false;
    }

    FrameRequest request;
    request.extent = extent_;
    request.seed = 0;
    request.tick = 0;
    request.scene = &scene;
    FrameStats stats{};
    last_frame_ = offscreen_->render(request, stats);
    if (last_frame_.empty()) {
        return;
    }

    std::string error;
    if (!acquire_and_blit(last_frame_, error) && !error.empty()) {
        std::cerr << "protekt: present failed: " << error << '\n';
    }
}

void VulkanWindow::destroy() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
    }
    offscreen_.reset();
    if (staging_mapped_ != nullptr) {
        vkUnmapMemory(device_, staging_memory_);
        staging_mapped_ = nullptr;
    }
    if (staging_ != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, staging_, nullptr);
        staging_ = VK_NULL_HANDLE;
    }
    if (staging_memory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, staging_memory_, nullptr);
        staging_memory_ = VK_NULL_HANDLE;
    }
    if (device_ != VK_NULL_HANDLE) {
        destroy_swapchain();
        if (frame_fence_ != VK_NULL_HANDLE) {
            vkDestroyFence(device_, frame_fence_, nullptr);
        }
        if (image_ready_ != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_, image_ready_, nullptr);
        }
        if (command_pool_ != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device_, command_pool_, nullptr);
        }
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (destroy_messenger_ != nullptr && messenger_ != VK_NULL_HANDLE) {
        destroy_messenger_(instance_, messenger_, nullptr);
        messenger_ = VK_NULL_HANDLE;
    }
    if (instance_ != VK_NULL_HANDLE) {
        if (surface_ != VK_NULL_HANDLE) {
            SDL_Vulkan_DestroySurface(instance_, surface_, nullptr);
            surface_ = VK_NULL_HANDLE;
        }
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
    if (window_ != nullptr) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
}

} // namespace

std::unique_ptr<Window> Window::create(const WindowOptions& options, std::string& error) {
    std::unique_ptr<VulkanWindow> window(new (std::nothrow) VulkanWindow());
    if (window == nullptr) {
        error = "out of memory creating the window";
        return nullptr;
    }
    if (!window->init(options, error)) {
        return nullptr;
    }
    return window;
}

} // namespace protekt::render
