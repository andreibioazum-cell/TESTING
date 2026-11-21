#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "studio_app.hpp"
#include "mesh3d_vert_spv.h"
#include "mesh3d_frag_spv.h"
#include "shadow_vert_spv.h"
#include "ui2d_vert_spv.h"
#include "ui2d_frag_spv.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "ObsidianVulkan", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "ObsidianVulkan", __VA_ARGS__)

namespace {
constexpr uint32_t kFrames = 2;
constexpr uint32_t kShadowSize = 1024;

struct Push3D {
    float mvp[16];
    float flags[4];
};

struct SceneUbo {
    float lightVP[16];
    float sunDir[4];
    float skyTint[4];
};

struct Push2D {
    float scale[2];
};

struct DynBuffer {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDeviceSize cap = 0;
    void *mapped = nullptr;
};

struct Frame {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkSemaphore acquire = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    DynBuffer vb3d;
    DynBuffer vb2d;
    DynBuffer ubo3d;
    VkDescriptorSet descSet3D = VK_NULL_HANDLE;
};

struct Renderer {
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t family = 0, apiVersion = VK_API_VERSION_1_0, frameIndex = 0;
    uint32_t requestedW = 0, requestedH = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkFormat depthFormat = VK_FORMAT_D24_UNORM_S8_UINT;
    VkExtent2D extent{};

    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    VkImage depthImage = VK_NULL_HANDLE;
    VkDeviceMemory depthMem = VK_NULL_HANDLE;
    VkImageView depthView = VK_NULL_HANDLE;

    VkRenderPass renderPass = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers;
    std::vector<VkSemaphore> renderFinished;
    std::vector<VkFence> imagesInFlight;
    std::array<Frame, kFrames> frames{};

    // Shadow map pass
    VkImage shadowImg = VK_NULL_HANDLE;
    VkDeviceMemory shadowMem = VK_NULL_HANDLE;
    VkImageView shadowView = VK_NULL_HANDLE;
    VkSampler shadowSampler = VK_NULL_HANDLE;
    VkRenderPass shadowRenderPass = VK_NULL_HANDLE;
    VkFramebuffer shadowFb = VK_NULL_HANDLE;

    // Pipelines & descriptors
    VkDescriptorSetLayout descLayout3D = VK_NULL_HANDLE;
    VkDescriptorSetLayout descLayout = VK_NULL_HANDLE;
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    VkDescriptorSet descSet = VK_NULL_HANDLE;
    VkPipelineLayout layout3D = VK_NULL_HANDLE;
    VkPipelineLayout layout2D = VK_NULL_HANDLE;
    VkPipeline pipeShadow = VK_NULL_HANDLE;
    VkPipeline pipe3DMesh = VK_NULL_HANDLE;
    VkPipeline pipe3DLines = VK_NULL_HANDLE;
    VkPipeline pipe3DOverlay = VK_NULL_HANDLE;
    VkPipeline pipeUi2D = VK_NULL_HANDLE;

    // Font atlas texture
    VkImage atlasImg = VK_NULL_HANDLE;
    VkDeviceMemory atlasMem = VK_NULL_HANDLE;
    VkImageView atlasView = VK_NULL_HANDLE;
    VkSampler atlasSampler = VK_NULL_HANDLE;
    DynBuffer atlasStage;
    uint64_t uploadedAtlasVersion = 0;
} r;

void check(VkResult res, const char *msg) { if (res != VK_SUCCESS) throw std::runtime_error(msg); }

uint32_t findMemType(uint32_t bits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(r.physical, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) return i;
    }
    throw std::runtime_error("No matching Vulkan memory type");
}

void destroyDynBuffer(DynBuffer &b) {
    if (b.mapped) { vkUnmapMemory(r.device, b.mem); b.mapped = nullptr; }
    if (b.buf) { vkDestroyBuffer(r.device, b.buf, nullptr); b.buf = VK_NULL_HANDLE; }
    if (b.mem) { vkFreeMemory(r.device, b.mem, nullptr); b.mem = VK_NULL_HANDLE; }
    b.cap = 0;
}

void ensureDynBuffer(DynBuffer &b, VkDeviceSize need, VkBufferUsageFlags usage) {
    if (need == 0) need = 256;
    if (b.cap >= need && b.buf) return;
    destroyDynBuffer(b);
    VkDeviceSize cap = std::max<VkDeviceSize>(need * 2, 65536);
    if (usage == VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) cap = std::max<VkDeviceSize>(need, 256);
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, cap, usage, VK_SHARING_MODE_EXCLUSIVE};
    check(vkCreateBuffer(r.device, &bi, nullptr, &b.buf), "vkCreateBuffer");
    VkMemoryRequirements mr{};
    vkGetBufferMemoryRequirements(r.device, b.buf, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, mr.size,
                            findMemType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    check(vkAllocateMemory(r.device, &ai, nullptr, &b.mem), "vkAllocateMemory");
    check(vkBindBufferMemory(r.device, b.buf, b.mem, 0), "vkBindBufferMemory");
    check(vkMapMemory(r.device, b.mem, 0, cap, 0, &b.mapped), "vkMapMemory");
    b.cap = cap;
}

VkShaderModule makeShader(const void *words, size_t byteLen) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, byteLen,
                                reinterpret_cast<const uint32_t *>(words)};
    VkShaderModule m = VK_NULL_HANDLE;
    check(vkCreateShaderModule(r.device, &ci, nullptr, &m), "vkCreateShaderModule");
    return m;
}

void createInstance(ANativeWindow *window) {
    uint32_t ver = VK_API_VERSION_1_0;
    if (auto fn = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion")))
        if (fn(&ver) != VK_SUCCESS) ver = VK_API_VERSION_1_0;
    r.apiVersion = ver >= VK_API_VERSION_1_1 ? VK_API_VERSION_1_1 : VK_API_VERSION_1_0;

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "Obsidian Core", 0, "Obsidronium", 0, r.apiVersion};
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
            r.physical = cand; r.family = fam; bestScore = score;
        }
    }
    if (r.physical == VK_NULL_HANDLE) throw std::runtime_error("No present queue");

    for (VkFormat df : {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM}) {
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(r.physical, df, &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            r.depthFormat = df;
            break;
        }
    }

    const char *ext = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, r.family, 1, &prio};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, nullptr, 0, 1, &qci, 0, nullptr, 1, &ext};
    check(vkCreateDevice(r.physical, &dci, nullptr, &r.device), "vkCreateDevice");
    vkGetDeviceQueue(r.device, r.family, 0, &r.queue);
}

void destroyDepth() {
    if (r.depthView) { vkDestroyImageView(r.device, r.depthView, nullptr); r.depthView = VK_NULL_HANDLE; }
    if (r.depthImage) { vkDestroyImage(r.device, r.depthImage, nullptr); r.depthImage = VK_NULL_HANDLE; }
    if (r.depthMem) { vkFreeMemory(r.device, r.depthMem, nullptr); r.depthMem = VK_NULL_HANDLE; }
}

void createDepth() {
    destroyDepth();
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = r.depthFormat;
    ii.extent = {r.extent.width, r.extent.height, 1};
    ii.mipLevels = 1; ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    check(vkCreateImage(r.device, &ii, nullptr, &r.depthImage), "vkCreateImage(depth)");
    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(r.device, r.depthImage, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, mr.size,
                            findMemType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    check(vkAllocateMemory(r.device, &ai, nullptr, &r.depthMem), "vkAllocateMemory(depth)");
    check(vkBindImageMemory(r.device, r.depthImage, r.depthMem, 0), "vkBindImageMemory(depth)");

    VkImageViewCreateInfo iv{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    iv.image = r.depthImage;
    iv.viewType = VK_IMAGE_VIEW_TYPE_2D;
    iv.format = r.depthFormat;
    iv.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    check(vkCreateImageView(r.device, &iv, nullptr, &r.depthView), "vkCreateImageView(depth)");
}

void createShadowResources() {
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_D16_UNORM;
    ii.extent = {kShadowSize, kShadowSize, 1};
    ii.mipLevels = 1; ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    check(vkCreateImage(r.device, &ii, nullptr, &r.shadowImg), "vkCreateImage(shadow)");
    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(r.device, r.shadowImg, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, mr.size,
                            findMemType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    check(vkAllocateMemory(r.device, &ai, nullptr, &r.shadowMem), "vkAllocateMemory(shadow)");
    check(vkBindImageMemory(r.device, r.shadowImg, r.shadowMem, 0), "vkBindImageMemory(shadow)");

    VkImageViewCreateInfo iv{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    iv.image = r.shadowImg; iv.viewType = VK_IMAGE_VIEW_TYPE_2D; iv.format = VK_FORMAT_D16_UNORM;
    iv.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    check(vkCreateImageView(r.device, &iv, nullptr, &r.shadowView), "vkCreateImageView(shadow)");

    VkSamplerCreateInfo sc{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sc.magFilter = VK_FILTER_NEAREST; sc.minFilter = VK_FILTER_NEAREST;
    sc.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sc.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    check(vkCreateSampler(r.device, &sc, nullptr, &r.shadowSampler), "vkCreateSampler(shadow)");

    VkAttachmentDescription att{0, VK_FORMAT_D16_UNORM, VK_SAMPLE_COUNT_1_BIT,
        VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
        VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkAttachmentReference dRef{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, nullptr, 0, nullptr, nullptr, &dRef};
    VkSubpassDependency deps[2]{
        {VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
         VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, 0},
        {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, 0}
    };
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, nullptr, 0, 1, &att, 1, &sub, 2, deps};
    check(vkCreateRenderPass(r.device, &rp, nullptr, &r.shadowRenderPass), "vkCreateRenderPass(shadow)");

    VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, nullptr, 0, r.shadowRenderPass,
        1, &r.shadowView, kShadowSize, kShadowSize, 1};
    check(vkCreateFramebuffer(r.device, &fb, nullptr, &r.shadowFb), "vkCreateFramebuffer(shadow)");
}

void destroySwapchainViews() {
    for (auto fb : r.framebuffers) if (fb) vkDestroyFramebuffer(r.device, fb, nullptr);
    for (auto v : r.views) if (v) vkDestroyImageView(r.device, v, nullptr);
    r.framebuffers.clear(); r.views.clear(); r.images.clear(); r.imagesInFlight.clear();
    destroyDepth();
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
    if (r.requestedW > 0 && r.requestedH > 0) {
        uint32_t w = std::clamp(r.requestedW, caps.minImageExtent.width, caps.maxImageExtent.width);
        uint32_t h = std::clamp(r.requestedH, caps.minImageExtent.height, caps.maxImageExtent.height);
        r.extent = {w, h};
    } else if (caps.currentExtent.width != UINT32_MAX) {
        r.extent = caps.currentExtent;
    } else {
        r.extent = caps.minImageExtent;
    }
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
    sci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
        ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
        : caps.currentTransform;
    sci.compositeAlpha = alpha;
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

    createDepth();

    if (!r.renderPass) {
        VkAttachmentDescription atts[2]{};
        atts[0] = {0, r.format, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR,
                   VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
                   VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR};
        atts[1] = {0, r.depthFormat, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR,
                   VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
                   VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, nullptr, 1, &colorRef, nullptr, &depthRef};
        VkSubpassDependency dep{VK_SUBPASS_EXTERNAL, 0,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
            0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, 0};
        VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, nullptr, 0, 2, atts, 1, &sub, 1, &dep};
        check(vkCreateRenderPass(r.device, &rp, nullptr, &r.renderPass), "vkCreateRenderPass");
    }

    r.framebuffers.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        VkImageView fviews[2] = {r.views[i], r.depthView};
        VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, nullptr, 0, r.renderPass,
            2, fviews, r.extent.width, r.extent.height, 1};
        check(vkCreateFramebuffer(r.device, &fb, nullptr, &r.framebuffers[i]), "vkCreateFramebuffer");
    }

    for (auto s : r.renderFinished) if (s) vkDestroySemaphore(r.device, s, nullptr);
    r.renderFinished.assign(count, VK_NULL_HANDLE);
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t i = 0; i < count; ++i) check(vkCreateSemaphore(r.device, &si, nullptr, &r.renderFinished[i]), "vkCreateSemaphore");
    r.imagesInFlight.assign(count, VK_NULL_HANDLE);
}

void createAtlasTexture() {
    uint32_t sz = StudioFont::kAtlasSize;
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = {sz, sz, 1};
    ii.mipLevels = 1; ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    check(vkCreateImage(r.device, &ii, nullptr, &r.atlasImg), "vkCreateImage(atlas)");
    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(r.device, r.atlasImg, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, mr.size,
                            findMemType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    check(vkAllocateMemory(r.device, &ai, nullptr, &r.atlasMem), "vkAllocateMemory(atlas)");
    check(vkBindImageMemory(r.device, r.atlasImg, r.atlasMem, 0), "vkBindImageMemory(atlas)");

    VkImageViewCreateInfo iv{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    iv.image = r.atlasImg; iv.viewType = VK_IMAGE_VIEW_TYPE_2D; iv.format = VK_FORMAT_R8G8B8A8_UNORM;
    iv.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    check(vkCreateImageView(r.device, &iv, nullptr, &r.atlasView), "vkCreateImageView(atlas)");

    VkSamplerCreateInfo sc{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sc.magFilter = VK_FILTER_LINEAR; sc.minFilter = VK_FILTER_LINEAR;
    sc.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sc.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    check(vkCreateSampler(r.device, &sc, nullptr, &r.atlasSampler), "vkCreateSampler");
}

void createPipelines() {
    // 2D UI descriptor layout
    VkDescriptorSetLayoutBinding b2{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dsl2{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, 1, &b2};
    check(vkCreateDescriptorSetLayout(r.device, &dsl2, nullptr, &r.descLayout), "vkCreateDescriptorSetLayout(2D)");

    // 3D descriptor layout (shadow map + scene UBO)
    VkDescriptorSetLayoutBinding b3[2]{
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}
    };
    VkDescriptorSetLayoutCreateInfo dsl3{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, 2, b3};
    check(vkCreateDescriptorSetLayout(r.device, &dsl3, nullptr, &r.descLayout3D), "vkCreateDescriptorSetLayout(3D)");

    VkDescriptorPoolSize ps[2]{
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 + kFrames},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kFrames}
    };
    VkDescriptorPoolCreateInfo dpc{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, 1 + kFrames, 2, ps};
    check(vkCreateDescriptorPool(r.device, &dpc, nullptr, &r.descPool), "vkCreateDescriptorPool");

    VkDescriptorSetAllocateInfo dsa2{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, r.descPool, 1, &r.descLayout};
    check(vkAllocateDescriptorSets(r.device, &dsa2, &r.descSet), "vkAllocateDescriptorSets(2D)");
    VkDescriptorImageInfo dii{r.atlasSampler, r.atlasView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wds2{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, r.descSet, 0, 0, 1,
                            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &dii, nullptr, nullptr};
    vkUpdateDescriptorSets(r.device, 1, &wds2, 0, nullptr);

    for (auto &f : r.frames) {
        ensureDynBuffer(f.ubo3d, sizeof(SceneUbo), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        VkDescriptorSetAllocateInfo dsa3{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, r.descPool, 1, &r.descLayout3D};
        check(vkAllocateDescriptorSets(r.device, &dsa3, &f.descSet3D), "vkAllocateDescriptorSets(3D)");
        VkDescriptorImageInfo shInfo{r.shadowSampler, r.shadowView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorBufferInfo uboInfo{f.ubo3d.buf, 0, sizeof(SceneUbo)};
        VkWriteDescriptorSet wds3[2]{
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, f.descSet3D, 0, 0, 1,
             VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &shInfo, nullptr, nullptr},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, f.descSet3D, 1, 0, 1,
             VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &uboInfo, nullptr}
        };
        vkUpdateDescriptorSets(r.device, 2, wds3, 0, nullptr);
    }

    VkPushConstantRange pc3{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push3D)};
    VkPipelineLayoutCreateInfo pl3{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 1, &r.descLayout3D, 1, &pc3};
    check(vkCreatePipelineLayout(r.device, &pl3, nullptr, &r.layout3D), "vkCreatePipelineLayout(3D)");

    VkPushConstantRange pc2{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Push2D)};
    VkPipelineLayoutCreateInfo pl2{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 1, &r.descLayout, 1, &pc2};
    check(vkCreatePipelineLayout(r.device, &pl2, nullptr, &r.layout2D), "vkCreatePipelineLayout(2D)");

    VkShaderModule shv = makeShader(shadow_vert_spv, sizeof(shadow_vert_spv));
    VkShaderModule m3v = makeShader(mesh3d_vert_spv, sizeof(mesh3d_vert_spv));
    VkShaderModule m3f = makeShader(mesh3d_frag_spv, sizeof(mesh3d_frag_spv));
    VkShaderModule u2v = makeShader(ui2d_vert_spv, sizeof(ui2d_vert_spv));
    VkShaderModule u2f = makeShader(ui2d_frag_spv, sizeof(ui2d_frag_spv));

    VkDynamicState dyns[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, nullptr, 0, 2, dyns};
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, nullptr, 0, 1, nullptr, 1, nullptr};
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, nullptr, 0, VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, nullptr, 0, VK_FALSE, VK_LOGIC_OP_COPY, 1, &blend};

    // 3D pipelines
    VkPipelineShaderStageCreateInfo st3[2]{
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, m3v, "main"},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, m3f, "main"}
    };
    VkVertexInputBindingDescription bind3{0, sizeof(MeshVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attr3[3]{
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, x)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, nx)},
        {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(MeshVertex, color)}
    };
    VkPipelineVertexInputStateCreateInfo vi3{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, nullptr, 0, 1, &bind3, 3, attr3};
    VkPipelineInputAssemblyStateCreateInfo iaTri{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, nullptr, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineInputAssemblyStateCreateInfo iaLine{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, nullptr, 0, VK_PRIMITIVE_TOPOLOGY_LINE_LIST};

    VkPipelineRasterizationStateCreateInfo rs3{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs3.polygonMode = VK_POLYGON_MODE_FILL;
    rs3.cullMode = VK_CULL_MODE_NONE;
    rs3.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs3.lineWidth = 1.0f;

    VkPipelineDepthStencilStateCreateInfo ds3{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds3.depthTestEnable = VK_TRUE; ds3.depthWriteEnable = VK_TRUE; ds3.depthCompareOp = VK_COMPARE_OP_LESS;

    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.stageCount = 2; gp.pStages = st3; gp.pVertexInputState = &vi3; gp.pInputAssemblyState = &iaTri;
    gp.pViewportState = &vp; gp.pRasterizationState = &rs3; gp.pMultisampleState = &ms;
    gp.pDepthStencilState = &ds3; gp.pColorBlendState = &cb; gp.pDynamicState = &dyn;
    gp.layout = r.layout3D; gp.renderPass = r.renderPass;
    check(vkCreateGraphicsPipelines(r.device, VK_NULL_HANDLE, 1, &gp, nullptr, &r.pipe3DMesh), "pipe3DMesh");

    ds3.depthWriteEnable = VK_FALSE;
    gp.pInputAssemblyState = &iaLine;
    check(vkCreateGraphicsPipelines(r.device, VK_NULL_HANDLE, 1, &gp, nullptr, &r.pipe3DLines), "pipe3DLines");

    ds3.depthTestEnable = VK_FALSE;
    check(vkCreateGraphicsPipelines(r.device, VK_NULL_HANDLE, 1, &gp, nullptr, &r.pipe3DOverlay), "pipe3DOverlay");

    // Shadow map pipeline (depth-only)
    VkPipelineShaderStageCreateInfo stSh{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, shv, "main"};
    VkPipelineColorBlendStateCreateInfo cbShadow{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, nullptr, 0, VK_FALSE, VK_LOGIC_OP_COPY, 0, nullptr};
    VkPipelineDepthStencilStateCreateInfo dsShadow{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    dsShadow.depthTestEnable = VK_TRUE; dsShadow.depthWriteEnable = VK_TRUE; dsShadow.depthCompareOp = VK_COMPARE_OP_LESS;
    VkGraphicsPipelineCreateInfo gpSh{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpSh.stageCount = 1; gpSh.pStages = &stSh; gpSh.pVertexInputState = &vi3; gpSh.pInputAssemblyState = &iaTri;
    gpSh.pViewportState = &vp; gpSh.pRasterizationState = &rs3; gpSh.pMultisampleState = &ms;
    gpSh.pDepthStencilState = &dsShadow; gpSh.pColorBlendState = &cbShadow; gpSh.pDynamicState = &dyn;
    gpSh.layout = r.layout3D; gpSh.renderPass = r.shadowRenderPass;
    check(vkCreateGraphicsPipelines(r.device, VK_NULL_HANDLE, 1, &gpSh, nullptr, &r.pipeShadow), "pipeShadow");

    // 2D UI pipeline
    VkPipelineShaderStageCreateInfo st2[2]{
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, u2v, "main"},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, u2f, "main"}
    };
    VkVertexInputBindingDescription bind2{0, sizeof(UiVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attr2[3]{
        {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(UiVertex, x)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(UiVertex, u)},
        {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(UiVertex, color)}
    };
    VkPipelineVertexInputStateCreateInfo vi2{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, nullptr, 0, 1, &bind2, 3, attr2};
    gp.pStages = st2; gp.pVertexInputState = &vi2; gp.pInputAssemblyState = &iaTri;
    gp.pDepthStencilState = &ds3; gp.pColorBlendState = &cb;
    gp.layout = r.layout2D; gp.renderPass = r.renderPass;
    check(vkCreateGraphicsPipelines(r.device, VK_NULL_HANDLE, 1, &gp, nullptr, &r.pipeUi2D), "pipeUi2D");

    vkDestroyShaderModule(r.device, shv, nullptr);
    vkDestroyShaderModule(r.device, m3v, nullptr);
    vkDestroyShaderModule(r.device, m3f, nullptr);
    vkDestroyShaderModule(r.device, u2v, nullptr);
    vkDestroyShaderModule(r.device, u2f, nullptr);
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
    createShadowResources();
    createAtlasTexture();
    createPipelines();
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
        destroyDynBuffer(f.vb3d);
        destroyDynBuffer(f.vb2d);
        destroyDynBuffer(f.ubo3d);
        if (f.fence) vkDestroyFence(r.device, f.fence, nullptr);
        if (f.acquire) vkDestroySemaphore(r.device, f.acquire, nullptr);
        if (f.pool) vkDestroyCommandPool(r.device, f.pool, nullptr);
    }
    destroyDynBuffer(r.atlasStage);
    if (r.pipeShadow) vkDestroyPipeline(r.device, r.pipeShadow, nullptr);
    if (r.pipe3DMesh) vkDestroyPipeline(r.device, r.pipe3DMesh, nullptr);
    if (r.pipe3DLines) vkDestroyPipeline(r.device, r.pipe3DLines, nullptr);
    if (r.pipe3DOverlay) vkDestroyPipeline(r.device, r.pipe3DOverlay, nullptr);
    if (r.pipeUi2D) vkDestroyPipeline(r.device, r.pipeUi2D, nullptr);
    if (r.layout3D) vkDestroyPipelineLayout(r.device, r.layout3D, nullptr);
    if (r.layout2D) vkDestroyPipelineLayout(r.device, r.layout2D, nullptr);
    if (r.descPool) vkDestroyDescriptorPool(r.device, r.descPool, nullptr);
    if (r.descLayout3D) vkDestroyDescriptorSetLayout(r.device, r.descLayout3D, nullptr);
    if (r.descLayout) vkDestroyDescriptorSetLayout(r.device, r.descLayout, nullptr);
    if (r.atlasSampler) vkDestroySampler(r.device, r.atlasSampler, nullptr);
    if (r.atlasView) vkDestroyImageView(r.device, r.atlasView, nullptr);
    if (r.atlasImg) vkDestroyImage(r.device, r.atlasImg, nullptr);
    if (r.atlasMem) vkFreeMemory(r.device, r.atlasMem, nullptr);

    if (r.shadowFb) vkDestroyFramebuffer(r.device, r.shadowFb, nullptr);
    if (r.shadowRenderPass) vkDestroyRenderPass(r.device, r.shadowRenderPass, nullptr);
    if (r.shadowSampler) vkDestroySampler(r.device, r.shadowSampler, nullptr);
    if (r.shadowView) vkDestroyImageView(r.device, r.shadowView, nullptr);
    if (r.shadowImg) vkDestroyImage(r.device, r.shadowImg, nullptr);
    if (r.shadowMem) vkFreeMemory(r.device, r.shadowMem, nullptr);

    for (auto s : r.renderFinished) if (s) vkDestroySemaphore(r.device, s, nullptr);
    destroySwapchainViews();
    if (r.renderPass) vkDestroyRenderPass(r.device, r.renderPass, nullptr);
    if (r.swapchain) vkDestroySwapchainKHR(r.device, r.swapchain, nullptr);
    if (r.device) vkDestroyDevice(r.device, nullptr);
    if (r.surface) vkDestroySurfaceKHR(r.instance, r.surface, nullptr);
    if (r.instance) vkDestroyInstance(r.instance, nullptr);
    r = Renderer{};
}

void drawStudio(StudioApp *app, float density) {
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

    if (app) app->buildFrame((float)r.extent.width, (float)r.extent.height, density);

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(f.cmd, &bi);

    // Upload font atlas if updated
    if (app && !app->font.atlasRgba.empty() && app->font.atlasVersion != r.uploadedAtlasVersion) {
        VkDeviceSize bytes = app->font.atlasRgba.size();
        ensureDynBuffer(r.atlasStage, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        std::memcpy(r.atlasStage.mapped, app->font.atlasRgba.data(), bytes);

        VkImageMemoryBarrier toDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr,
            0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, r.atlasImg, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vkCmdPipelineBarrier(f.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &toDst);

        VkBufferImageCopy reg{};
        reg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        reg.imageExtent = {StudioFont::kAtlasSize, StudioFont::kAtlasSize, 1};
        vkCmdCopyBufferToImage(f.cmd, r.atlasStage.buf, r.atlasImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &reg);

        VkImageMemoryBarrier toRead{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, r.atlasImg, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vkCmdPipelineBarrier(f.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &toRead);
        r.uploadedAtlasVersion = app->font.atlasVersion;
    }

    // Pack dynamic vertex buffers + UBO for 3D + 2D
    uint32_t offMesh = 0, offShadow = 0, offLine = 0, offOv = 0;
    uint32_t skyOffset = 0;
    std::vector<uint32_t> batchOffsets;

    if (app) {
        SceneUbo ubo{};
        std::memcpy(ubo.lightVP, app->lightVP, sizeof(ubo.lightVP));
        std::memcpy(ubo.sunDir, app->sunDir, sizeof(ubo.sunDir));
        std::memcpy(ubo.skyTint, app->skyTint, sizeof(ubo.skyTint));
        std::memcpy(f.ubo3d.mapped, &ubo, sizeof(ubo));

        size_t cnt3D = app->frame3DMesh.size() + app->frame3DShadowCasters.size()
                     + app->frame3DLines.size() + app->frame3DOverlayLines.size();
        if (cnt3D > 0) {
            ensureDynBuffer(f.vb3d, cnt3D * sizeof(MeshVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
            uint8_t *dst = static_cast<uint8_t *>(f.vb3d.mapped);
            size_t cur = 0;
            if (!app->frame3DMesh.empty()) {
                std::memcpy(dst + cur * sizeof(MeshVertex), app->frame3DMesh.data(), app->frame3DMesh.size() * sizeof(MeshVertex));
                offMesh = (uint32_t)cur; cur += app->frame3DMesh.size();
            }
            if (!app->frame3DShadowCasters.empty()) {
                std::memcpy(dst + cur * sizeof(MeshVertex), app->frame3DShadowCasters.data(), app->frame3DShadowCasters.size() * sizeof(MeshVertex));
                offShadow = (uint32_t)cur; cur += app->frame3DShadowCasters.size();
            }
            if (!app->frame3DLines.empty()) {
                std::memcpy(dst + cur * sizeof(MeshVertex), app->frame3DLines.data(), app->frame3DLines.size() * sizeof(MeshVertex));
                offLine = (uint32_t)cur; cur += app->frame3DLines.size();
            }
            if (!app->frame3DOverlayLines.empty()) {
                std::memcpy(dst + cur * sizeof(MeshVertex), app->frame3DOverlayLines.data(), app->frame3DOverlayLines.size() * sizeof(MeshVertex));
                offOv = (uint32_t)cur; cur += app->frame3DOverlayLines.size();
            }
        }

        size_t cnt2D = 6; // sky quad
        for (const auto &b : app->uiBatches) cnt2D += b.verts.size();
        ensureDynBuffer(f.vb2d, cnt2D * sizeof(UiVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        auto *dst2 = static_cast<UiVertex *>(f.vb2d.mapped);
        size_t cur2 = 0;

        float u = 1.5f / StudioFont::kAtlasSize;
        uint32_t cTop = StudioFont::rgba((uint8_t)(app->skyTop[0] * 255), (uint8_t)(app->skyTop[1] * 255), (uint8_t)(app->skyTop[2] * 255));
        uint32_t cBot = StudioFont::rgba((uint8_t)(app->skyBot[0] * 255), (uint8_t)(app->skyBot[1] * 255), (uint8_t)(app->skyBot[2] * 255));
        Rect rv = app->rViewport;
        dst2[cur2++] = {rv.x, rv.y, u, u, cTop};
        dst2[cur2++] = {rv.x + rv.w, rv.y, u, u, cTop};
        dst2[cur2++] = {rv.x + rv.w, rv.y + rv.h, u, u, cBot};
        dst2[cur2++] = {rv.x, rv.y, u, u, cTop};
        dst2[cur2++] = {rv.x + rv.w, rv.y + rv.h, u, u, cBot};
        dst2[cur2++] = {rv.x, rv.y + rv.h, u, u, cBot};

        batchOffsets.resize(app->uiBatches.size());
        for (size_t i = 0; i < app->uiBatches.size(); ++i) {
            batchOffsets[i] = (uint32_t)cur2;
            const auto &bv = app->uiBatches[i].verts;
            if (!bv.empty()) {
                std::memcpy(dst2 + cur2, bv.data(), bv.size() * sizeof(UiVertex));
                cur2 += bv.size();
            }
        }
    }

    // 0. Shadow Map pass
    {
        VkClearValue shClr{};
        shClr.depthStencil = {1.0f, 0};
        VkRenderPassBeginInfo shRp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, nullptr, r.shadowRenderPass,
            r.shadowFb, {{0, 0}, {kShadowSize, kShadowSize}}, 1, &shClr};
        vkCmdBeginRenderPass(f.cmd, &shRp, VK_SUBPASS_CONTENTS_INLINE);
        if (app && app->has3DViewportVisible && !app->frame3DShadowCasters.empty() && f.vb3d.buf) {
            VkViewport shVp{0, 0, (float)kShadowSize, (float)kShadowSize, 0.0f, 1.0f};
            VkRect2D shSc{{0, 0}, {kShadowSize, kShadowSize}};
            vkCmdSetViewport(f.cmd, 0, 1, &shVp);
            vkCmdSetScissor(f.cmd, 0, 1, &shSc);
            vkCmdBindPipeline(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.pipeShadow);
            VkDeviceSize zero = 0;
            vkCmdBindVertexBuffers(f.cmd, 0, 1, &f.vb3d.buf, &zero);
            Push3D shPush{};
            std::memcpy(shPush.mvp, app->lightVP, sizeof(shPush.mvp));
            vkCmdPushConstants(f.cmd, r.layout3D, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push3D), &shPush);
            vkCmdDraw(f.cmd, (uint32_t)app->frame3DShadowCasters.size(), 1, offShadow, 0);
        }
        vkCmdEndRenderPass(f.cmd);
    }

    VkClearValue clears[2]{};
    clears[0].color = {{0.09f, 0.10f, 0.12f, 1.0f}};
    clears[1].depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, nullptr, r.renderPass,
        r.framebuffers[img], {{0, 0}, r.extent}, 2, clears};
    vkCmdBeginRenderPass(f.cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

    auto setFullVp = [&]() {
        VkViewport vp{0, 0, (float)r.extent.width, (float)r.extent.height, 0.0f, 1.0f};
        VkRect2D sc{{0, 0}, r.extent};
        vkCmdSetViewport(f.cmd, 0, 1, &vp);
        vkCmdSetScissor(f.cmd, 0, 1, &sc);
    };

    if (app) {
        Push2D p2{{2.0f / (float)r.extent.width, 2.0f / (float)r.extent.height}};
        VkDeviceSize zero = 0;

        // 1. Sky + 3D pass inside rViewport
        if (app->has3DViewportVisible && app->rViewport.w > 2 && app->rViewport.h > 2) {
            int32_t vx = std::max(0, (int32_t)app->rViewport.x);
            int32_t vy = std::max(0, (int32_t)app->rViewport.y);
            uint32_t vw = std::min(r.extent.width - (uint32_t)vx, (uint32_t)std::max(1.0f, app->rViewport.w));
            uint32_t vh = std::min(r.extent.height - (uint32_t)vy, (uint32_t)std::max(1.0f, app->rViewport.h));
            VkRect2D vSc{{vx, vy}, {vw, vh}};

            setFullVp();
            vkCmdSetScissor(f.cmd, 0, 1, &vSc);
            vkCmdBindPipeline(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.pipeUi2D);
            vkCmdBindDescriptorSets(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.layout2D, 0, 1, &r.descSet, 0, nullptr);
            vkCmdPushConstants(f.cmd, r.layout2D, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Push2D), &p2);
            vkCmdBindVertexBuffers(f.cmd, 0, 1, &f.vb2d.buf, &zero);
            vkCmdDraw(f.cmd, 6, 1, skyOffset, 0);

            VkViewport vp3{(float)vx, (float)vy, (float)vw, (float)vh, 0.0f, 1.0f};
            vkCmdSetViewport(f.cmd, 0, 1, &vp3);
            vkCmdSetScissor(f.cmd, 0, 1, &vSc);

            if (f.vb3d.buf) {
                vkCmdBindVertexBuffers(f.cmd, 0, 1, &f.vb3d.buf, &zero);
                vkCmdBindDescriptorSets(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.layout3D, 0, 1, &f.descSet3D, 0, nullptr);
                Push3D p3{};
                std::memcpy(p3.mvp, app->viewProj, sizeof(p3.mvp));
                p3.flags[0] = 0.0f; // lit + shadows

                if (!app->frame3DMesh.empty()) {
                    vkCmdBindPipeline(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.pipe3DMesh);
                    vkCmdPushConstants(f.cmd, r.layout3D, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push3D), &p3);
                    vkCmdDraw(f.cmd, (uint32_t)app->frame3DMesh.size(), 1, offMesh, 0);
                }
                p3.flags[0] = 1.0f; // unlit lines
                if (!app->frame3DLines.empty()) {
                    vkCmdBindPipeline(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.pipe3DLines);
                    vkCmdPushConstants(f.cmd, r.layout3D, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push3D), &p3);
                    vkCmdDraw(f.cmd, (uint32_t)app->frame3DLines.size(), 1, offLine, 0);
                }
                if (!app->frame3DOverlayLines.empty()) {
                    vkCmdBindPipeline(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.pipe3DOverlay);
                    vkCmdPushConstants(f.cmd, r.layout3D, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push3D), &p3);
                    vkCmdDraw(f.cmd, (uint32_t)app->frame3DOverlayLines.size(), 1, offOv, 0);
                }
            }
        }

        // 2. 2D Studio UI batches
        setFullVp();
        vkCmdBindPipeline(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.pipeUi2D);
        vkCmdBindDescriptorSets(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.layout2D, 0, 1, &r.descSet, 0, nullptr);
        vkCmdPushConstants(f.cmd, r.layout2D, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Push2D), &p2);
        vkCmdBindVertexBuffers(f.cmd, 0, 1, &f.vb2d.buf, &zero);

        for (size_t i = 0; i < app->uiBatches.size(); ++i) {
            const auto &b = app->uiBatches[i];
            if (b.verts.empty()) continue;
            if (b.hasScissor) {
                int32_t sx = std::max(0, (int32_t)b.scissor.x);
                int32_t sy = std::max(0, (int32_t)b.scissor.y);
                int32_t sw = std::min((int32_t)r.extent.width - sx, (int32_t)std::ceil(b.scissor.w));
                int32_t sh = std::min((int32_t)r.extent.height - sy, (int32_t)std::ceil(b.scissor.h));
                if (sw <= 0 || sh <= 0) continue;
                VkRect2D sc{{sx, sy}, {(uint32_t)sw, (uint32_t)sh}};
                vkCmdSetScissor(f.cmd, 0, 1, &sc);
            } else {
                VkRect2D sc{{0, 0}, r.extent};
                vkCmdSetScissor(f.cmd, 0, 1, &sc);
            }
            vkCmdDraw(f.cmd, (uint32_t)b.verts.size(), 1, batchOffsets[i], 0);
        }
    }

    vkCmdEndRenderPass(f.cmd);
    if (vkEndCommandBuffer(f.cmd) != VK_SUCCESS) return;

    VkSemaphore fin = r.renderFinished[img];
    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo sub{VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 1, &f.acquire, &wait, 1, &f.cmd, 1, &fin};
    if (vkQueueSubmit(r.queue, 1, &sub, f.fence) != VK_SUCCESS) return;

    VkPresentInfoKHR pres{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, nullptr, 1, &fin, 1, &r.swapchain, &img};
    VkResult presRes = vkQueuePresentKHR(r.queue, &pres);
    r.frameIndex = (r.frameIndex + 1) % kFrames;
    if (acq == VK_SUBOPTIMAL_KHR || presRes == VK_ERROR_OUT_OF_DATE_KHR || presRes == VK_SUBOPTIMAL_KHR) recreateSwapchain();
}
} // namespace

bool androidVulkanCreate(JNIEnv *env, jobject surface, int width, int height) {
    try {
        destroy();
        r.requestedW = width > 0 ? (uint32_t)width : 0;
        r.requestedH = height > 0 ? (uint32_t)height : 0;
        ANativeWindow *window = ANativeWindow_fromSurface(env, surface);
        if (!window) return false;
        initialize(window);
        ANativeWindow_release(window);
        LOGI("Vulkan %u.%u Studio renderer (%ux%u)", VK_VERSION_MAJOR(r.apiVersion),
             VK_VERSION_MINOR(r.apiVersion), r.extent.width, r.extent.height);
        return true;
    } catch (const std::exception &error) {
        LOGE("Initialization failed: %s", error.what());
        destroy();
        return false;
    }
}

void androidVulkanResize(int width, int height) {
    if (!r.device || !r.swapchain) return;
    if (width > 0 && height > 0) {
        r.requestedW = (uint32_t)width;
        r.requestedH = (uint32_t)height;
    }
    try {
        recreateSwapchain();
    } catch (const std::exception &error) {
        LOGE("Resize swapchain failed: %s", error.what());
    }
}

void androidVulkanDrawStudio(StudioApp *app, float density) {
    drawStudio(app, density);
}

void androidVulkanDraw() {
    drawStudio(nullptr, 3.0f);
}

void androidVulkanDestroy() {
    destroy();
}
