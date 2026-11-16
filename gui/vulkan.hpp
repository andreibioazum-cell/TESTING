#pragma once
#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include "geometry.hpp"

class VulkanRenderer {
public:
    VulkanRenderer() = default;
    ~VulkanRenderer();
    VulkanRenderer(const VulkanRenderer &) = delete;
    VulkanRenderer &operator=(const VulkanRenderer &) = delete;

    void initialize(GLFWwindow *window, const std::string &shaderDirectory);
    void render(const std::vector<LcVertex> &vertices, const std::vector<LcDraw> &draws,
                int pixelWidth, int pixelHeight);
    void shutdown();
    uint32_t loadTexture(const std::string &path);
    void updateTexture(uint32_t id, int width, int height, const unsigned char *rgba);
    void updateTextureRegion(uint32_t id, int x, int y, int width, int height,
                             int stridePixels, const unsigned char *rgba);

    uint32_t apiVersion() const { return deviceApiVersion_; }
    bool usesDynamicRendering() const { return useDynamicRendering_; }

private:
    static constexpr uint32_t kFrames = 2;

    struct Buffer {
        VkBuffer handle = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void *mapped = nullptr;
        VkDeviceSize capacity = 0;
        bool coherent = true;
    };
    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkSemaphore acquire = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        Buffer vertices, staging;
    };
    struct Texture {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkSampler sampler = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        int width = 0, height = 0;
    };
    struct Upload {
        uint32_t id = 0;
        int x = 0, y = 0, width = 0, height = 0;
        std::vector<unsigned char> rgba;
    };

    GLFWwindow *window_ = nullptr;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0, instanceApiVersion_ = VK_API_VERSION_1_1, deviceApiVersion_ = VK_API_VERSION_1_1, frameIndex_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;
    bool useDynamicRendering_ = false, useSync2_ = false, usePushDescriptors_ = false;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED, pipelineFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};
    std::vector<VkImage> images_;
    std::vector<VkImageView> views_;
    std::vector<VkFramebuffer> framebuffers_;
    std::vector<VkSemaphore> renderFinished_;
    std::vector<VkFence> imagesInFlight_;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkPipelineCache pipelineCache_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;

    std::array<Frame, kFrames> frames_{};
    VkCommandPool uploadPool_ = VK_NULL_HANDLE;
    VkCommandBuffer uploadCmd_ = VK_NULL_HANDLE;
    VkFence uploadFence_ = VK_NULL_HANDLE;

    std::string shaders_;
    std::vector<Texture> textures_;
    std::vector<Upload> pendingUploads_;
    VkDescriptorSetLayout descriptorLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;

#ifdef VK_API_VERSION_1_3
    PFN_vkCmdBeginRendering pfnBeginRendering_ = nullptr;
    PFN_vkCmdEndRendering pfnEndRendering_ = nullptr;
    PFN_vkCmdPipelineBarrier2 pfnBarrier2_ = nullptr;
    PFN_vkQueueSubmit2 pfnSubmit2_ = nullptr;
#endif
#ifdef VK_KHR_push_descriptor
    PFN_vkCmdPushDescriptorSetKHR pfnPushDescriptor_ = nullptr;
#endif

    void createInstance();
    void selectDevice();
    void createSwapchain(VkSwapchainKHR oldSwapchain = VK_NULL_HANDLE);
    void createPipeline();
    void createFrameResources();
    void createDefaultTextures();
    void recreateSwapchain();
    uint32_t findMemoryType(uint32_t bits, VkMemoryPropertyFlags req, VkMemoryPropertyFlags pref, bool &coherent) const;
    void ensureBuffer(Buffer &buf, VkDeviceSize bytes, VkBufferUsageFlags usage, bool preferDeviceLocal);
    void destroyBuffer(Buffer &buf) const;
    void uploadTextureFull(Texture &tex, int width, int height, const unsigned char *rgba);
    void transitionImage(VkCommandBuffer cmd, VkImage img, VkImageLayout oldL, VkImageLayout newL, bool syncRender) const;
    void recordPendingUploads(Frame &frame);
    void destroyTexture(Texture &tex);
    std::vector<char> readShader(const char *name) const;
};
