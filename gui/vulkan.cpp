#include "vulkan.hpp"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STB_IMAGE_IMPLEMENTATION
#include "../src/third_party/stb_image.h"

namespace {
void vk(bool ok, const char *msg) { if (!ok) throw std::runtime_error(msg); }
void vk(VkResult res, const char *msg) { if (res != VK_SUCCESS) throw std::runtime_error(msg); }

bool hasExt(const std::vector<VkExtensionProperties> &exts, const char *name) {
    for (const auto &e : exts) if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
}

#ifdef __APPLE__
bool hasInstanceExt(const char *name) {
    uint32_t n = 0;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr) != VK_SUCCESS || !n) return false;
    std::vector<VkExtensionProperties> exts(n);
    return vkEnumerateInstanceExtensionProperties(nullptr, &n, exts.data()) == VK_SUCCESS && hasExt(exts, name);
}
#endif

VkShaderModule shaderModule(VkDevice dev, const std::vector<char> &bytes) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes.size();
    ci.pCode = reinterpret_cast<const uint32_t *>(bytes.data());
    VkShaderModule mod = VK_NULL_HANDLE;
    vk(vkCreateShaderModule(dev, &ci, nullptr, &mod), "vkCreateShaderModule");
    return mod;
}
}

std::vector<char> VulkanRenderer::readShader(const char *name) const {
    std::string path = shaders_ + "/" + name;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("cannot open shader: " + path);
    std::streamsize size = file.tellg();
    file.seekg(0);
    std::vector<char> bytes(static_cast<size_t>(size));
    file.read(bytes.data(), size);
    return bytes;
}

void VulkanRenderer::createInstance() {
    uint32_t ver = VK_API_VERSION_1_0;
    if (auto fn = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion")))
        if (fn(&ver) != VK_SUCCESS) ver = VK_API_VERSION_1_0;
    instanceApiVersion_ = VK_API_VERSION_1_0;
#ifdef VK_API_VERSION_1_4
    if (ver >= VK_API_VERSION_1_4) instanceApiVersion_ = VK_API_VERSION_1_4; else
#endif
#ifdef VK_API_VERSION_1_3
    if (ver >= VK_API_VERSION_1_3) instanceApiVersion_ = VK_API_VERSION_1_3; else
#endif
#ifdef VK_API_VERSION_1_2
    if (ver >= VK_API_VERSION_1_2) instanceApiVersion_ = VK_API_VERSION_1_2; else
#endif
    if (ver >= VK_API_VERSION_1_1) instanceApiVersion_ = VK_API_VERSION_1_1;

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "LuaC Studio";
    app.applicationVersion = VK_MAKE_VERSION(0, 5, 0);
    app.pEngineName = "LuaC";
    app.engineVersion = VK_MAKE_VERSION(0, 5, 0);
    app.apiVersion = instanceApiVersion_;

    uint32_t count = 0;
    const char **exts = glfwGetRequiredInstanceExtensions(&count);
    vk(exts && count, "GLFW Vulkan extensions are unavailable");
    std::vector<const char *> enabled(exts, exts + count);
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &app;
#ifdef __APPLE__
    if (hasInstanceExt(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        enabled.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }
#endif
    info.enabledExtensionCount = static_cast<uint32_t>(enabled.size());
    info.ppEnabledExtensionNames = enabled.data();
    vk(vkCreateInstance(&info, nullptr, &instance_), "Vulkan instance is unavailable");
    vk(glfwCreateWindowSurface(instance_, window_, nullptr, &surface_), "Vulkan surface creation failed");
}

void VulkanRenderer::selectDevice() {
    uint32_t count = 0;
    vk(vkEnumeratePhysicalDevices(instance_, &count, nullptr), "vkEnumeratePhysicalDevices");
    if (!count) throw std::runtime_error("no Vulkan physical device");
    std::vector<VkPhysicalDevice> devs(count);
    vk(vkEnumeratePhysicalDevices(instance_, &count, devs.data()), "vkEnumeratePhysicalDevices");

    uint32_t bestScore = 0;
    VkPhysicalDeviceProperties bestProps{};
    std::vector<VkExtensionProperties> bestExts;
    for (VkPhysicalDevice cand : devs) {
        uint32_t fc = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(cand, &fc, nullptr);
        std::vector<VkQueueFamilyProperties> fams(fc);
        vkGetPhysicalDeviceQueueFamilyProperties(cand, &fc, fams.data());
        uint32_t fam = UINT32_MAX;
        for (uint32_t i = 0; i < fc; ++i) {
            VkBool32 sup = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(cand, i, surface_, &sup);
            if ((fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && sup) { fam = i; break; }
        }
        if (fam == UINT32_MAX) continue;
        uint32_t ec = 0;
        vkEnumerateDeviceExtensionProperties(cand, nullptr, &ec, nullptr);
        std::vector<VkExtensionProperties> exts(ec);
        vkEnumerateDeviceExtensionProperties(cand, nullptr, &ec, exts.data());
        if (!hasExt(exts, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;

        VkPhysicalDeviceProperties props{};
        VkPhysicalDeviceMemoryProperties mem{};
        vkGetPhysicalDeviceProperties(cand, &props);
        vkGetPhysicalDeviceMemoryProperties(cand, &mem);
        uint32_t score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 100000 :
                         props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 20000 :
                         props.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU ? 5000 : 1000;
        for (uint32_t i = 0; i < mem.memoryHeapCount; ++i)
            if (mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                score += static_cast<uint32_t>(std::min<uint64_t>(mem.memoryHeaps[i].size >> 20, 32768));
        if (physical_ == VK_NULL_HANDLE || score > bestScore) {
            physical_ = cand; queueFamily_ = fam; bestScore = score;
            bestProps = props; bestExts = std::move(exts);
        }
    }
    if (physical_ == VK_NULL_HANDLE) throw std::runtime_error("no graphics/present Vulkan device");
    deviceApiVersion_ = std::min(instanceApiVersion_, bestProps.apiVersion);

    std::vector<const char *> enabledExts{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    if (hasExt(bestExts, "VK_KHR_portability_subset")) enabledExts.push_back("VK_KHR_portability_subset");

    bool enDR = false, enSync2 = false, enPush = false;
    auto getFeat2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
        vkGetInstanceProcAddr(instance_, "vkGetPhysicalDeviceFeatures2"));

#ifdef VK_API_VERSION_1_3
    bool has13 = deviceApiVersion_ >= VK_API_VERSION_1_3;
    VkPhysicalDeviceVulkan13Features vk13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan13Features enVk13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
#endif
#ifdef VK_API_VERSION_1_4
    bool has14 = deviceApiVersion_ >= VK_API_VERSION_1_4;
    VkPhysicalDeviceVulkan14Features vk14{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};
    VkPhysicalDeviceVulkan14Features enVk14{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};
#endif

    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceFeatures2 enF2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    if (deviceApiVersion_ >= VK_API_VERSION_1_1 && getFeat2) {
        void **qNext = &f2.pNext;
        void **eNext = &enF2.pNext;
#ifdef VK_API_VERSION_1_3
        if (has13) { *qNext = &vk13; qNext = &vk13.pNext; }
#endif
#ifdef VK_API_VERSION_1_4
        if (has14) { *qNext = &vk14; qNext = &vk14.pNext; }
#endif
        getFeat2(physical_, &f2);
#ifdef VK_API_VERSION_1_3
        if (has13) {
            if (vk13.dynamicRendering) { enVk13.dynamicRendering = VK_TRUE; enDR = true; }
            if (vk13.synchronization2) { enVk13.synchronization2 = VK_TRUE; enSync2 = true; }
            if (enDR || enSync2) { *eNext = &enVk13; eNext = &enVk13.pNext; }
        }
#endif
#ifdef VK_API_VERSION_1_4
        if (has14 && vk14.pushDescriptor) {
            enVk14.pushDescriptor = VK_TRUE; enPush = true;
            *eNext = &enVk14; eNext = &enVk14.pNext;
        }
#endif
    }

#ifdef VK_KHR_push_descriptor
    if (!enPush && hasExt(bestExts, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)) {
        enabledExts.push_back(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
        enPush = true;
    }
#endif

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queueFamily_; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = static_cast<uint32_t>(enabledExts.size());
    dci.ppEnabledExtensionNames = enabledExts.data();
    if (enF2.pNext) dci.pNext = &enF2;

    vk(vkCreateDevice(physical_, &dci, nullptr, &device_), "Vulkan logical device creation failed");
    vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);

#ifdef VK_API_VERSION_1_3
    if (enDR) {
        pfnBeginRendering_ = reinterpret_cast<PFN_vkCmdBeginRendering>(vkGetDeviceProcAddr(device_, "vkCmdBeginRendering"));
        pfnEndRendering_ = reinterpret_cast<PFN_vkCmdEndRendering>(vkGetDeviceProcAddr(device_, "vkCmdEndRendering"));
        useDynamicRendering_ = pfnBeginRendering_ && pfnEndRendering_;
    }
    if (enSync2) {
        pfnBarrier2_ = reinterpret_cast<PFN_vkCmdPipelineBarrier2>(vkGetDeviceProcAddr(device_, "vkCmdPipelineBarrier2"));
        pfnSubmit2_ = reinterpret_cast<PFN_vkQueueSubmit2>(vkGetDeviceProcAddr(device_, "vkQueueSubmit2"));
        useSync2_ = pfnBarrier2_ && pfnSubmit2_;
    }
#endif
#ifdef VK_KHR_push_descriptor
    if (enPush) {
        pfnPushDescriptor_ = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(vkGetDeviceProcAddr(device_, "vkCmdPushDescriptorSet"));
        if (!pfnPushDescriptor_)
            pfnPushDescriptor_ = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(vkGetDeviceProcAddr(device_, "vkCmdPushDescriptorSetKHR"));
        usePushDescriptors_ = pfnPushDescriptor_ != nullptr;
    }
#endif
}

void VulkanRenderer::createSwapchain(VkSwapchainKHR oldSwapchain) {
    VkSurfaceCapabilitiesKHR caps{};
    vk(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps), "surface capabilities");
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &count, fmts.data());
    if (!count) throw std::runtime_error("no Vulkan surface formats");
    VkSurfaceFormatKHR sel = fmts[0];
    for (const auto &f : fmts)
        if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { sel = f; break; }
    format_ = sel.format;

    uint32_t pmCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &pmCount, nullptr);
    std::vector<VkPresentModeKHR> pms(pmCount);
    if (pmCount) vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &pmCount, pms.data());
    VkPresentModeKHR pm = VK_PRESENT_MODE_FIFO_KHR;
    for (auto m : pms) {
        if (m == VK_PRESENT_MODE_MAILBOX_KHR) { pm = m; break; }
        if (m == VK_PRESENT_MODE_FIFO_RELAXED_KHR) pm = m;
    }

    int w = 0, h = 0;
    glfwGetFramebufferSize(window_, &w, &h);
    extent_ = caps.currentExtent;
    if (extent_.width == UINT32_MAX) {
        extent_.width = std::clamp(static_cast<uint32_t>(std::max(w, 1)), caps.minImageExtent.width, caps.maxImageExtent.width);
        extent_.height = std::clamp(static_cast<uint32_t>(std::max(h, 1)), caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    uint32_t imgCount = (pm == VK_PRESENT_MODE_MAILBOX_KHR) ? std::max(3u, caps.minImageCount) : (caps.minImageCount + 1);
    if (caps.maxImageCount && imgCount > caps.maxImageCount) imgCount = caps.maxImageCount;

    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    for (auto a : {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                   VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR})
        if (caps.supportedCompositeAlpha & a) { alpha = a; break; }

    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = surface_; sci.minImageCount = imgCount; sci.imageFormat = format_;
    sci.imageColorSpace = sel.colorSpace; sci.imageExtent = extent_; sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform; sci.compositeAlpha = alpha;
    sci.presentMode = pm; sci.clipped = VK_TRUE; sci.oldSwapchain = oldSwapchain;

    VkSwapchainKHR next = VK_NULL_HANDLE;
    vk(vkCreateSwapchainKHR(device_, &sci, nullptr, &next), "Vulkan swapchain creation failed");
    if (oldSwapchain) vkDestroySwapchainKHR(device_, oldSwapchain, nullptr);
    swapchain_ = next;

    vkGetSwapchainImagesKHR(device_, swapchain_, &count, nullptr);
    images_.resize(count); views_.resize(count);
    vkGetSwapchainImagesKHR(device_, swapchain_, &count, images_.data());
    for (uint32_t i = 0; i < count; ++i) {
        VkImageViewCreateInfo iv{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        iv.image = images_[i]; iv.viewType = VK_IMAGE_VIEW_TYPE_2D; iv.format = format_;
        iv.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vk(vkCreateImageView(device_, &iv, nullptr, &views_[i]), "Vulkan image view creation failed");
    }

    for (auto s : renderFinished_) if (s) vkDestroySemaphore(device_, s, nullptr);
    renderFinished_.assign(count, VK_NULL_HANDLE);
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t i = 0; i < count; ++i) vk(vkCreateSemaphore(device_, &si, nullptr, &renderFinished_[i]), "Vulkan semaphore failed");
    imagesInFlight_.assign(count, VK_NULL_HANDLE);

    if (!useDynamicRendering_ && renderPass_) {
        framebuffers_.resize(count);
        for (uint32_t i = 0; i < count; ++i) {
            VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fb.renderPass = renderPass_; fb.attachmentCount = 1; fb.pAttachments = &views_[i];
            fb.width = extent_.width; fb.height = extent_.height; fb.layers = 1;
            vk(vkCreateFramebuffer(device_, &fb, nullptr, &framebuffers_[i]), "Vulkan framebuffer creation failed");
        }
    }
}

void VulkanRenderer::createPipeline() {
    if (!pipelineLayout_) {
        VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo dsl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dsl.bindingCount = 1; dsl.pBindings = &b;
#ifdef VK_KHR_push_descriptor
        if (usePushDescriptors_) dsl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
#endif
        vk(vkCreateDescriptorSetLayout(device_, &dsl, nullptr, &descriptorLayout_), "Vulkan texture descriptor layout failed");
        VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float) * 2};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 1; pl.pSetLayouts = &descriptorLayout_;
        pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &push;
        vk(vkCreatePipelineLayout(device_, &pl, nullptr, &pipelineLayout_), "Vulkan pipeline layout failed");
    }
    if (!pipelineCache_) {
        VkPipelineCacheCreateInfo pcc{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        vk(vkCreatePipelineCache(device_, &pcc, nullptr, &pipelineCache_), "Vulkan pipeline cache failed");
    }
    if (!useDynamicRendering_) {
        VkAttachmentDescription att{};
        att.format = format_; att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{}; sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1; sub.pColorAttachments = &ref;
        VkSubpassDependency dep{}; dep.srcSubpass = VK_SUBPASS_EXTERNAL; dep.dstSubpass = 0;
        dep.srcStageMask = dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rp.attachmentCount = 1; rp.pAttachments = &att; rp.subpassCount = 1; rp.pSubpasses = &sub;
        rp.dependencyCount = 1; rp.pDependencies = &dep;
        vk(vkCreateRenderPass(device_, &rp, nullptr, &renderPass_), "Vulkan render pass creation failed");

        framebuffers_.resize(views_.size());
        for (size_t i = 0; i < views_.size(); ++i) {
            VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fb.renderPass = renderPass_; fb.attachmentCount = 1; fb.pAttachments = &views_[i];
            fb.width = extent_.width; fb.height = extent_.height; fb.layers = 1;
            vk(vkCreateFramebuffer(device_, &fb, nullptr, &framebuffers_[i]), "Vulkan framebuffer creation failed");
        }
    }

    VkShaderModule vert = shaderModule(device_, readShader("shader.vert.spv"));
    VkShaderModule frag = shaderModule(device_, readShader("shader.frag.spv"));
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vert; stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = frag; stages[1].pName = "main";

    VkVertexInputBindingDescription binding{0, sizeof(LcVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[3]{
        {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(LcVertex, x)},
        {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(LcVertex, r)},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(LcVertex, u)}
    };
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = 3; vi.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport vp{0, 0, 1, 1, 0, 1}; VkRect2D sc{{0, 0}, {1, 1}};
    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = 1; vps.pViewports = &vp; vps.scissorCount = 1; vps.pScissors = &sc;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState ba{};
    ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    ba.blendEnable = VK_TRUE;
    ba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA; ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1; cb.pAttachments = &ba;
    VkDynamicState dynStates[]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2; dyn.pDynamicStates = dynStates;

    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.stageCount = 2; gp.pStages = stages; gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vps; gp.pRasterizationState = &rs; gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cb; gp.pDynamicState = &dyn; gp.layout = pipelineLayout_;
    gp.renderPass = useDynamicRendering_ ? VK_NULL_HANDLE : renderPass_;
#ifdef VK_API_VERSION_1_3
    VkPipelineRenderingCreateInfo rc{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    if (useDynamicRendering_) { rc.colorAttachmentCount = 1; rc.pColorAttachmentFormats = &format_; gp.pNext = &rc; }
#endif
    vk(vkCreateGraphicsPipelines(device_, pipelineCache_, 1, &gp, nullptr, &pipeline_), "Vulkan graphics pipeline failed");
    pipelineFormat_ = format_;
    vkDestroyShaderModule(device_, frag, nullptr);
    vkDestroyShaderModule(device_, vert, nullptr);
}

uint32_t VulkanRenderer::findMemoryType(uint32_t bits, VkMemoryPropertyFlags req,
                                        VkMemoryPropertyFlags pref, bool &coherent) const {
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(physical_, &props);
    for (VkMemoryPropertyFlags want : {pref, req}) {
        for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
            if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want) {
                coherent = (props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
                return i;
            }
        }
    }
    throw std::runtime_error("required Vulkan memory type is unavailable");
}

void VulkanRenderer::destroyBuffer(Buffer &buf) const {
    if (buf.mapped && buf.memory) vkUnmapMemory(device_, buf.memory);
    if (buf.handle) vkDestroyBuffer(device_, buf.handle, nullptr);
    if (buf.memory) vkFreeMemory(device_, buf.memory, nullptr);
    buf = Buffer{};
}

void VulkanRenderer::ensureBuffer(Buffer &buf, VkDeviceSize bytes, VkBufferUsageFlags usage, bool preferDeviceLocal) {
    if (bytes <= buf.capacity) return;
    VkDeviceSize newCap = std::max<VkDeviceSize>(bytes, std::max<VkDeviceSize>(256 * 1024, buf.capacity * 2));
    destroyBuffer(buf);
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = newCap; ci.usage = usage; ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vk(vkCreateBuffer(device_, &ci, nullptr, &buf.handle), "Vulkan buffer creation failed");
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device_, buf.handle, &req);
    VkMemoryPropertyFlags pref = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                 (preferDeviceLocal ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT : 0);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, pref, buf.coherent);
    vk(vkAllocateMemory(device_, &ai, nullptr, &buf.memory), "Vulkan buffer memory failed");
    vk(vkBindBufferMemory(device_, buf.handle, buf.memory, 0), "Vulkan buffer bind failed");
    vk(vkMapMemory(device_, buf.memory, 0, newCap, 0, &buf.mapped), "Vulkan buffer map failed");
    buf.capacity = newCap;
}

void VulkanRenderer::createFrameResources() {
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.queueFamilyIndex = queueFamily_;
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (auto &f : frames_) {
        vk(vkCreateCommandPool(device_, &pci, nullptr, &f.pool), "Vulkan command pool failed");
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = f.pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
        vk(vkAllocateCommandBuffers(device_, &ai, &f.cmd), "Vulkan command buffer failed");
        vk(vkCreateSemaphore(device_, &si, nullptr, &f.acquire), "Vulkan semaphore failed");
        vk(vkCreateFence(device_, &fi, nullptr, &f.fence), "Vulkan fence failed");
        ensureBuffer(f.vertices, 256 * 1024, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
    }
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    vk(vkCreateCommandPool(device_, &pci, nullptr, &uploadPool_), "Vulkan upload pool failed");
    VkCommandBufferAllocateInfo uai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    uai.commandPool = uploadPool_; uai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; uai.commandBufferCount = 1;
    vk(vkAllocateCommandBuffers(device_, &uai, &uploadCmd_), "Vulkan upload command buffer failed");
    VkFenceCreateInfo ufi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vk(vkCreateFence(device_, &ufi, nullptr, &uploadFence_), "Vulkan upload fence failed");
}

void VulkanRenderer::initialize(GLFWwindow *window, const std::string &shaderDirectory) {
    window_ = window; shaders_ = shaderDirectory;
    createInstance(); selectDevice(); createSwapchain(); createPipeline(); createFrameResources(); createDefaultTextures();
}

void VulkanRenderer::recreateSwapchain() {
    int w = 0, h = 0;
    do {
        glfwGetFramebufferSize(window_, &w, &h);
        if ((!w || !h) && !glfwWindowShouldClose(window_)) glfwWaitEvents();
    } while ((!w || !h) && !glfwWindowShouldClose(window_));
    if (glfwWindowShouldClose(window_)) return;
    vkDeviceWaitIdle(device_);
    for (auto fb : framebuffers_) if (fb) vkDestroyFramebuffer(device_, fb, nullptr);
    for (auto v : views_) if (v) vkDestroyImageView(device_, v, nullptr);
    framebuffers_.clear(); views_.clear(); images_.clear(); imagesInFlight_.clear();
    VkSwapchainKHR old = swapchain_; swapchain_ = VK_NULL_HANDLE;
    createSwapchain(old);
    if (format_ != pipelineFormat_) {
        if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
        if (renderPass_) vkDestroyRenderPass(device_, renderPass_, nullptr);
        pipeline_ = VK_NULL_HANDLE; renderPass_ = VK_NULL_HANDLE;
        createPipeline();
    }
}

void VulkanRenderer::transitionImage(VkCommandBuffer cmd, VkImage img, VkImageLayout oldL,
                                     VkImageLayout newL, bool syncRender) const {
    auto aspect = VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
#ifdef VK_API_VERSION_1_3
    if (useSync2_) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.oldLayout = oldL; b.newLayout = newL; b.image = img; b.subresourceRange = aspect;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        if (syncRender) {
            bool toPresent = newL == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            b.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            b.srcAccessMask = toPresent ? VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT : 0;
            b.dstStageMask = toPresent ? 0 : VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            b.dstAccessMask = toPresent ? 0 : VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        } else if (newL == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
            b.srcStageMask = oldL == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            b.srcAccessMask = oldL == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT; b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        } else {
            b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT; b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT; b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        }
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1; dep.pImageMemoryBarriers = &b;
        pfnBarrier2_(cmd, &dep);
        return;
    }
#endif
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = oldL; b.newLayout = newL; b.image = img; b.subresourceRange = aspect;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    VkPipelineStageFlags srcS, dstS;
    if (syncRender) {
        bool toPresent = newL == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        srcS = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dstS = toPresent ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        b.srcAccessMask = toPresent ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : 0;
        b.dstAccessMask = toPresent ? 0 : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    } else if (newL == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        srcS = oldL == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dstS = VK_PIPELINE_STAGE_TRANSFER_BIT;
        b.srcAccessMask = oldL == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_SHADER_READ_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    } else {
        srcS = VK_PIPELINE_STAGE_TRANSFER_BIT; dstS = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    }
    vkCmdPipelineBarrier(cmd, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
}

void VulkanRenderer::recordPendingUploads(Frame &frame) {
    if (pendingUploads_.empty()) return;
    VkDeviceSize total = 0;
    for (const auto &u : pendingUploads_) total += u.rgba.size();
    ensureBuffer(frame.staging, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false);

    VkDeviceSize cursor = 0;
    for (const auto &u : pendingUploads_) {
        std::memcpy(static_cast<unsigned char *>(frame.staging.mapped) + cursor, u.rgba.data(), u.rgba.size());
        Texture &tex = textures_[u.id];
        transitionImage(frame.cmd, tex.image, tex.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, false);
        VkBufferImageCopy copy{cursor, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {u.x, u.y, 0},
                               {static_cast<uint32_t>(u.width), static_cast<uint32_t>(u.height), 1}};
        vkCmdCopyBufferToImage(frame.cmd, frame.staging.handle, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        transitionImage(frame.cmd, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false);
        tex.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        cursor += u.rgba.size();
    }
    if (!frame.staging.coherent) {
        VkMappedMemoryRange mr{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, frame.staging.memory, 0, VK_WHOLE_SIZE};
        vkFlushMappedMemoryRanges(device_, 1, &mr);
    }
    pendingUploads_.clear();
}

void VulkanRenderer::render(const std::vector<LcVertex> &vertices, const std::vector<LcDraw> &draws,
                            int pixelWidth, int pixelHeight) {
    if (!device_ || vertices.empty() || draws.empty() || pixelWidth <= 0 || pixelHeight <= 0) return;
    if (!extent_.width || !extent_.height) { recreateSwapchain(); return; }

    Frame &frame = frames_[frameIndex_];
    vk(vkWaitForFences(device_, 1, &frame.fence, VK_TRUE, UINT64_MAX), "Vulkan wait fence failed");
    uint32_t image = 0;
    VkResult acq = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, frame.acquire, VK_NULL_HANDLE, &image);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) { recreateSwapchain(); return; }
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) throw std::runtime_error("Vulkan acquire failed");

    if (imagesInFlight_[image] && imagesInFlight_[image] != frame.fence)
        vk(vkWaitForFences(device_, 1, &imagesInFlight_[image], VK_TRUE, UINT64_MAX), "Vulkan image fence failed");
    imagesInFlight_[image] = frame.fence;
    vk(vkResetFences(device_, 1, &frame.fence), "Vulkan reset fence failed");
    vk(vkResetCommandPool(device_, frame.pool, 0), "Vulkan reset command pool failed");

    VkDeviceSize vbytes = vertices.size() * sizeof(LcVertex);
    ensureBuffer(frame.vertices, vbytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
    std::memcpy(frame.vertices.mapped, vertices.data(), vbytes);
    if (!frame.vertices.coherent) {
        VkMappedMemoryRange mr{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, frame.vertices.memory, 0, VK_WHOLE_SIZE};
        vkFlushMappedMemoryRanges(device_, 1, &mr);
    }

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk(vkBeginCommandBuffer(frame.cmd, &bi), "Vulkan begin command buffer failed");
    recordPendingUploads(frame);

    VkClearValue clear{}; clear.color = {{0.027f, 0.035f, 0.055f, 1.0f}};
#ifdef VK_API_VERSION_1_3
    if (useDynamicRendering_) {
        transitionImage(frame.cmd, images_[image], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, true);
        VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        att.imageView = views_[image]; att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; att.storeOp = VK_ATTACHMENT_STORE_OP_STORE; att.clearValue = clear;
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, extent_}; ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &att;
        pfnBeginRendering_(frame.cmd, &ri);
    } else
#endif
    {
        VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rp.renderPass = renderPass_; rp.framebuffer = framebuffers_[image];
        rp.renderArea = {{0, 0}, extent_}; rp.clearValueCount = 1; rp.pClearValues = &clear;
        vkCmdBeginRenderPass(frame.cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    }

    vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    VkViewport vp{0, 0, static_cast<float>(extent_.width), static_cast<float>(extent_.height), 0, 1};
    vkCmdSetViewport(frame.cmd, 0, 1, &vp);
    float scale[2]{2.0f / static_cast<float>(pixelWidth), 2.0f / static_cast<float>(pixelHeight)};
    vkCmdPushConstants(frame.cmd, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(scale), scale);
    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(frame.cmd, 0, 1, &frame.vertices.handle, &off);

    uint32_t lastTex = UINT32_MAX;
    VkRect2D lastClip{{INT32_MIN, INT32_MIN}, {0, 0}};
    for (const LcDraw &draw : draws) {
        if (!draw.count) continue;
        if (draw.first > vertices.size() || draw.count > vertices.size() - draw.first)
            throw std::runtime_error("invalid Nuklear Vulkan draw batch");
        int x = std::max(0, draw.x), y = std::max(0, draw.y);
        uint32_t right = std::min(extent_.width, static_cast<uint32_t>(x) + draw.width);
        uint32_t bottom = std::min(extent_.height, static_cast<uint32_t>(y) + draw.height);
        if (static_cast<uint32_t>(x) >= right || static_cast<uint32_t>(y) >= bottom) continue;
        VkRect2D clip{{x, y}, {right - static_cast<uint32_t>(x), bottom - static_cast<uint32_t>(y)}};
        if (clip.offset.x != lastClip.offset.x || clip.offset.y != lastClip.offset.y ||
            clip.extent.width != lastClip.extent.width || clip.extent.height != lastClip.extent.height) {
            vkCmdSetScissor(frame.cmd, 0, 1, &clip);
            lastClip = clip;
        }
        uint32_t texId = draw.texture < textures_.size() ? draw.texture : 0;
        if (texId != lastTex) {
#ifdef VK_KHR_push_descriptor
            if (usePushDescriptors_) {
                VkDescriptorImageInfo ii{textures_[texId].sampler, textures_[texId].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                w.descriptorCount = 1; w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &ii;
                pfnPushDescriptor_(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1, &w);
            } else
#endif
            vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1, &textures_[texId].set, 0, nullptr);
            lastTex = texId;
        }
        vkCmdDraw(frame.cmd, draw.count, 1, draw.first, 0);
    }

#ifdef VK_API_VERSION_1_3
    if (useDynamicRendering_) {
        pfnEndRendering_(frame.cmd);
        transitionImage(frame.cmd, images_[image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, true);
    } else
#endif
    vkCmdEndRenderPass(frame.cmd);
    vk(vkEndCommandBuffer(frame.cmd), "Vulkan end command buffer failed");

    VkSemaphore fin = renderFinished_[image];
#ifdef VK_API_VERSION_1_3
    if (useSync2_) {
        VkSemaphoreSubmitInfo w{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, frame.acquire, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0};
        VkCommandBufferSubmitInfo c{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, nullptr, frame.cmd, 0};
        VkSemaphoreSubmitInfo s{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, fin, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0};
        VkSubmitInfo2 sub{VK_STRUCTURE_TYPE_SUBMIT_INFO_2, nullptr, 0, 1, &w, 1, &c, 1, &s};
        vk(pfnSubmit2_(queue_, 1, &sub, frame.fence), "Vulkan submit2 failed");
    } else
#endif
    {
        VkPipelineStageFlags ws = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo sub{VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 1, &frame.acquire, &ws, 1, &frame.cmd, 1, &fin};
        vk(vkQueueSubmit(queue_, 1, &sub, frame.fence), "Vulkan submit failed");
    }

    VkPresentInfoKHR pres{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, nullptr, 1, &fin, 1, &swapchain_, &image};
    VkResult presRes = vkQueuePresentKHR(queue_, &pres);
    frameIndex_ = (frameIndex_ + 1) % kFrames;
    if (acq == VK_SUBOPTIMAL_KHR || presRes == VK_ERROR_OUT_OF_DATE_KHR || presRes == VK_SUBOPTIMAL_KHR) recreateSwapchain();
    else if (presRes != VK_SUCCESS) throw std::runtime_error("Vulkan present failed");
}

void VulkanRenderer::destroyTexture(Texture &tex) {
    if (tex.sampler) vkDestroySampler(device_, tex.sampler, nullptr);
    if (tex.view) vkDestroyImageView(device_, tex.view, nullptr);
    if (tex.image) vkDestroyImage(device_, tex.image, nullptr);
    if (tex.memory) vkFreeMemory(device_, tex.memory, nullptr);
    tex = Texture{};
}

void VulkanRenderer::uploadTextureFull(Texture &tex, int width, int height, const unsigned char *rgba) {
    if (width <= 0 || height <= 0) throw std::runtime_error("invalid texture dimensions");
    if (tex.image && (tex.width != width || tex.height != height)) { vkDeviceWaitIdle(device_); destroyTexture(tex); }

    VkDeviceSize size = static_cast<VkDeviceSize>(width) * height * 4;
    Buffer staging;
    ensureBuffer(staging, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false);
    std::memcpy(staging.mapped, rgba, size);
    if (!staging.coherent) {
        VkMappedMemoryRange mr{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, staging.memory, 0, VK_WHOLE_SIZE};
        vkFlushMappedMemoryRanges(device_, 1, &mr);
    }

    if (!tex.image) {
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D; ici.format = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
        ici.mipLevels = 1; ici.arrayLayers = 1; ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL; ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        vk(vkCreateImage(device_, &ici, nullptr, &tex.image), "Vulkan texture image failed");
        VkMemoryRequirements req{}; vkGetImageMemoryRequirements(device_, tex.image, &req);
        bool unused = false;
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, unused);
        vk(vkAllocateMemory(device_, &ai, nullptr, &tex.memory), "Vulkan texture memory failed");
        vk(vkBindImageMemory(device_, tex.image, tex.memory, 0), "Vulkan texture bind failed");
        tex.width = width; tex.height = height;

        VkImageViewCreateInfo iv{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        iv.image = tex.image; iv.viewType = VK_IMAGE_VIEW_TYPE_2D; iv.format = VK_FORMAT_R8G8B8A8_UNORM;
        iv.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vk(vkCreateImageView(device_, &iv, nullptr, &tex.view), "Vulkan texture view failed");

        VkSamplerCreateInfo sc{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sc.magFilter = sc.minFilter = VK_FILTER_LINEAR;
        sc.addressModeU = sc.addressModeV = sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vk(vkCreateSampler(device_, &sc, nullptr, &tex.sampler), "Vulkan texture sampler failed");

        if (!usePushDescriptors_) {
            if (!tex.set) {
                VkDescriptorSetAllocateInfo ds{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, descriptorPool_, 1, &descriptorLayout_};
                vk(vkAllocateDescriptorSets(device_, &ds, &tex.set), "Vulkan texture descriptor allocation failed");
            }
            VkDescriptorImageInfo ii{tex.sampler, tex.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = tex.set; w.descriptorCount = 1; w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &ii;
            vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
        }
    }

    vkResetFences(device_, 1, &uploadFence_);
    vkResetCommandBuffer(uploadCmd_, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(uploadCmd_, &bi);
    transitionImage(uploadCmd_, tex.image, tex.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, false);
    VkBufferImageCopy copy{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1}};
    vkCmdCopyBufferToImage(uploadCmd_, staging.handle, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    transitionImage(uploadCmd_, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false);
    vkEndCommandBuffer(uploadCmd_);

    VkSubmitInfo sub{VK_STRUCTURE_TYPE_SUBMIT_INFO}; sub.commandBufferCount = 1; sub.pCommandBuffers = &uploadCmd_;
    vk(vkQueueSubmit(queue_, 1, &sub, uploadFence_), "Vulkan texture upload failed");
    vkWaitForFences(device_, 1, &uploadFence_, VK_TRUE, UINT64_MAX);
    tex.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    destroyBuffer(staging);
}

void VulkanRenderer::createDefaultTextures() {
    if (!usePushDescriptors_) {
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64};
        VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, 64, 1, &ps};
        vk(vkCreateDescriptorPool(device_, &dp, nullptr, &descriptorPool_), "Vulkan texture descriptor pool failed");
    }
    textures_.resize(2);
    unsigned char white[4]{255, 255, 255, 255};
    uploadTextureFull(textures_[0], 1, 1, white);
    std::vector<unsigned char> empty(1024 * 1024 * 4, 0);
    for (size_t i = 0; i < empty.size(); i += 4) empty[i] = empty[i + 1] = empty[i + 2] = 255;
    uploadTextureFull(textures_[1], 1024, 1024, empty.data());
}

uint32_t VulkanRenderer::loadTexture(const std::string &path) {
    int w = 0, h = 0, c = 0;
    stbi_uc *pixels = stbi_load(path.c_str(), &w, &h, &c, 4);
    if (!pixels) throw std::runtime_error("cannot decode texture: " + path);
    if (!usePushDescriptors_ && textures_.size() >= 64) { stbi_image_free(pixels); throw std::runtime_error("Vulkan texture limit reached"); }
    textures_.emplace_back();
    uint32_t id = static_cast<uint32_t>(textures_.size() - 1);
    try { uploadTextureFull(textures_.back(), w, h, pixels); }
    catch (...) { destroyTexture(textures_.back()); textures_.pop_back(); stbi_image_free(pixels); throw; }
    stbi_image_free(pixels);
    return id;
}

void VulkanRenderer::updateTexture(uint32_t id, int width, int height, const unsigned char *rgba) {
    if (id >= textures_.size()) throw std::runtime_error("invalid Vulkan texture ID");
    if (textures_[id].width == width && textures_[id].height == height)
        updateTextureRegion(id, 0, 0, width, height, width, rgba);
    else uploadTextureFull(textures_[id], width, height, rgba);
}

void VulkanRenderer::updateTextureRegion(uint32_t id, int x, int y, int width, int height,
                                         int stridePixels, const unsigned char *rgba) {
    if (id >= textures_.size()) throw std::runtime_error("invalid Vulkan texture ID");
    const Texture &tex = textures_[id];
    if (width <= 0 || height <= 0 || x < 0 || y < 0 || x + width > tex.width || y + height > tex.height || stridePixels < width)
        throw std::runtime_error("invalid Vulkan texture region");
    Upload up{id, x, y, width, height, std::vector<unsigned char>(static_cast<size_t>(width) * height * 4)};
    for (int row = 0; row < height; ++row)
        std::memcpy(up.rgba.data() + static_cast<size_t>(row) * width * 4,
                    rgba + (static_cast<size_t>(y + row) * stridePixels + x) * 4, static_cast<size_t>(width) * 4);
    pendingUploads_.push_back(std::move(up));
}

void VulkanRenderer::shutdown() {
    if (device_) vkDeviceWaitIdle(device_);
    pendingUploads_.clear();
    for (auto &t : textures_) destroyTexture(t);
    textures_.clear();
    if (descriptorPool_) vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
    if (descriptorLayout_) vkDestroyDescriptorSetLayout(device_, descriptorLayout_, nullptr);
    for (auto &f : frames_) {
        destroyBuffer(f.staging); destroyBuffer(f.vertices);
        if (f.fence) vkDestroyFence(device_, f.fence, nullptr);
        if (f.acquire) vkDestroySemaphore(device_, f.acquire, nullptr);
        if (f.pool) vkDestroyCommandPool(device_, f.pool, nullptr);
    }
    if (uploadFence_) vkDestroyFence(device_, uploadFence_, nullptr);
    if (uploadPool_) vkDestroyCommandPool(device_, uploadPool_, nullptr);
    for (auto s : renderFinished_) if (s) vkDestroySemaphore(device_, s, nullptr);
    for (auto fb : framebuffers_) if (fb) vkDestroyFramebuffer(device_, fb, nullptr);
    for (auto v : views_) if (v) vkDestroyImageView(device_, v, nullptr);
    if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
    if (pipelineCache_) vkDestroyPipelineCache(device_, pipelineCache_, nullptr);
    if (pipelineLayout_) vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
    if (renderPass_) vkDestroyRenderPass(device_, renderPass_, nullptr);
    if (device_) vkDestroyDevice(device_, nullptr);
    if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    if (instance_) vkDestroyInstance(instance_, nullptr);
    descriptorPool_ = VK_NULL_HANDLE; descriptorLayout_ = VK_NULL_HANDLE;
    uploadFence_ = VK_NULL_HANDLE; uploadPool_ = VK_NULL_HANDLE; uploadCmd_ = VK_NULL_HANDLE;
    renderFinished_.clear(); framebuffers_.clear(); views_.clear(); images_.clear(); imagesInFlight_.clear();
    swapchain_ = VK_NULL_HANDLE; pipeline_ = VK_NULL_HANDLE; pipelineCache_ = VK_NULL_HANDLE;
    pipelineLayout_ = VK_NULL_HANDLE; renderPass_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE; surface_ = VK_NULL_HANDLE; instance_ = VK_NULL_HANDLE;
    physical_ = VK_NULL_HANDLE; queue_ = VK_NULL_HANDLE; window_ = nullptr;
    frames_ = {}; frameIndex_ = 0; pipelineFormat_ = VK_FORMAT_UNDEFINED;
    useDynamicRendering_ = useSync2_ = usePushDescriptors_ = false;
}

VulkanRenderer::~VulkanRenderer() { shutdown(); }
