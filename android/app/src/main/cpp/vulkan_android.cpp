#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "LuaCVulkan", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "LuaCVulkan", __VA_ARGS__)

namespace {
constexpr uint32_t kMaxFramesInFlight = 2;

struct Frame {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkSemaphore imageAvailable = VK_NULL_HANDLE;
    VkFence inFlight = VK_NULL_HANDLE;
};

struct Renderer {
    ANativeWindow *window = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t family = 0;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t instanceApiVersion = VK_API_VERSION_1_0;
    uint32_t deviceApiVersion = VK_API_VERSION_1_0;
    bool useDynamicRendering = false;
    bool useSync2 = false;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    std::vector<VkFramebuffer> framebuffers;
    std::vector<VkSemaphore> renderFinished;
    std::vector<VkFence> imagesInFlight;
    VkRenderPass renderPass = VK_NULL_HANDLE;

    std::array<Frame, kMaxFramesInFlight> frames{};
    uint32_t frameIndex = 0;

#ifdef VK_API_VERSION_1_3
    PFN_vkCmdBeginRendering pfnCmdBeginRendering = nullptr;
    PFN_vkCmdEndRendering pfnCmdEndRendering = nullptr;
    PFN_vkCmdPipelineBarrier2 pfnCmdPipelineBarrier2 = nullptr;
    PFN_vkQueueSubmit2 pfnQueueSubmit2 = nullptr;
#endif
} r;

void check(VkResult result, const char *message) {
    if (result != VK_SUCCESS) throw std::runtime_error(message);
}

bool hasDeviceExtension(const std::vector<VkExtensionProperties> &extensions, const char *name) {
    for (const auto &ext : extensions)
        if (std::strcmp(ext.extensionName, name) == 0) return true;
    return false;
}

uint32_t scoreDevice(const VkPhysicalDeviceProperties &props) {
    switch (props.deviceType) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 10000;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 5000;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 1000;
    case VK_PHYSICAL_DEVICE_TYPE_OTHER: return 500;
    default: return 100;
    }
}

void createInstance(ANativeWindow *window) {
    uint32_t loaderVersion = VK_API_VERSION_1_0;
    auto enumerateVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
        vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
    if (enumerateVersion && enumerateVersion(&loaderVersion) != VK_SUCCESS)
        loaderVersion = VK_API_VERSION_1_0;

    uint32_t targetVersion = VK_API_VERSION_1_0;
#ifdef VK_API_VERSION_1_4
    if (loaderVersion >= VK_API_VERSION_1_4) targetVersion = VK_API_VERSION_1_4;
    else
#endif
#ifdef VK_API_VERSION_1_3
    if (loaderVersion >= VK_API_VERSION_1_3) targetVersion = VK_API_VERSION_1_3;
    else
#endif
#ifdef VK_API_VERSION_1_2
    if (loaderVersion >= VK_API_VERSION_1_2) targetVersion = VK_API_VERSION_1_2;
    else
#endif
    if (loaderVersion >= VK_API_VERSION_1_1) targetVersion = VK_API_VERSION_1_1;
    r.instanceApiVersion = targetVersion;

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "LuaC Studio";
    app.applicationVersion = VK_MAKE_VERSION(0, 5, 0);
    app.pEngineName = "LuaC";
    app.engineVersion = VK_MAKE_VERSION(0, 5, 0);
    app.apiVersion = r.instanceApiVersion;

    const char *extensions[]{VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = 2;
    info.ppEnabledExtensionNames = extensions;
    check(vkCreateInstance(&info, nullptr, &r.instance), "vkCreateInstance");

    VkAndroidSurfaceCreateInfoKHR surface{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    surface.window = window;
    check(vkCreateAndroidSurfaceKHR(r.instance, &surface, nullptr, &r.surface), "vkCreateAndroidSurfaceKHR");
}

void createDevice() {
    uint32_t count = 0;
    check(vkEnumeratePhysicalDevices(r.instance, &count, nullptr), "vkEnumeratePhysicalDevices");
    if (!count) throw std::runtime_error("No Vulkan device");
    std::vector<VkPhysicalDevice> devices(count);
    check(vkEnumeratePhysicalDevices(r.instance, &count, devices.data()), "vkEnumeratePhysicalDevices");

    uint32_t bestScore = 0;
    VkPhysicalDeviceProperties bestProps{};
    std::vector<VkExtensionProperties> bestExtensions;
    bool found = false;

    for (VkPhysicalDevice candidate : devices) {
        uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, nullptr);
        std::vector<VkQueueFamilyProperties> properties(families);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, properties.data());
        uint32_t chosenFamily = UINT32_MAX;
        for (uint32_t i = 0; i < families; ++i) {
            VkBool32 supported = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, r.surface, &supported);
            if (supported && (properties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                chosenFamily = i;
                break;
            }
        }
        if (chosenFamily == UINT32_MAX) continue;

        uint32_t extCount = 0;
        vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> extensions(extCount);
        vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extCount, extensions.data());
        if (!hasDeviceExtension(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;

        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(candidate, &props);
        uint32_t score = scoreDevice(props);
        if (!found || score > bestScore) {
            found = true;
            bestScore = score;
            bestProps = props;
            bestExtensions = std::move(extensions);
            r.physical = candidate;
            r.family = chosenFamily;
        }
    }
    if (!found || r.physical == VK_NULL_HANDLE) throw std::runtime_error("No present queue");

    r.deviceApiVersion = std::min(r.instanceApiVersion, bestProps.apiVersion);
    std::vector<const char *> enabledExtensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    bool enableDR = false;
    bool enableSync2 = false;
#ifdef VK_API_VERSION_1_3
    bool hasVk13 = r.deviceApiVersion >= VK_API_VERSION_1_3;
    VkPhysicalDeviceVulkan13Features vk13Features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceDynamicRenderingFeaturesKHR drExt{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR};
    VkPhysicalDeviceSynchronization2FeaturesKHR sync2Ext{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR};
    bool hasDrExt = !hasVk13 && hasDeviceExtension(bestExtensions, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
    bool hasSync2Ext = !hasVk13 && hasDeviceExtension(bestExtensions, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);

    VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    void **nextPtr = &features2.pNext;
    if (r.deviceApiVersion >= VK_API_VERSION_1_1) {
        if (hasVk13) {
            *nextPtr = &vk13Features;
            nextPtr = &vk13Features.pNext;
        } else {
            if (hasDrExt) { *nextPtr = &drExt; nextPtr = &drExt.pNext; }
            if (hasSync2Ext) { *nextPtr = &sync2Ext; nextPtr = &sync2Ext.pNext; }
        }
        vkGetPhysicalDeviceFeatures2(r.physical, &features2);
    }

    VkPhysicalDeviceVulkan13Features enabledVk13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceDynamicRenderingFeaturesKHR enabledDrExt{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR};
    VkPhysicalDeviceSynchronization2FeaturesKHR enabledSync2Ext{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR};
    if (hasVk13) {
        if (vk13Features.dynamicRendering) { enabledVk13.dynamicRendering = VK_TRUE; enableDR = true; }
        if (vk13Features.synchronization2) { enabledVk13.synchronization2 = VK_TRUE; enableSync2 = true; }
    } else {
        if (hasDrExt && drExt.dynamicRendering) {
            enabledDrExt.dynamicRendering = VK_TRUE;
            enabledExtensions.push_back(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
            enableDR = true;
        }
        if (hasSync2Ext && sync2Ext.synchronization2) {
            enabledSync2Ext.synchronization2 = VK_TRUE;
            enabledExtensions.push_back(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
            enableSync2 = true;
        }
    }
#endif

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueFamilyIndex = r.family;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;

    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue;
    info.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
    info.ppEnabledExtensionNames = enabledExtensions.data();

#ifdef VK_API_VERSION_1_3
    VkPhysicalDeviceFeatures2 enabledFeatures2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    if (r.deviceApiVersion >= VK_API_VERSION_1_1) {
        void **enableNext = &enabledFeatures2.pNext;
        if (hasVk13 && (enableDR || enableSync2)) {
            *enableNext = &enabledVk13;
        } else {
            if (hasDrExt && enableDR) { *enableNext = &enabledDrExt; enableNext = &enabledDrExt.pNext; }
            if (hasSync2Ext && enableSync2) { *enableNext = &enabledSync2Ext; }
        }
        info.pNext = &enabledFeatures2;
    }
#endif

    check(vkCreateDevice(r.physical, &info, nullptr, &r.device), "vkCreateDevice");
    vkGetDeviceQueue(r.device, r.family, 0, &r.queue);

#ifdef VK_API_VERSION_1_3
    if (enableDR) {
        r.pfnCmdBeginRendering = reinterpret_cast<PFN_vkCmdBeginRendering>(
            vkGetDeviceProcAddr(r.device, hasVk13 ? "vkCmdBeginRendering" : "vkCmdBeginRenderingKHR"));
        r.pfnCmdEndRendering = reinterpret_cast<PFN_vkCmdEndRendering>(
            vkGetDeviceProcAddr(r.device, hasVk13 ? "vkCmdEndRendering" : "vkCmdEndRenderingKHR"));
        r.useDynamicRendering = (r.pfnCmdBeginRendering != nullptr && r.pfnCmdEndRendering != nullptr);
    }
    if (enableSync2) {
        r.pfnCmdPipelineBarrier2 = reinterpret_cast<PFN_vkCmdPipelineBarrier2>(
            vkGetDeviceProcAddr(r.device, hasVk13 ? "vkCmdPipelineBarrier2" : "vkCmdPipelineBarrier2KHR"));
        r.pfnQueueSubmit2 = reinterpret_cast<PFN_vkQueueSubmit2>(
            vkGetDeviceProcAddr(r.device, hasVk13 ? "vkQueueSubmit2" : "vkQueueSubmit2KHR"));
        r.useSync2 = (r.pfnCmdPipelineBarrier2 != nullptr && r.pfnQueueSubmit2 != nullptr);
    }
#endif
}

void destroySwapchainViews() {
    for (auto framebuffer : r.framebuffers)
        if (framebuffer) vkDestroyFramebuffer(r.device, framebuffer, nullptr);
    for (auto view : r.views)
        if (view) vkDestroyImageView(r.device, view, nullptr);
    r.framebuffers.clear();
    r.views.clear();
    r.images.clear();
    r.imagesInFlight.clear();
}

void createSwapchain(VkSwapchainKHR oldSwapchain = VK_NULL_HANDLE) {
    VkSurfaceCapabilitiesKHR capabilities{};
    check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(r.physical, r.surface, &capabilities), "Surface capabilities");

    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(r.physical, r.surface, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(r.physical, r.surface, &count, formats.data());
    if (!count) throw std::runtime_error("No surface formats");
    VkSurfaceFormatKHR selected = formats[0];
    for (const auto &f : formats) {
        if ((f.format == VK_FORMAT_R8G8B8A8_UNORM || f.format == VK_FORMAT_B8G8R8A8_UNORM) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            selected = f;
            break;
        }
    }
    r.format = selected.format;
    r.extent = capabilities.currentExtent;
    if (r.extent.width == UINT32_MAX) {
        r.extent = capabilities.minImageExtent;
    }

    uint32_t imageCount = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount && imageCount > capabilities.maxImageCount)
        imageCount = capabilities.maxImageCount;

    VkCompositeAlphaFlagBitsKHR compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    const VkCompositeAlphaFlagBitsKHR alphaOrder[] = {
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR
    };
    for (auto alpha : alphaOrder) {
        if (capabilities.supportedCompositeAlpha & alpha) {
            compositeAlpha = alpha;
            break;
        }
    }

    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = r.surface;
    info.minImageCount = imageCount;
    info.imageFormat = selected.format;
    info.imageColorSpace = selected.colorSpace;
    info.imageExtent = r.extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = capabilities.currentTransform;
    info.compositeAlpha = compositeAlpha;
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    info.clipped = VK_TRUE;
    info.oldSwapchain = oldSwapchain;

    VkSwapchainKHR newSwapchain = VK_NULL_HANDLE;
    check(vkCreateSwapchainKHR(r.device, &info, nullptr, &newSwapchain), "vkCreateSwapchainKHR");
    if (oldSwapchain != VK_NULL_HANDLE) vkDestroySwapchainKHR(r.device, oldSwapchain, nullptr);
    r.swapchain = newSwapchain;

    vkGetSwapchainImagesKHR(r.device, r.swapchain, &count, nullptr);
    r.images.resize(count);
    vkGetSwapchainImagesKHR(r.device, r.swapchain, &count, r.images.data());
    r.views.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = r.images[i];
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = r.format;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        check(vkCreateImageView(r.device, &view, nullptr, &r.views[i]), "vkCreateImageView");
    }

    if (!r.useDynamicRendering) {
        if (r.renderPass == VK_NULL_HANDLE) {
            VkAttachmentDescription attachment{};
            attachment.format = r.format;
            attachment.samples = VK_SAMPLE_COUNT_1_BIT;
            attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
            VkSubpassDescription subpass{};
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.colorAttachmentCount = 1;
            subpass.pColorAttachments = &color;
            VkSubpassDependency dep{};
            dep.srcSubpass = VK_SUBPASS_EXTERNAL;
            dep.dstSubpass = 0;
            dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
            pass.attachmentCount = 1;
            pass.pAttachments = &attachment;
            pass.subpassCount = 1;
            pass.pSubpasses = &subpass;
            pass.dependencyCount = 1;
            pass.pDependencies = &dep;
            check(vkCreateRenderPass(r.device, &pass, nullptr, &r.renderPass), "vkCreateRenderPass");
        }
        r.framebuffers.resize(count);
        for (uint32_t i = 0; i < count; ++i) {
            VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fb.renderPass = r.renderPass;
            fb.attachmentCount = 1;
            fb.pAttachments = &r.views[i];
            fb.width = r.extent.width;
            fb.height = r.extent.height;
            fb.layers = 1;
            check(vkCreateFramebuffer(r.device, &fb, nullptr, &r.framebuffers[i]), "vkCreateFramebuffer");
        }
    }

    for (auto sem : r.renderFinished)
        if (sem) vkDestroySemaphore(r.device, sem, nullptr);
    r.renderFinished.assign(count, VK_NULL_HANDLE);
    VkSemaphoreCreateInfo semInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t i = 0; i < count; ++i)
        check(vkCreateSemaphore(r.device, &semInfo, nullptr, &r.renderFinished[i]), "vkCreateSemaphore");
    r.imagesInFlight.assign(count, VK_NULL_HANDLE);
}

void createFrameResources() {
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.queueFamilyIndex = r.family;
    VkSemaphoreCreateInfo semInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (auto &frame : r.frames) {
        check(vkCreateCommandPool(r.device, &poolInfo, nullptr, &frame.pool), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        alloc.commandPool = frame.pool;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(r.device, &alloc, &frame.cmd), "vkAllocateCommandBuffers");
        check(vkCreateSemaphore(r.device, &semInfo, nullptr, &frame.imageAvailable), "vkCreateSemaphore");
        check(vkCreateFence(r.device, &fenceInfo, nullptr, &frame.inFlight), "vkCreateFence");
    }
}

void recreateSwapchain() {
    if (!r.device) return;
    vkDeviceWaitIdle(r.device);
    destroySwapchainViews();
    VkSwapchainKHR oldSwapchain = r.swapchain;
    r.swapchain = VK_NULL_HANDLE;
    createSwapchain(oldSwapchain);
}

void initialize(ANativeWindow *window) {
    r.window = window;
    createInstance(window);
    createDevice();
    createSwapchain();
    createFrameResources();
}

void destroy() {
    if (r.device) vkDeviceWaitIdle(r.device);
    for (auto &frame : r.frames) {
        if (frame.inFlight) vkDestroyFence(r.device, frame.inFlight, nullptr);
        if (frame.imageAvailable) vkDestroySemaphore(r.device, frame.imageAvailable, nullptr);
        if (frame.pool) vkDestroyCommandPool(r.device, frame.pool, nullptr);
        frame = Frame{};
    }
    for (auto sem : r.renderFinished)
        if (sem) vkDestroySemaphore(r.device, sem, nullptr);
    r.renderFinished.clear();
    destroySwapchainViews();
    if (r.renderPass) vkDestroyRenderPass(r.device, r.renderPass, nullptr);
    if (r.swapchain) vkDestroySwapchainKHR(r.device, r.swapchain, nullptr);
    if (r.device) vkDestroyDevice(r.device, nullptr);
    if (r.surface) vkDestroySurfaceKHR(r.instance, r.surface, nullptr);
    if (r.instance) vkDestroyInstance(r.instance, nullptr);
    r = Renderer{};
}

void draw() {
    if (!r.device || !r.swapchain || r.extent.width == 0 || r.extent.height == 0) return;

    Frame &frame = r.frames[r.frameIndex];
    if (vkWaitForFences(r.device, 1, &frame.inFlight, VK_TRUE, UINT64_MAX) != VK_SUCCESS) return;

    uint32_t image = 0;
    VkResult acquireResult = vkAcquireNextImageKHR(r.device, r.swapchain, 1000000000ull,
                                                   frame.imageAvailable, VK_NULL_HANDLE, &image);
    if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateSwapchain();
        return;
    }
    if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR) return;

    if (r.imagesInFlight[image] != VK_NULL_HANDLE && r.imagesInFlight[image] != frame.inFlight)
        vkWaitForFences(r.device, 1, &r.imagesInFlight[image], VK_TRUE, UINT64_MAX);
    r.imagesInFlight[image] = frame.inFlight;

    vkResetFences(r.device, 1, &frame.inFlight);
    vkResetCommandPool(r.device, frame.pool, 0);

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(frame.cmd, &begin);

    VkClearValue clear{};
    clear.color = {{0.025f, 0.15f, 0.18f, 1.0f}};

#ifdef VK_API_VERSION_1_3
    if (r.useDynamicRendering && r.useSync2) {
        VkImageMemoryBarrier2 toColor{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        toColor.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        toColor.srcAccessMask = VK_ACCESS_2_NONE;
        toColor.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        toColor.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        toColor.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toColor.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toColor.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toColor.image = r.images[image];
        toColor.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep1{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep1.imageMemoryBarrierCount = 1;
        dep1.pImageMemoryBarriers = &toColor;
        r.pfnCmdPipelineBarrier2(frame.cmd, &dep1);

        VkRenderingAttachmentInfo colorAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        colorAttachment.imageView = r.views[image];
        colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.clearValue = clear;

        VkRenderingInfo renderingInfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
        renderingInfo.renderArea = {{0, 0}, r.extent};
        renderingInfo.layerCount = 1;
        renderingInfo.colorAttachmentCount = 1;
        renderingInfo.pColorAttachments = &colorAttachment;
        r.pfnCmdBeginRendering(frame.cmd, &renderingInfo);
        r.pfnCmdEndRendering(frame.cmd);

        VkImageMemoryBarrier2 toPresent{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        toPresent.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        toPresent.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        toPresent.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
        toPresent.dstAccessMask = VK_ACCESS_2_NONE;
        toPresent.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        toPresent.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toPresent.image = r.images[image];
        toPresent.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep2{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep2.imageMemoryBarrierCount = 1;
        dep2.pImageMemoryBarriers = &toPresent;
        r.pfnCmdPipelineBarrier2(frame.cmd, &dep2);
    } else
#endif
    {
        if (r.renderPass == VK_NULL_HANDLE) {
            vkEndCommandBuffer(frame.cmd);
            return;
        }
        VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rp.renderPass = r.renderPass;
        rp.framebuffer = r.framebuffers[image];
        rp.renderArea = {{0, 0}, r.extent};
        rp.clearValueCount = 1;
        rp.pClearValues = &clear;
        vkCmdBeginRenderPass(frame.cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdEndRenderPass(frame.cmd);
    }
    if (vkEndCommandBuffer(frame.cmd) != VK_SUCCESS) return;

    VkSemaphore finished = r.renderFinished[image];
#ifdef VK_API_VERSION_1_3
    if (r.useSync2) {
        VkSemaphoreSubmitInfo waitSem{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        waitSem.semaphore = frame.imageAvailable;
        waitSem.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

        VkCommandBufferSubmitInfo cmdInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        cmdInfo.commandBuffer = frame.cmd;

        VkSemaphoreSubmitInfo signalSem{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        signalSem.semaphore = finished;
        signalSem.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

        VkSubmitInfo2 submit2{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        submit2.waitSemaphoreInfoCount = 1;
        submit2.pWaitSemaphoreInfos = &waitSem;
        submit2.commandBufferInfoCount = 1;
        submit2.pCommandBufferInfos = &cmdInfo;
        submit2.signalSemaphoreInfoCount = 1;
        submit2.pSignalSemaphoreInfos = &signalSem;
        if (r.pfnQueueSubmit2(r.queue, 1, &submit2, frame.inFlight) != VK_SUCCESS) return;
    } else
#endif
    {
        VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &frame.imageAvailable;
        submit.pWaitDstStageMask = &wait;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &frame.cmd;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &finished;
        if (vkQueueSubmit(r.queue, 1, &submit, frame.inFlight) != VK_SUCCESS) return;
    }

    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &finished;
    present.swapchainCount = 1;
    present.pSwapchains = &r.swapchain;
    present.pImageIndices = &image;
    VkResult presentResult = vkQueuePresentKHR(r.queue, &present);

    r.frameIndex = (r.frameIndex + 1) % kMaxFramesInFlight;

    if (acquireResult == VK_SUBOPTIMAL_KHR || presentResult == VK_ERROR_OUT_OF_DATE_KHR ||
        presentResult == VK_SUBOPTIMAL_KHR) {
        recreateSwapchain();
    }
}
}

void androidVulkanDraw();

bool androidVulkanCreate(JNIEnv *env, jobject surface) {
    try {
        destroy();
        ANativeWindow *window = ANativeWindow_fromSurface(env, surface);
        if (!window) return false;
        initialize(window);
        ANativeWindow_release(window);
        LOGI("Vulkan %u.%u initialized (%ux%u, DR=%d, Sync2=%d)",
             VK_VERSION_MAJOR(r.deviceApiVersion), VK_VERSION_MINOR(r.deviceApiVersion),
             r.extent.width, r.extent.height, r.useDynamicRendering ? 1 : 0, r.useSync2 ? 1 : 0);
        androidVulkanDraw();
        return true;
    } catch (const std::exception &error) {
        LOGE("Initialization failed: %s", error.what());
        destroy();
        return false;
    }
}
void androidVulkanDraw() { draw(); }
void androidVulkanDestroy() { destroy(); }
