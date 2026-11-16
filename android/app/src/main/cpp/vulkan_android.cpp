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
constexpr uint32_t kFrames = 2;

struct Frame {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkSemaphore acquire = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
};

struct Renderer {
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t family = 0, apiVersion = VK_API_VERSION_1_0, frameIndex = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    std::vector<VkFramebuffer> framebuffers;
    std::vector<VkSemaphore> renderFinished;
    std::vector<VkFence> imagesInFlight;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    std::array<Frame, kFrames> frames{};
    bool dynamicRendering = false, sync2 = false;
#ifdef VK_API_VERSION_1_3
    PFN_vkCmdBeginRendering beginRendering = nullptr;
    PFN_vkCmdEndRendering endRendering = nullptr;
    PFN_vkCmdPipelineBarrier2 barrier2 = nullptr;
    PFN_vkQueueSubmit2 submit2 = nullptr;
#endif
} r;

void check(VkResult res, const char *msg) { if (res != VK_SUCCESS) throw std::runtime_error(msg); }

void createInstance(ANativeWindow *window) {
    uint32_t ver = VK_API_VERSION_1_0;
    if (auto fn = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion")))
        if (fn(&ver) != VK_SUCCESS) ver = VK_API_VERSION_1_0;
    r.apiVersion = VK_API_VERSION_1_0;
#ifdef VK_API_VERSION_1_4
    if (ver >= VK_API_VERSION_1_4) r.apiVersion = VK_API_VERSION_1_4; else
#endif
#ifdef VK_API_VERSION_1_3
    if (ver >= VK_API_VERSION_1_3) r.apiVersion = VK_API_VERSION_1_3; else
#endif
#ifdef VK_API_VERSION_1_2
    if (ver >= VK_API_VERSION_1_2) r.apiVersion = VK_API_VERSION_1_2; else
#endif
    if (ver >= VK_API_VERSION_1_1) r.apiVersion = VK_API_VERSION_1_1;

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "LuaC Studio", 0, "LuaC", 0, r.apiVersion};
    const char *exts[]{VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0, &app, 0, nullptr, 2, exts};
    check(vkCreateInstance(&ci, nullptr, &r.instance), "vkCreateInstance");
    VkAndroidSurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR, nullptr, 0, window};
    check(vkCreateAndroidSurfaceKHR(r.instance, &sci, nullptr, &r.surface), "vkCreateAndroidSurfaceKHR");
}

void createDevice() {
    uint32_t count = 0;
    check(vkEnumeratePhysicalDevices(r.instance, &count, nullptr), "vkEnumeratePhysicalDevices");
    if (!count) throw std::runtime_error("No Vulkan device");
    std::vector<VkPhysicalDevice> devs(count);
    check(vkEnumeratePhysicalDevices(r.instance, &count, devs.data()), "vkEnumeratePhysicalDevices");

    uint32_t bestScore = 0;
    VkPhysicalDeviceProperties bestProps{};
    for (VkPhysicalDevice cand : devs) {
        uint32_t fc = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(cand, &fc, nullptr);
        std::vector<VkQueueFamilyProperties> props(fc);
        vkGetPhysicalDeviceQueueFamilyProperties(cand, &fc, props.data());
        uint32_t fam = UINT32_MAX;
        for (uint32_t i = 0; i < fc; ++i) {
            VkBool32 sup = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(cand, i, r.surface, &sup);
            if (sup && (props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) { fam = i; break; }
        }
        if (fam == UINT32_MAX) continue;
        VkPhysicalDeviceProperties dp{};
        vkGetPhysicalDeviceProperties(cand, &dp);
        uint32_t score = dp.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 1000 :
                         dp.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 500 : 100;
        if (r.physical == VK_NULL_HANDLE || score > bestScore) {
            r.physical = cand; r.family = fam; bestScore = score; bestProps = dp;
        }
    }
    if (r.physical == VK_NULL_HANDLE) throw std::runtime_error("No present queue");
    r.apiVersion = std::min(r.apiVersion, bestProps.apiVersion);

    const char *ext = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, r.family, 1, &prio};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, nullptr, 0, 1, &qci, 0, nullptr, 1, &ext};

#ifdef VK_API_VERSION_1_3
    bool has13 = r.apiVersion >= VK_API_VERSION_1_3;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f13};
    VkPhysicalDeviceVulkan13Features en13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFeatures2 en2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &en13};
    auto getFeat2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
        vkGetInstanceProcAddr(r.instance, "vkGetPhysicalDeviceFeatures2"));
    if (has13 && getFeat2) {
        getFeat2(r.physical, &f2);
        en13.dynamicRendering = f13.dynamicRendering;
        en13.synchronization2 = f13.synchronization2;
        if (en13.dynamicRendering || en13.synchronization2) dci.pNext = &en2;
    }
#endif
    check(vkCreateDevice(r.physical, &dci, nullptr, &r.device), "vkCreateDevice");
    vkGetDeviceQueue(r.device, r.family, 0, &r.queue);

#ifdef VK_API_VERSION_1_3
    if (en13.dynamicRendering) {
        r.beginRendering = reinterpret_cast<PFN_vkCmdBeginRendering>(vkGetDeviceProcAddr(r.device, "vkCmdBeginRendering"));
        r.endRendering = reinterpret_cast<PFN_vkCmdEndRendering>(vkGetDeviceProcAddr(r.device, "vkCmdEndRendering"));
        r.dynamicRendering = r.beginRendering && r.endRendering;
    }
    if (en13.synchronization2) {
        r.barrier2 = reinterpret_cast<PFN_vkCmdPipelineBarrier2>(vkGetDeviceProcAddr(r.device, "vkCmdPipelineBarrier2"));
        r.submit2 = reinterpret_cast<PFN_vkQueueSubmit2>(vkGetDeviceProcAddr(r.device, "vkQueueSubmit2"));
        r.sync2 = r.barrier2 && r.submit2;
    }
#endif
}

void destroySwapchainViews() {
    for (auto fb : r.framebuffers) if (fb) vkDestroyFramebuffer(r.device, fb, nullptr);
    for (auto v : r.views) if (v) vkDestroyImageView(r.device, v, nullptr);
    r.framebuffers.clear(); r.views.clear(); r.images.clear(); r.imagesInFlight.clear();
}

void createSwapchain(VkSwapchainKHR old = VK_NULL_HANDLE) {
    VkSurfaceCapabilitiesKHR caps{};
    check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(r.physical, r.surface, &caps), "Surface capabilities");
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(r.physical, r.surface, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(r.physical, r.surface, &count, fmts.data());
    if (!count) throw std::runtime_error("No surface formats");
    VkSurfaceFormatKHR sel = fmts[0];
    for (auto &f : fmts) if (f.format == VK_FORMAT_R8G8B8A8_UNORM || f.format == VK_FORMAT_B8G8R8A8_UNORM) { sel = f; break; }
    r.format = sel.format;
    r.extent = caps.currentExtent.width == UINT32_MAX ? caps.minImageExtent : caps.currentExtent;
    uint32_t imgCount = caps.minImageCount + 1;
    if (caps.maxImageCount && imgCount > caps.maxImageCount) imgCount = caps.maxImageCount;

    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    for (auto a : {VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                   VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR})
        if (caps.supportedCompositeAlpha & a) { alpha = a; break; }

    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = r.surface; sci.minImageCount = imgCount;
    sci.imageFormat = sel.format; sci.imageColorSpace = sel.colorSpace;
    sci.imageExtent = r.extent; sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform; sci.compositeAlpha = alpha;
    sci.presentMode = VK_PRESENT_MODE_FIFO_KHR; sci.clipped = VK_TRUE; sci.oldSwapchain = old;

    VkSwapchainKHR next = VK_NULL_HANDLE;
    check(vkCreateSwapchainKHR(r.device, &sci, nullptr, &next), "vkCreateSwapchainKHR");
    if (old) vkDestroySwapchainKHR(r.device, old, nullptr);
    r.swapchain = next;

    vkGetSwapchainImagesKHR(r.device, r.swapchain, &count, nullptr);
    r.images.resize(count); r.views.resize(count);
    vkGetSwapchainImagesKHR(r.device, r.swapchain, &count, r.images.data());
    for (uint32_t i = 0; i < count; ++i) {
        VkImageViewCreateInfo iv{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        iv.image = r.images[i]; iv.viewType = VK_IMAGE_VIEW_TYPE_2D; iv.format = r.format;
        iv.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        check(vkCreateImageView(r.device, &iv, nullptr, &r.views[i]), "vkCreateImageView");
    }

    if (!r.dynamicRendering) {
        if (!r.renderPass) {
            VkAttachmentDescription att{0, r.format, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR,
                VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR};
            VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
            VkSubpassDescription sub{0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, nullptr, 1, &ref};
            VkSubpassDependency dep{VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0};
            VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, nullptr, 0, 1, &att, 1, &sub, 1, &dep};
            check(vkCreateRenderPass(r.device, &rp, nullptr, &r.renderPass), "vkCreateRenderPass");
        }
        r.framebuffers.resize(count);
        for (uint32_t i = 0; i < count; ++i) {
            VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, nullptr, 0, r.renderPass,
                1, &r.views[i], r.extent.width, r.extent.height, 1};
            check(vkCreateFramebuffer(r.device, &fb, nullptr, &r.framebuffers[i]), "vkCreateFramebuffer");
        }
    }

    for (auto s : r.renderFinished) if (s) vkDestroySemaphore(r.device, s, nullptr);
    r.renderFinished.assign(count, VK_NULL_HANDLE);
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t i = 0; i < count; ++i) check(vkCreateSemaphore(r.device, &si, nullptr, &r.renderFinished[i]), "vkCreateSemaphore");
    r.imagesInFlight.assign(count, VK_NULL_HANDLE);
}

void recreateSwapchain() {
    if (!r.device) return;
    vkDeviceWaitIdle(r.device);
    destroySwapchainViews();
    VkSwapchainKHR old = r.swapchain;
    r.swapchain = VK_NULL_HANDLE;
    createSwapchain(old);
}

void initialize(ANativeWindow *window) {
    createInstance(window);
    createDevice();
    createSwapchain();
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, 0, r.family};
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT};
    for (auto &f : r.frames) {
        check(vkCreateCommandPool(r.device, &pci, nullptr, &f.pool), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, f.pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        check(vkAllocateCommandBuffers(r.device, &ai, &f.cmd), "vkAllocateCommandBuffers");
        check(vkCreateSemaphore(r.device, &si, nullptr, &f.acquire), "vkCreateSemaphore");
        check(vkCreateFence(r.device, &fi, nullptr, &f.fence), "vkCreateFence");
    }
}

void destroy() {
    if (r.device) vkDeviceWaitIdle(r.device);
    for (auto &f : r.frames) {
        if (f.fence) vkDestroyFence(r.device, f.fence, nullptr);
        if (f.acquire) vkDestroySemaphore(r.device, f.acquire, nullptr);
        if (f.pool) vkDestroyCommandPool(r.device, f.pool, nullptr);
    }
    for (auto s : r.renderFinished) if (s) vkDestroySemaphore(r.device, s, nullptr);
    destroySwapchainViews();
    if (r.renderPass) vkDestroyRenderPass(r.device, r.renderPass, nullptr);
    if (r.swapchain) vkDestroySwapchainKHR(r.device, r.swapchain, nullptr);
    if (r.device) vkDestroyDevice(r.device, nullptr);
    if (r.surface) vkDestroySurfaceKHR(r.instance, r.surface, nullptr);
    if (r.instance) vkDestroyInstance(r.instance, nullptr);
    r = Renderer{};
}

void draw() {
    if (!r.device || !r.swapchain || !r.extent.width || !r.extent.height) return;
    Frame &f = r.frames[r.frameIndex];
    if (vkWaitForFences(r.device, 1, &f.fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) return;

    uint32_t img = 0;
    VkResult acq = vkAcquireNextImageKHR(r.device, r.swapchain, 1000000000ull, f.acquire, VK_NULL_HANDLE, &img);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) { recreateSwapchain(); return; }
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) return;

    if (r.imagesInFlight[img] && r.imagesInFlight[img] != f.fence)
        vkWaitForFences(r.device, 1, &r.imagesInFlight[img], VK_TRUE, UINT64_MAX);
    r.imagesInFlight[img] = f.fence;
    vkResetFences(r.device, 1, &f.fence);
    vkResetCommandPool(r.device, f.pool, 0);

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(f.cmd, &bi);
    VkClearValue clear{}; clear.color = {{0.025f, 0.15f, 0.18f, 1.0f}};

#ifdef VK_API_VERSION_1_3
    if (r.dynamicRendering && r.sync2) {
        auto barrier = [&](VkImageLayout oldL, VkImageLayout newL, VkPipelineStageFlags2 srcS,
                           VkAccessFlags2 srcA, VkPipelineStageFlags2 dstS, VkAccessFlags2 dstA) {
            VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr, srcS, srcA, dstS, dstA,
                oldL, newL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, r.images[img], {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
            VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO, nullptr, 0, 0, nullptr, 0, nullptr, 1, &b};
            r.barrier2(f.cmd, &dep);
        };
        barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        att.imageView = r.views[img]; att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; att.storeOp = VK_ATTACHMENT_STORE_OP_STORE; att.clearValue = clear;
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, r.extent}; ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &att;
        r.beginRendering(f.cmd, &ri);
        r.endRendering(f.cmd);
        barrier(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, 0, 0);
    } else
#endif
    {
        VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, nullptr, r.renderPass,
            r.framebuffers[img], {{0, 0}, r.extent}, 1, &clear};
        vkCmdBeginRenderPass(f.cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdEndRenderPass(f.cmd);
    }
    if (vkEndCommandBuffer(f.cmd) != VK_SUCCESS) return;

    VkSemaphore fin = r.renderFinished[img];
#ifdef VK_API_VERSION_1_3
    if (r.sync2) {
        VkSemaphoreSubmitInfo w{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, f.acquire, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0};
        VkCommandBufferSubmitInfo c{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, nullptr, f.cmd, 0};
        VkSemaphoreSubmitInfo s{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, fin, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0};
        VkSubmitInfo2 sub{VK_STRUCTURE_TYPE_SUBMIT_INFO_2, nullptr, 0, 1, &w, 1, &c, 1, &s};
        if (r.submit2(r.queue, 1, &sub, f.fence) != VK_SUCCESS) return;
    } else
#endif
    {
        VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo sub{VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 1, &f.acquire, &wait, 1, &f.cmd, 1, &fin};
        if (vkQueueSubmit(r.queue, 1, &sub, f.fence) != VK_SUCCESS) return;
    }
    VkPresentInfoKHR pres{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, nullptr, 1, &fin, 1, &r.swapchain, &img};
    VkResult presRes = vkQueuePresentKHR(r.queue, &pres);
    r.frameIndex = (r.frameIndex + 1) % kFrames;
    if (acq == VK_SUBOPTIMAL_KHR || presRes == VK_ERROR_OUT_OF_DATE_KHR || presRes == VK_SUBOPTIMAL_KHR) recreateSwapchain();
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
        LOGI("Vulkan %u.%u (%ux%u, DR=%d, Sync2=%d)", VK_VERSION_MAJOR(r.apiVersion),
             VK_VERSION_MINOR(r.apiVersion), r.extent.width, r.extent.height, r.dynamicRendering, r.sync2);
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
