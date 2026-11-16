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
void vk(bool ok, const char *message) {
    if (!ok) throw std::runtime_error(message);
}
void vk(VkResult result, const char *message) {
    if (result != VK_SUCCESS) throw std::runtime_error(message);
}

bool hasExtension(const std::vector<VkExtensionProperties> &extensions, const char *name) {
    for (const auto &ext : extensions)
        if (std::strcmp(ext.extensionName, name) == 0) return true;
    return false;
}

#ifdef __APPLE__
bool hasInstanceExtension(const char *name) {
    uint32_t count = 0;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS || !count)
        return false;
    std::vector<VkExtensionProperties> extensions(count);
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data()) != VK_SUCCESS)
        return false;
    return hasExtension(extensions, name);
}
#endif

VkShaderModule shaderModule(VkDevice device, const std::vector<char> &bytes) {
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = bytes.size();
    info.pCode = reinterpret_cast<const uint32_t *>(bytes.data());
    VkShaderModule module = VK_NULL_HANDLE;
    vk(vkCreateShaderModule(device, &info, nullptr, &module), "vkCreateShaderModule");
    return module;
}

uint32_t scorePhysicalDevice(const VkPhysicalDeviceProperties &props,
                             const VkPhysicalDeviceMemoryProperties &mem) {
    uint32_t score = 0;
    switch (props.deviceType) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: score += 100000; break;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: score += 20000; break;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: score += 5000; break;
    case VK_PHYSICAL_DEVICE_TYPE_OTHER: score += 1000; break;
    default: break;
    }
    uint64_t deviceLocalMB = 0;
    for (uint32_t i = 0; i < mem.memoryHeapCount; ++i) {
        if (mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            deviceLocalMB += mem.memoryHeaps[i].size / (1024ULL * 1024ULL);
    }
    score += static_cast<uint32_t>(std::min<uint64_t>(deviceLocalMB, 32768));
#ifdef VK_API_VERSION_MINOR
    score += VK_API_VERSION_MINOR(props.apiVersion) * 100;
#else
    score += VK_VERSION_MINOR(props.apiVersion) * 100;
#endif
    return score;
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
    instanceApiVersion_ = targetVersion;

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "LuaC Studio";
    app.applicationVersion = VK_MAKE_VERSION(0, 5, 0);
    app.pEngineName = "LuaC";
    app.engineVersion = VK_MAKE_VERSION(0, 5, 0);
    app.apiVersion = instanceApiVersion_;

    uint32_t count = 0;
    const char **extensions = glfwGetRequiredInstanceExtensions(&count);
    vk(extensions != nullptr && count > 0, "GLFW Vulkan extensions are unavailable");
    std::vector<const char *> enabled(extensions, extensions + count);

    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &app;
#ifdef __APPLE__
    if (hasInstanceExtension(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
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
    std::vector<VkPhysicalDevice> devices(count);
    vk(vkEnumeratePhysicalDevices(instance_, &count, devices.data()), "vkEnumeratePhysicalDevices");

    uint32_t bestScore = 0;
    VkPhysicalDeviceProperties bestProps{};
    std::vector<VkExtensionProperties> bestExtensions;
    bool found = false;

    for (VkPhysicalDevice candidate : devices) {
        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());

        uint32_t chosenFamily = UINT32_MAX;
        for (uint32_t i = 0; i < familyCount; ++i) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface_, &present);
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                chosenFamily = i;
                break;
            }
        }
        if (chosenFamily == UINT32_MAX) continue;

        uint32_t extensionCount = 0;
        vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extensionCount, nullptr);
        std::vector<VkExtensionProperties> extensions(extensionCount);
        vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extensionCount, extensions.data());
        if (!hasExtension(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;

        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(candidate, &props);
        VkPhysicalDeviceMemoryProperties mem{};
        vkGetPhysicalDeviceMemoryProperties(candidate, &mem);
        uint32_t score = scorePhysicalDevice(props, mem);
        if (!found || score > bestScore) {
            found = true;
            bestScore = score;
            bestProps = props;
            bestExtensions = std::move(extensions);
            physical_ = candidate;
            queueFamily_ = chosenFamily;
        }
    }
    if (!found || physical_ == VK_NULL_HANDLE)
        throw std::runtime_error("no graphics/present Vulkan device");

    deviceApiVersion_ = std::min(instanceApiVersion_, bestProps.apiVersion);

    std::vector<const char *> enabledExtensions;
    enabledExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    if (hasExtension(bestExtensions, "VK_KHR_portability_subset"))
        enabledExtensions.push_back("VK_KHR_portability_subset");

    bool enableDynamicRendering = false;
    bool enableSync2 = false;
    bool enablePushDescriptor = false;

#ifdef VK_API_VERSION_1_3
    VkPhysicalDeviceVulkan13Features vk13Features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    bool hasVk13 = deviceApiVersion_ >= VK_API_VERSION_1_3;
    VkPhysicalDeviceDynamicRenderingFeaturesKHR drExt{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR};
    VkPhysicalDeviceSynchronization2FeaturesKHR sync2Ext{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR};
    bool hasDrExt = !hasVk13 && hasExtension(bestExtensions, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
    bool hasSync2Ext = !hasVk13 && hasExtension(bestExtensions, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
#endif
#ifdef VK_API_VERSION_1_4
    VkPhysicalDeviceVulkan14Features vk14Features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};
    bool hasVk14 = deviceApiVersion_ >= VK_API_VERSION_1_4;
#endif

    VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    void **nextPtr = &features2.pNext;
    if (deviceApiVersion_ >= VK_API_VERSION_1_1) {
#ifdef VK_API_VERSION_1_3
        if (hasVk13) {
            *nextPtr = &vk13Features;
            nextPtr = &vk13Features.pNext;
        } else {
            if (hasDrExt) {
                *nextPtr = &drExt;
                nextPtr = &drExt.pNext;
            }
            if (hasSync2Ext) {
                *nextPtr = &sync2Ext;
                nextPtr = &sync2Ext.pNext;
            }
        }
#endif
#ifdef VK_API_VERSION_1_4
        if (hasVk14) {
            *nextPtr = &vk14Features;
            nextPtr = &vk14Features.pNext;
        }
#endif
        vkGetPhysicalDeviceFeatures2(physical_, &features2);
    } else {
        vkGetPhysicalDeviceFeatures(physical_, &features2.features);
    }

#ifdef VK_API_VERSION_1_3
    VkPhysicalDeviceVulkan13Features enabledVk13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceDynamicRenderingFeaturesKHR enabledDrExt{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR};
    VkPhysicalDeviceSynchronization2FeaturesKHR enabledSync2Ext{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR};
    if (hasVk13) {
        if (vk13Features.dynamicRendering) {
            enabledVk13.dynamicRendering = VK_TRUE;
            enableDynamicRendering = true;
        }
        if (vk13Features.synchronization2) {
            enabledVk13.synchronization2 = VK_TRUE;
            enableSync2 = true;
        }
    } else {
        if (hasDrExt && drExt.dynamicRendering) {
            enabledDrExt.dynamicRendering = VK_TRUE;
            enabledExtensions.push_back(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
            enableDynamicRendering = true;
        }
        if (hasSync2Ext && sync2Ext.synchronization2) {
            enabledSync2Ext.synchronization2 = VK_TRUE;
            enabledExtensions.push_back(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
            enableSync2 = true;
        }
    }
#endif

#ifdef VK_API_VERSION_1_4
    VkPhysicalDeviceVulkan14Features enabledVk14{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};
    if (hasVk14 && vk14Features.pushDescriptor) {
        enabledVk14.pushDescriptor = VK_TRUE;
        enablePushDescriptor = true;
    }
#endif
#ifdef VK_KHR_push_descriptor
    if (!enablePushDescriptor && hasExtension(bestExtensions, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)) {
        enabledExtensions.push_back(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
        enablePushDescriptor = true;
    }
#endif

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueFamilyIndex = queueFamily_;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;

    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue;
    info.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
    info.ppEnabledExtensionNames = enabledExtensions.data();

    VkPhysicalDeviceFeatures2 enabledFeatures2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    if (deviceApiVersion_ >= VK_API_VERSION_1_1) {
        void **enableNext = &enabledFeatures2.pNext;
#ifdef VK_API_VERSION_1_3
        if (hasVk13 && (enableDynamicRendering || enableSync2)) {
            *enableNext = &enabledVk13;
            enableNext = &enabledVk13.pNext;
        } else {
            if (hasDrExt && enableDynamicRendering) {
                *enableNext = &enabledDrExt;
                enableNext = &enabledDrExt.pNext;
            }
            if (hasSync2Ext && enableSync2) {
                *enableNext = &enabledSync2Ext;
                enableNext = &enabledSync2Ext.pNext;
            }
        }
#endif
#ifdef VK_API_VERSION_1_4
        if (hasVk14 && enablePushDescriptor) {
            *enableNext = &enabledVk14;
            enableNext = &enabledVk14.pNext;
        }
#endif
        info.pNext = &enabledFeatures2;
    }

    vk(vkCreateDevice(physical_, &info, nullptr, &device_), "Vulkan logical device creation failed");
    vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);

#ifdef VK_API_VERSION_1_3
    if (enableDynamicRendering) {
        pfnCmdBeginRendering_ = reinterpret_cast<PFN_vkCmdBeginRendering>(
            vkGetDeviceProcAddr(device_, hasVk13 ? "vkCmdBeginRendering" : "vkCmdBeginRenderingKHR"));
        pfnCmdEndRendering_ = reinterpret_cast<PFN_vkCmdEndRendering>(
            vkGetDeviceProcAddr(device_, hasVk13 ? "vkCmdEndRendering" : "vkCmdEndRenderingKHR"));
        useDynamicRendering_ = (pfnCmdBeginRendering_ != nullptr && pfnCmdEndRendering_ != nullptr);
    }
    if (enableSync2) {
        pfnCmdPipelineBarrier2_ = reinterpret_cast<PFN_vkCmdPipelineBarrier2>(
            vkGetDeviceProcAddr(device_, hasVk13 ? "vkCmdPipelineBarrier2" : "vkCmdPipelineBarrier2KHR"));
        pfnQueueSubmit2_ = reinterpret_cast<PFN_vkQueueSubmit2>(
            vkGetDeviceProcAddr(device_, hasVk13 ? "vkQueueSubmit2" : "vkQueueSubmit2KHR"));
        useSync2_ = (pfnCmdPipelineBarrier2_ != nullptr && pfnQueueSubmit2_ != nullptr);
    }
#endif
#ifdef VK_KHR_push_descriptor
    if (enablePushDescriptor) {
        pfnCmdPushDescriptorSet_ = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(
            vkGetDeviceProcAddr(device_, "vkCmdPushDescriptorSet"));
        if (!pfnCmdPushDescriptorSet_) {
            pfnCmdPushDescriptorSet_ = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(
                vkGetDeviceProcAddr(device_, "vkCmdPushDescriptorSetKHR"));
        }
        usePushDescriptors_ = (pfnCmdPushDescriptorSet_ != nullptr);
    }
#endif
}

void VulkanRenderer::createSwapchain(VkSwapchainKHR oldSwapchain) {
    VkSurfaceCapabilitiesKHR capabilities{};
    vk(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &capabilities), "surface capabilities");

    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &count, formats.data());
    if (!count) throw std::runtime_error("no Vulkan surface formats");
    VkSurfaceFormatKHR selected = formats[0];
    for (const auto &format : formats) {
        if ((format.format == VK_FORMAT_B8G8R8A8_UNORM || format.format == VK_FORMAT_R8G8B8A8_UNORM) &&
            format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            selected = format;
            break;
        }
    }
    format_ = selected.format;

    uint32_t presentModeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &presentModeCount, nullptr);
    std::vector<VkPresentModeKHR> presentModes(presentModeCount);
    if (presentModeCount)
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &presentModeCount, presentModes.data());
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    for (VkPresentModeKHR mode : presentModes) {
        if (mode == VK_PRESENT_MODE_MAILBOX_KHR) { presentMode = mode; break; }
        if (mode == VK_PRESENT_MODE_FIFO_RELAXED_KHR) presentMode = mode;
    }

    int width = 0, height = 0;
    glfwGetFramebufferSize(window_, &width, &height);
    extent_ = capabilities.currentExtent;
    if (extent_.width == UINT32_MAX) {
        extent_.width = std::max(capabilities.minImageExtent.width,
            std::min(capabilities.maxImageExtent.width, static_cast<uint32_t>(std::max(width, 1))));
        extent_.height = std::max(capabilities.minImageExtent.height,
            std::min(capabilities.maxImageExtent.height, static_cast<uint32_t>(std::max(height, 1))));
    }

    uint32_t desiredImages = (presentMode == VK_PRESENT_MODE_MAILBOX_KHR)
        ? std::max(3u, capabilities.minImageCount)
        : (capabilities.minImageCount + 1);
    if (capabilities.maxImageCount && desiredImages > capabilities.maxImageCount)
        desiredImages = capabilities.maxImageCount;

    VkCompositeAlphaFlagBitsKHR compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    const VkCompositeAlphaFlagBitsKHR alphaCandidates[] = {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR
    };
    for (auto candidate : alphaCandidates) {
        if (capabilities.supportedCompositeAlpha & candidate) {
            compositeAlpha = candidate;
            break;
        }
    }

    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = surface_;
    info.minImageCount = desiredImages;
    info.imageFormat = format_;
    info.imageColorSpace = selected.colorSpace;
    info.imageExtent = extent_;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = capabilities.currentTransform;
    info.compositeAlpha = compositeAlpha;
    info.presentMode = presentMode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = oldSwapchain;

    VkSwapchainKHR newSwapchain = VK_NULL_HANDLE;
    vk(vkCreateSwapchainKHR(device_, &info, nullptr, &newSwapchain), "Vulkan swapchain creation failed");
    if (oldSwapchain != VK_NULL_HANDLE) vkDestroySwapchainKHR(device_, oldSwapchain, nullptr);
    swapchain_ = newSwapchain;

    vkGetSwapchainImagesKHR(device_, swapchain_, &count, nullptr);
    images_.resize(count);
    vkGetSwapchainImagesKHR(device_, swapchain_, &count, images_.data());
    views_.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = images_[i];
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = format_;
        view.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                           VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vk(vkCreateImageView(device_, &view, nullptr, &views_[i]), "Vulkan image view creation failed");
    }

    for (auto sem : renderFinishedSemaphores_)
        if (sem) vkDestroySemaphore(device_, sem, nullptr);
    renderFinishedSemaphores_.assign(count, VK_NULL_HANDLE);
    VkSemaphoreCreateInfo semInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t i = 0; i < count; ++i)
        vk(vkCreateSemaphore(device_, &semInfo, nullptr, &renderFinishedSemaphores_[i]), "Vulkan semaphore failed");
    imagesInFlight_.assign(count, VK_NULL_HANDLE);

    if (!useDynamicRendering_ && renderPass_ != VK_NULL_HANDLE) {
        framebuffers_.resize(count);
        for (uint32_t i = 0; i < count; ++i) {
            VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fb.renderPass = renderPass_;
            fb.attachmentCount = 1;
            fb.pAttachments = &views_[i];
            fb.width = extent_.width;
            fb.height = extent_.height;
            fb.layers = 1;
            vk(vkCreateFramebuffer(device_, &fb, nullptr, &framebuffers_[i]), "Vulkan framebuffer creation failed");
        }
    }
}

void VulkanRenderer::destroySwapchainViews() {
    for (auto framebuffer : framebuffers_)
        if (framebuffer) vkDestroyFramebuffer(device_, framebuffer, nullptr);
    for (auto view : views_)
        if (view) vkDestroyImageView(device_, view, nullptr);
    framebuffers_.clear();
    views_.clear();
    images_.clear();
    imagesInFlight_.clear();
}

void VulkanRenderer::createDescriptorLayout() {
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.bindingCount = 1;
    info.pBindings = &binding;
#ifdef VK_KHR_push_descriptor
    if (usePushDescriptors_) info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
#endif
    vk(vkCreateDescriptorSetLayout(device_, &info, nullptr, &descriptorLayout_), "Vulkan texture descriptor layout failed");
}

void VulkanRenderer::destroyPipeline() {
    if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
    pipeline_ = VK_NULL_HANDLE;
    if (renderPass_) vkDestroyRenderPass(device_, renderPass_, nullptr);
    renderPass_ = VK_NULL_HANDLE;
}

void VulkanRenderer::createRenderPassAndPipeline() {
    if (pipelineLayout_ == VK_NULL_HANDLE) {
        createDescriptorLayout();
        VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float) * 2};
        VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layout.setLayoutCount = 1;
        layout.pSetLayouts = &descriptorLayout_;
        layout.pushConstantRangeCount = 1;
        layout.pPushConstantRanges = &push;
        vk(vkCreatePipelineLayout(device_, &layout, nullptr, &pipelineLayout_), "Vulkan pipeline layout failed");
    }
    if (pipelineCache_ == VK_NULL_HANDLE) {
        VkPipelineCacheCreateInfo cacheInfo{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        vk(vkCreatePipelineCache(device_, &cacheInfo, nullptr, &pipelineCache_), "Vulkan pipeline cache failed");
    }

    if (!useDynamicRendering_) {
        VkAttachmentDescription attachment{};
        attachment.format = format_;
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
        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo render{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        render.attachmentCount = 1;
        render.pAttachments = &attachment;
        render.subpassCount = 1;
        render.pSubpasses = &subpass;
        render.dependencyCount = 1;
        render.pDependencies = &dependency;
        vk(vkCreateRenderPass(device_, &render, nullptr, &renderPass_), "Vulkan render pass creation failed");

        framebuffers_.resize(views_.size());
        for (size_t i = 0; i < views_.size(); ++i) {
            VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fb.renderPass = renderPass_;
            fb.attachmentCount = 1;
            fb.pAttachments = &views_[i];
            fb.width = extent_.width;
            fb.height = extent_.height;
            fb.layers = 1;
            vk(vkCreateFramebuffer(device_, &fb, nullptr, &framebuffers_[i]), "Vulkan framebuffer creation failed");
        }
    }

    auto vertBytes = readShader("shader.vert.spv");
    auto fragBytes = readShader("shader.frag.spv");
    VkShaderModule vert = shaderModule(device_, vertBytes);
    VkShaderModule frag = shaderModule(device_, fragBytes);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding{0, sizeof(LcVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attributes[3]{};
    attributes[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(LcVertex, x)};
    attributes[1] = {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(LcVertex, r)};
    attributes[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(LcVertex, u)};
    VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    input.vertexBindingDescriptionCount = 1;
    input.pVertexBindingDescriptions = &binding;
    input.vertexAttributeDescriptionCount = 3;
    input.pVertexAttributeDescriptions = attributes;

    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport viewport{0, 0, 1, 1, 0, 1};
    VkRect2D scissor{{0, 0}, {1, 1}};
    VkPipelineViewportStateCreateInfo view{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    view.viewportCount = 1;
    view.pViewports = &viewport;
    view.scissorCount = 1;
    view.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    VkPipelineColorBlendStateCreateInfo blending{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blending.attachmentCount = 1;
    blending.pAttachments = &blend;

    VkDynamicState states[]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = states;

    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &view;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pColorBlendState = &blending;
    info.pDynamicState = &dynamic;
    info.layout = pipelineLayout_;
    info.renderPass = useDynamicRendering_ ? VK_NULL_HANDLE : renderPass_;
    info.subpass = 0;

#ifdef VK_API_VERSION_1_3
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    if (useDynamicRendering_) {
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &format_;
        info.pNext = &rendering;
    }
#endif

    vk(vkCreateGraphicsPipelines(device_, pipelineCache_, 1, &info, nullptr, &pipeline_), "Vulkan graphics pipeline failed");
    pipelineFormat_ = format_;
    vkDestroyShaderModule(device_, frag, nullptr);
    vkDestroyShaderModule(device_, vert, nullptr);
}

uint32_t VulkanRenderer::findMemoryType(uint32_t bits, VkMemoryPropertyFlags required,
                                        VkMemoryPropertyFlags preferred, bool &isCoherent) const {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physical_, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & preferred) == preferred) {
            isCoherent = (properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
            return i;
        }
    }
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & required) == required) {
            isCoherent = (properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
            return i;
        }
    }
    throw std::runtime_error("required Vulkan memory type is unavailable");
}

void VulkanRenderer::allocateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool preferDeviceLocal,
                                    VkBuffer &buffer, VkDeviceMemory &memory, void *&mapped, bool &coherent) const {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vk(vkCreateBuffer(device_, &info, nullptr, &buffer), "Vulkan buffer creation failed");

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device_, buffer, &req);

    VkMemoryPropertyFlags required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    VkMemoryPropertyFlags preferred = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (preferDeviceLocal) preferred |= VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

    uint32_t typeIndex = findMemoryType(req.memoryTypeBits, required, preferred, coherent);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = typeIndex;
    vk(vkAllocateMemory(device_, &alloc, nullptr, &memory), "Vulkan buffer memory allocation failed");
    vk(vkBindBufferMemory(device_, buffer, memory, 0), "Vulkan buffer bind failed");
    vk(vkMapMemory(device_, memory, 0, size, 0, &mapped), "Vulkan buffer map failed");
}

void VulkanRenderer::destroyBuffer(VkBuffer &buffer, VkDeviceMemory &memory, void *&mapped, VkDeviceSize &capacity) const {
    if (mapped && memory) vkUnmapMemory(device_, memory);
    if (buffer) vkDestroyBuffer(device_, buffer, nullptr);
    if (memory) vkFreeMemory(device_, memory, nullptr);
    buffer = VK_NULL_HANDLE;
    memory = VK_NULL_HANDLE;
    mapped = nullptr;
    capacity = 0;
}

void VulkanRenderer::ensureVertexBuffer(FrameResources &frame, VkDeviceSize requiredBytes) {
    if (requiredBytes <= frame.vertexCapacity) return;
    destroyBuffer(frame.vertexBuffer, frame.vertexMemory, frame.vertexMapped, frame.vertexCapacity);
    VkDeviceSize newCapacity = std::max<VkDeviceSize>(requiredBytes, std::max<VkDeviceSize>(256 * 1024, frame.vertexCapacity * 2));
    allocateBuffer(newCapacity, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true,
                   frame.vertexBuffer, frame.vertexMemory, frame.vertexMapped, frame.vertexHostCoherent);
    frame.vertexCapacity = newCapacity;
}

void VulkanRenderer::ensureStagingBuffer(FrameResources &frame, VkDeviceSize requiredBytes) {
    if (requiredBytes <= frame.stagingCapacity) return;
    destroyBuffer(frame.stagingBuffer, frame.stagingMemory, frame.stagingMapped, frame.stagingCapacity);
    VkDeviceSize newCapacity = std::max<VkDeviceSize>(requiredBytes, std::max<VkDeviceSize>(256 * 1024, frame.stagingCapacity * 2));
    allocateBuffer(newCapacity, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false,
                   frame.stagingBuffer, frame.stagingMemory, frame.stagingMapped, frame.stagingHostCoherent);
    frame.stagingCapacity = newCapacity;
}

void VulkanRenderer::createFrameResources() {
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.queueFamilyIndex = queueFamily_;
    VkSemaphoreCreateInfo semInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fenceSignaled{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenceSignaled.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (auto &frame : frames_) {
        vk(vkCreateCommandPool(device_, &poolInfo, nullptr, &frame.commandPool), "Vulkan command pool failed");
        VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        alloc.commandPool = frame.commandPool;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        vk(vkAllocateCommandBuffers(device_, &alloc, &frame.commandBuffer), "Vulkan command buffer failed");
        vk(vkCreateSemaphore(device_, &semInfo, nullptr, &frame.imageAvailable), "Vulkan semaphore failed");
        vk(vkCreateFence(device_, &fenceSignaled, nullptr, &frame.inFlight), "Vulkan fence failed");
        ensureVertexBuffer(frame, 256 * 1024);
    }

    VkCommandPoolCreateInfo uploadPoolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    uploadPoolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    uploadPoolInfo.queueFamilyIndex = queueFamily_;
    vk(vkCreateCommandPool(device_, &uploadPoolInfo, nullptr, &uploadPool_), "Vulkan upload pool failed");
    VkCommandBufferAllocateInfo uploadAlloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    uploadAlloc.commandPool = uploadPool_;
    uploadAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    uploadAlloc.commandBufferCount = 1;
    vk(vkAllocateCommandBuffers(device_, &uploadAlloc, &uploadCommand_), "Vulkan upload command buffer failed");
    VkFenceCreateInfo uploadFenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vk(vkCreateFence(device_, &uploadFenceInfo, nullptr, &uploadFence_), "Vulkan upload fence failed");
}

void VulkanRenderer::initialize(GLFWwindow *window, const std::string &shaderDirectory) {
    window_ = window;
    shaders_ = shaderDirectory;
    createInstance();
    selectDevice();
    createSwapchain();
    createRenderPassAndPipeline();
    createFrameResources();
    createDefaultTextures();
}

void VulkanRenderer::recreateSwapchain() {
    int width = 0, height = 0;
    do {
        glfwGetFramebufferSize(window_, &width, &height);
        if ((width == 0 || height == 0) && !glfwWindowShouldClose(window_)) glfwWaitEvents();
    } while ((width == 0 || height == 0) && !glfwWindowShouldClose(window_));
    if (glfwWindowShouldClose(window_)) return;

    vkDeviceWaitIdle(device_);
    destroySwapchainViews();
    VkSwapchainKHR oldSwapchain = swapchain_;
    swapchain_ = VK_NULL_HANDLE;
    createSwapchain(oldSwapchain);
    if (format_ != pipelineFormat_) {
        destroyPipeline();
        createRenderPassAndPipeline();
    }
}

void VulkanRenderer::transitionImage(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout,
                                     VkImageLayout newLayout, bool toPresent) const {
#ifdef VK_API_VERSION_1_3
    if (useSync2_) {
        VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (toPresent) {
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
            barrier.dstAccessMask = VK_ACCESS_2_NONE;
        } else {
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_NONE;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        }
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &barrier;
        pfnCmdPipelineBarrier2_(cmd, &dep);
        return;
    }
#endif
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkPipelineStageFlags dstStage = toPresent ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    barrier.srcAccessMask = toPresent ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : 0;
    barrier.dstAccessMask = toPresent ? 0 : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void VulkanRenderer::recordPendingUploads(FrameResources &frame) {
    if (pendingUploads_.empty()) return;

    VkDeviceSize totalBytes = 0;
    for (const auto &up : pendingUploads_) totalBytes += up.rgba.size();
    ensureStagingBuffer(frame, totalBytes);

    std::vector<VkDeviceSize> offsets;
    offsets.reserve(pendingUploads_.size());
    VkDeviceSize cursor = 0;
    auto *dst = static_cast<unsigned char *>(frame.stagingMapped);
    for (const auto &up : pendingUploads_) {
        offsets.push_back(cursor);
        std::memcpy(dst + cursor, up.rgba.data(), up.rgba.size());
        cursor += up.rgba.size();
    }
    if (!frame.stagingHostCoherent) {
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = frame.stagingMemory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        vkFlushMappedMemoryRanges(device_, 1, &range);
    }

    for (size_t i = 0; i < pendingUploads_.size(); ++i) {
        const auto &up = pendingUploads_[i];
        Texture &tex = textures_[up.id];
#ifdef VK_API_VERSION_1_3
        if (useSync2_) {
            VkImageMemoryBarrier2 toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            toTransfer.srcStageMask = (tex.layout == VK_IMAGE_LAYOUT_UNDEFINED)
                ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT
                : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            toTransfer.srcAccessMask = (tex.layout == VK_IMAGE_LAYOUT_UNDEFINED)
                ? VK_ACCESS_2_NONE
                : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            toTransfer.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            toTransfer.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            toTransfer.oldLayout = tex.layout;
            toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toTransfer.image = tex.image;
            toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkDependencyInfo dep1{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep1.imageMemoryBarrierCount = 1;
            dep1.pImageMemoryBarriers = &toTransfer;
            pfnCmdPipelineBarrier2_(frame.commandBuffer, &dep1);

            VkBufferImageCopy copy{};
            copy.bufferOffset = offsets[i];
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageOffset = {up.x, up.y, 0};
            copy.imageExtent = {static_cast<uint32_t>(up.width), static_cast<uint32_t>(up.height), 1};
            vkCmdCopyBufferToImage(frame.commandBuffer, frame.stagingBuffer, tex.image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

            VkImageMemoryBarrier2 toRead{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            toRead.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            toRead.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            toRead.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            toRead.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            toRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toRead.image = tex.image;
            toRead.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkDependencyInfo dep2{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep2.imageMemoryBarrierCount = 1;
            dep2.pImageMemoryBarriers = &toRead;
            pfnCmdPipelineBarrier2_(frame.commandBuffer, &dep2);
            tex.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            continue;
        }
#endif
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.oldLayout = tex.layout;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = tex.image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier.srcAccessMask = (tex.layout == VK_IMAGE_LAYOUT_UNDEFINED) ? 0 : VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        VkPipelineStageFlags srcStage = (tex.layout == VK_IMAGE_LAYOUT_UNDEFINED)
            ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
            : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        vkCmdPipelineBarrier(frame.commandBuffer, srcStage, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);

        VkBufferImageCopy copy{};
        copy.bufferOffset = offsets[i];
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageOffset = {up.x, up.y, 0};
        copy.imageExtent = {static_cast<uint32_t>(up.width), static_cast<uint32_t>(up.height), 1};
        vkCmdCopyBufferToImage(frame.commandBuffer, frame.stagingBuffer, tex.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(frame.commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
        tex.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    pendingUploads_.clear();
}

void VulkanRenderer::render(const std::vector<LcVertex> &vertices, const std::vector<LcDraw> &draws,
                            int pixelWidth, int pixelHeight) {
    if (!device_ || vertices.empty() || draws.empty() || pixelWidth <= 0 || pixelHeight <= 0) return;
    if (extent_.width == 0 || extent_.height == 0) { recreateSwapchain(); return; }

    FrameResources &frame = frames_[frameIndex_];
    vk(vkWaitForFences(device_, 1, &frame.inFlight, VK_TRUE, UINT64_MAX), "Vulkan wait fence failed");

    uint32_t image = 0;
    VkResult acquireResult = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                                   frame.imageAvailable, VK_NULL_HANDLE, &image);
    if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateSwapchain();
        return;
    }
    if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR)
        throw std::runtime_error("Vulkan acquire failed");

    if (imagesInFlight_[image] != VK_NULL_HANDLE && imagesInFlight_[image] != frame.inFlight)
        vk(vkWaitForFences(device_, 1, &imagesInFlight_[image], VK_TRUE, UINT64_MAX), "Vulkan image fence failed");
    imagesInFlight_[image] = frame.inFlight;

    vk(vkResetFences(device_, 1, &frame.inFlight), "Vulkan reset fence failed");
    vk(vkResetCommandPool(device_, frame.commandPool, 0), "Vulkan reset command pool failed");

    VkDeviceSize vertexBytes = vertices.size() * sizeof(LcVertex);
    ensureVertexBuffer(frame, vertexBytes);
    std::memcpy(frame.vertexMapped, vertices.data(), vertexBytes);
    if (!frame.vertexHostCoherent) {
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = frame.vertexMemory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        vkFlushMappedMemoryRanges(device_, 1, &range);
    }

    VkCommandBuffer cmd = frame.commandBuffer;
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk(vkBeginCommandBuffer(cmd, &begin), "Vulkan begin command buffer failed");

    recordPendingUploads(frame);

    VkClearValue clear{};
    clear.color = {{0.027f, 0.035f, 0.055f, 1.0f}};

#ifdef VK_API_VERSION_1_3
    if (useDynamicRendering_) {
        transitionImage(cmd, images_[image], VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, false);
        VkRenderingAttachmentInfo colorAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        colorAttachment.imageView = views_[image];
        colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.clearValue = clear;

        VkRenderingInfo renderingInfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
        renderingInfo.renderArea = {{0, 0}, extent_};
        renderingInfo.layerCount = 1;
        renderingInfo.colorAttachmentCount = 1;
        renderingInfo.pColorAttachments = &colorAttachment;
        pfnCmdBeginRendering_(cmd, &renderingInfo);
    } else
#endif
    {
        VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        pass.renderPass = renderPass_;
        pass.framebuffer = framebuffers_[image];
        pass.renderArea = {{0, 0}, extent_};
        pass.clearValueCount = 1;
        pass.pClearValues = &clear;
        vkCmdBeginRenderPass(cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    VkViewport viewport{0, 0, static_cast<float>(extent_.width), static_cast<float>(extent_.height), 0, 1};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    float scale[2]{2.0f / static_cast<float>(pixelWidth), 2.0f / static_cast<float>(pixelHeight)};
    vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(scale), scale);
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &frame.vertexBuffer, &offset);

    uint32_t lastTexture = UINT32_MAX;
    VkRect2D lastClip{{INT32_MIN, INT32_MIN}, {0, 0}};

    for (const LcDraw &draw : draws) {
        if (draw.count == 0) continue;
        if (draw.first > vertices.size() || draw.count > vertices.size() - draw.first)
            throw std::runtime_error("invalid Nuklear Vulkan draw batch");
        int x = std::max(0, draw.x), y = std::max(0, draw.y);
        uint32_t right = std::min(extent_.width, static_cast<uint32_t>(x) + draw.width);
        uint32_t bottom = std::min(extent_.height, static_cast<uint32_t>(y) + draw.height);
        if (static_cast<uint32_t>(x) >= right || static_cast<uint32_t>(y) >= bottom) continue;

        VkRect2D clip{{x, y}, {right - static_cast<uint32_t>(x), bottom - static_cast<uint32_t>(y)}};
        if (clip.offset.x != lastClip.offset.x || clip.offset.y != lastClip.offset.y ||
            clip.extent.width != lastClip.extent.width || clip.extent.height != lastClip.extent.height) {
            vkCmdSetScissor(cmd, 0, 1, &clip);
            lastClip = clip;
        }

        uint32_t texture = draw.texture < textures_.size() ? draw.texture : 0;
        if (texture != lastTexture) {
#ifdef VK_KHR_push_descriptor
            if (usePushDescriptors_) {
                const Texture &tex = textures_[texture];
                VkDescriptorImageInfo imageInfo{tex.sampler, tex.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstBinding = 0;
                write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                write.pImageInfo = &imageInfo;
                pfnCmdPushDescriptorSet_(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1, &write);
            } else
#endif
            {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_,
                                        0, 1, &textures_[texture].set, 0, nullptr);
            }
            lastTexture = texture;
        }
        vkCmdDraw(cmd, draw.count, 1, draw.first, 0);
    }

#ifdef VK_API_VERSION_1_3
    if (useDynamicRendering_) {
        pfnCmdEndRendering_(cmd);
        transitionImage(cmd, images_[image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, true);
    } else
#endif
    {
        vkCmdEndRenderPass(cmd);
    }

    vk(vkEndCommandBuffer(cmd), "Vulkan end command buffer failed");

    VkSemaphore renderFinished = renderFinishedSemaphores_[image];
#ifdef VK_API_VERSION_1_3
    if (useSync2_) {
        VkSemaphoreSubmitInfo waitSem{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        waitSem.semaphore = frame.imageAvailable;
        waitSem.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

        VkCommandBufferSubmitInfo cmdInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        cmdInfo.commandBuffer = cmd;

        VkSemaphoreSubmitInfo signalSem{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        signalSem.semaphore = renderFinished;
        signalSem.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

        VkSubmitInfo2 submit2{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        submit2.waitSemaphoreInfoCount = 1;
        submit2.pWaitSemaphoreInfos = &waitSem;
        submit2.commandBufferInfoCount = 1;
        submit2.pCommandBufferInfos = &cmdInfo;
        submit2.signalSemaphoreInfoCount = 1;
        submit2.pSignalSemaphoreInfos = &signalSem;
        vk(pfnQueueSubmit2_(queue_, 1, &submit2, frame.inFlight), "Vulkan submit2 failed");
    } else
#endif
    {
        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &frame.imageAvailable;
        submit.pWaitDstStageMask = &waitStage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &renderFinished;
        vk(vkQueueSubmit(queue_, 1, &submit, frame.inFlight), "Vulkan submit failed");
    }

    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &renderFinished;
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain_;
    present.pImageIndices = &image;
    VkResult presentResult = vkQueuePresentKHR(queue_, &present);

    frameIndex_ = (frameIndex_ + 1) % kMaxFramesInFlight;

    if (acquireResult == VK_SUBOPTIMAL_KHR || presentResult == VK_ERROR_OUT_OF_DATE_KHR ||
        presentResult == VK_SUBOPTIMAL_KHR) {
        recreateSwapchain();
    } else if (presentResult != VK_SUCCESS) {
        throw std::runtime_error("Vulkan present failed");
    }
}

void VulkanRenderer::destroyTexture(Texture &texture) {
    if (texture.sampler) vkDestroySampler(device_, texture.sampler, nullptr);
    if (texture.view) vkDestroyImageView(device_, texture.view, nullptr);
    if (texture.image) vkDestroyImage(device_, texture.image, nullptr);
    if (texture.memory) vkFreeMemory(device_, texture.memory, nullptr);
    texture.image = VK_NULL_HANDLE;
    texture.memory = VK_NULL_HANDLE;
    texture.view = VK_NULL_HANDLE;
    texture.sampler = VK_NULL_HANDLE;
    texture.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    texture.width = 0;
    texture.height = 0;
}

void VulkanRenderer::uploadTextureFull(Texture &texture, int width, int height, const unsigned char *rgba) {
    if (width <= 0 || height <= 0) throw std::runtime_error("invalid texture dimensions");
    bool recreateImage = (texture.image == VK_NULL_HANDLE || texture.width != width || texture.height != height);
    if (recreateImage && texture.image != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        destroyTexture(texture);
    }

    VkDeviceSize size = static_cast<VkDeviceSize>(width) * height * 4;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    void *mapped = nullptr;
    bool stagingCoherent = true;
    allocateBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false, staging, stagingMemory, mapped, stagingCoherent);
    std::memcpy(mapped, rgba, size);
    if (!stagingCoherent) {
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = stagingMemory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        vkFlushMappedMemoryRanges(device_, 1, &range);
    }

    if (recreateImage) {
        VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image.imageType = VK_IMAGE_TYPE_2D;
        image.format = VK_FORMAT_R8G8B8A8_UNORM;
        image.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
        image.mipLevels = 1;
        image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        vk(vkCreateImage(device_, &image, nullptr, &texture.image), "Vulkan texture image failed");

        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(device_, texture.image, &req);
        bool unused = false;
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, unused);
        vk(vkAllocateMemory(device_, &alloc, nullptr, &texture.memory), "Vulkan texture memory failed");
        vk(vkBindImageMemory(device_, texture.image, texture.memory, 0), "Vulkan texture bind failed");
        texture.width = width;
        texture.height = height;
        texture.layout = VK_IMAGE_LAYOUT_UNDEFINED;

        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = texture.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = VK_FORMAT_R8G8B8A8_UNORM;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vk(vkCreateImageView(device_, &view, nullptr, &texture.view), "Vulkan texture view failed");

        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler.magFilter = VK_FILTER_LINEAR;
        sampler.minFilter = VK_FILTER_LINEAR;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vk(vkCreateSampler(device_, &sampler, nullptr, &texture.sampler), "Vulkan texture sampler failed");

        if (!usePushDescriptors_) {
            if (texture.set == VK_NULL_HANDLE) {
                VkDescriptorSetAllocateInfo sets{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
                sets.descriptorPool = descriptorPool_;
                sets.descriptorSetCount = 1;
                sets.pSetLayouts = &descriptorLayout_;
                vk(vkAllocateDescriptorSets(device_, &sets, &texture.set), "Vulkan texture descriptor allocation failed");
            }
            VkDescriptorImageInfo imageInfo{texture.sampler, texture.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = texture.set;
            write.dstBinding = 0;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &imageInfo;
            vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
        }
    }

    vkResetFences(device_, 1, &uploadFence_);
    vkResetCommandBuffer(uploadCommand_, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(uploadCommand_, &begin);

    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = texture.layout;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = texture.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.srcAccessMask = (texture.layout == VK_IMAGE_LAYOUT_UNDEFINED) ? 0 : VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(uploadCommand_,
                         (texture.layout == VK_IMAGE_LAYOUT_UNDEFINED) ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
    vkCmdCopyBufferToImage(uploadCommand_, staging, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(uploadCommand_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);
    vkEndCommandBuffer(uploadCommand_);

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &uploadCommand_;
    vk(vkQueueSubmit(queue_, 1, &submit, uploadFence_), "Vulkan texture upload failed");
    vkWaitForFences(device_, 1, &uploadFence_, VK_TRUE, UINT64_MAX);
    texture.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    destroyBuffer(staging, stagingMemory, mapped, size);
}

void VulkanRenderer::createDefaultTextures() {
    if (!usePushDescriptors_) {
        VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets = 64;
        pool.poolSizeCount = 1;
        pool.pPoolSizes = &poolSize;
        vk(vkCreateDescriptorPool(device_, &pool, nullptr, &descriptorPool_), "Vulkan texture descriptor pool failed");
    }
    textures_.resize(2);
    unsigned char white[4]{255, 255, 255, 255};
    uploadTextureFull(textures_[0], 1, 1, white);
    std::vector<unsigned char> empty(1024 * 1024 * 4, 0);
    for (size_t i = 0; i < empty.size(); i += 4) empty[i] = empty[i + 1] = empty[i + 2] = 255;
    uploadTextureFull(textures_[1], 1024, 1024, empty.data());
}

uint32_t VulkanRenderer::loadTexture(const std::string &path) {
    int width = 0, height = 0, channels = 0;
    stbi_uc *pixels = stbi_load(path.c_str(), &width, &height, &channels, 4);
    if (!pixels) throw std::runtime_error("cannot decode texture: " + path);
    if (!usePushDescriptors_ && textures_.size() >= 64) {
        stbi_image_free(pixels);
        throw std::runtime_error("Vulkan texture limit reached");
    }
    textures_.emplace_back();
    uint32_t id = static_cast<uint32_t>(textures_.size() - 1);
    try {
        uploadTextureFull(textures_.back(), width, height, pixels);
    } catch (...) {
        destroyTexture(textures_.back());
        textures_.pop_back();
        stbi_image_free(pixels);
        throw;
    }
    stbi_image_free(pixels);
    return id;
}

void VulkanRenderer::updateTexture(uint32_t id, int width, int height, const unsigned char *rgba) {
    if (id >= textures_.size()) throw std::runtime_error("invalid Vulkan texture ID");
    if (width <= 0 || height <= 0) throw std::runtime_error("invalid texture dimensions");
    Texture &tex = textures_[id];
    if (tex.width == width && tex.height == height) {
        updateTextureRegion(id, 0, 0, width, height, width, rgba);
        return;
    }
    uploadTextureFull(tex, width, height, rgba);
}

void VulkanRenderer::updateTextureRegion(uint32_t id, int x, int y, int width, int height,
                                         int stridePixels, const unsigned char *rgba) {
    if (id >= textures_.size()) throw std::runtime_error("invalid Vulkan texture ID");
    Texture &tex = textures_[id];
    if (width <= 0 || height <= 0 || x < 0 || y < 0 ||
        x + width > tex.width || y + height > tex.height || stridePixels < width)
        throw std::runtime_error("invalid Vulkan texture region");

    PendingUpload upload;
    upload.id = id;
    upload.x = x;
    upload.y = y;
    upload.width = width;
    upload.height = height;
    upload.rgba.resize(static_cast<size_t>(width) * height * 4);
    for (int row = 0; row < height; ++row) {
        const unsigned char *src = rgba + (static_cast<size_t>(y + row) * stridePixels + x) * 4;
        std::memcpy(upload.rgba.data() + static_cast<size_t>(row) * width * 4,
                    src, static_cast<size_t>(width) * 4);
    }
    pendingUploads_.push_back(std::move(upload));
}

void VulkanRenderer::shutdown() {
    if (device_) vkDeviceWaitIdle(device_);
    pendingUploads_.clear();
    for (auto &texture : textures_) destroyTexture(texture);
    textures_.clear();

    if (descriptorPool_) vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
    descriptorPool_ = VK_NULL_HANDLE;
    if (descriptorLayout_) vkDestroyDescriptorSetLayout(device_, descriptorLayout_, nullptr);
    descriptorLayout_ = VK_NULL_HANDLE;

    for (auto &frame : frames_) {
        destroyBuffer(frame.stagingBuffer, frame.stagingMemory, frame.stagingMapped, frame.stagingCapacity);
        destroyBuffer(frame.vertexBuffer, frame.vertexMemory, frame.vertexMapped, frame.vertexCapacity);
        if (frame.inFlight) vkDestroyFence(device_, frame.inFlight, nullptr);
        if (frame.imageAvailable) vkDestroySemaphore(device_, frame.imageAvailable, nullptr);
        if (frame.commandPool) vkDestroyCommandPool(device_, frame.commandPool, nullptr);
        frame = FrameResources{};
    }

    if (uploadFence_) vkDestroyFence(device_, uploadFence_, nullptr);
    uploadFence_ = VK_NULL_HANDLE;
    if (uploadPool_) vkDestroyCommandPool(device_, uploadPool_, nullptr);
    uploadPool_ = VK_NULL_HANDLE;
    uploadCommand_ = VK_NULL_HANDLE;

    for (auto sem : renderFinishedSemaphores_)
        if (sem) vkDestroySemaphore(device_, sem, nullptr);
    renderFinishedSemaphores_.clear();

    destroySwapchainViews();
    if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;

    destroyPipeline();
    if (pipelineCache_) vkDestroyPipelineCache(device_, pipelineCache_, nullptr);
    pipelineCache_ = VK_NULL_HANDLE;
    if (pipelineLayout_) vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
    pipelineLayout_ = VK_NULL_HANDLE;

    if (device_) vkDestroyDevice(device_, nullptr);
    device_ = VK_NULL_HANDLE;
    if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    surface_ = VK_NULL_HANDLE;
    if (instance_) vkDestroyInstance(instance_, nullptr);
    instance_ = VK_NULL_HANDLE;

    window_ = nullptr;
    physical_ = VK_NULL_HANDLE;
    queue_ = VK_NULL_HANDLE;
    frameIndex_ = 0;
    useDynamicRendering_ = false;
    useSync2_ = false;
    usePushDescriptors_ = false;
    pipelineFormat_ = VK_FORMAT_UNDEFINED;
#ifdef VK_API_VERSION_1_3
    pfnCmdBeginRendering_ = nullptr;
    pfnCmdEndRendering_ = nullptr;
    pfnCmdPipelineBarrier2_ = nullptr;
    pfnQueueSubmit2_ = nullptr;
#endif
#ifdef VK_KHR_push_descriptor
    pfnCmdPushDescriptorSet_ = nullptr;
#endif
}

VulkanRenderer::~VulkanRenderer() { shutdown(); }
