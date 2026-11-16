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
    bool usesSynchronization2() const { return useSync2_; }
    bool usesPushDescriptors() const { return usePushDescriptors_; }

private:
    static constexpr uint32_t kMaxFramesInFlight = 2;

    struct FrameResources {
        VkCommandPool commandPool = VK_NULL_HANDLE;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        VkSemaphore imageAvailable = VK_NULL_HANDLE;
        VkFence inFlight = VK_NULL_HANDLE;
        VkBuffer vertexBuffer = VK_NULL_HANDLE;
        VkDeviceMemory vertexMemory = VK_NULL_HANDLE;
        void *vertexMapped = nullptr;
        VkDeviceSize vertexCapacity = 0;
        bool vertexHostCoherent = true;
        VkBuffer stagingBuffer = VK_NULL_HANDLE;
        VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
        void *stagingMapped = nullptr;
        VkDeviceSize stagingCapacity = 0;
        bool stagingHostCoherent = true;
    };

    struct Texture {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkSampler sampler = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        int width = 0;
        int height = 0;
    };

    struct PendingUpload {
        uint32_t id = 0;
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
        std::vector<unsigned char> rgba;
    };

    GLFWwindow *window_ = nullptr;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t instanceApiVersion_ = VK_API_VERSION_1_1;
    uint32_t deviceApiVersion_ = VK_API_VERSION_1_1;
    bool useDynamicRendering_ = false;
    bool useSync2_ = false;
    bool usePushDescriptors_ = false;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkFormat pipelineFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};
    std::vector<VkImage> images_;
    std::vector<VkImageView> views_;
    std::vector<VkFramebuffer> framebuffers_;
    std::vector<VkSemaphore> renderFinishedSemaphores_;
    std::vector<VkFence> imagesInFlight_;

    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkPipelineCache pipelineCache_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;

    std::array<FrameResources, kMaxFramesInFlight> frames_{};
    uint32_t frameIndex_ = 0;

    VkCommandPool uploadPool_ = VK_NULL_HANDLE;
    VkCommandBuffer uploadCommand_ = VK_NULL_HANDLE;
    VkFence uploadFence_ = VK_NULL_HANDLE;

    std::string shaders_;
    std::vector<Texture> textures_;
    std::vector<PendingUpload> pendingUploads_;
    VkDescriptorSetLayout descriptorLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;

#ifdef VK_API_VERSION_1_3
    PFN_vkCmdBeginRendering pfnCmdBeginRendering_ = nullptr;
    PFN_vkCmdEndRendering pfnCmdEndRendering_ = nullptr;
    PFN_vkCmdPipelineBarrier2 pfnCmdPipelineBarrier2_ = nullptr;
    PFN_vkQueueSubmit2 pfnQueueSubmit2_ = nullptr;
#endif
#ifdef VK_KHR_push_descriptor
    PFN_vkCmdPushDescriptorSetKHR pfnCmdPushDescriptorSet_ = nullptr;
#endif

    void createInstance();
    void selectDevice();
    void createSwapchain(VkSwapchainKHR oldSwapchain = VK_NULL_HANDLE);
    void destroySwapchainViews();
    void createDescriptorLayout();
    void createRenderPassAndPipeline();
    void destroyPipeline();
    void createFrameResources();
    void createDefaultTextures();
    void recreateSwapchain();

    uint32_t findMemoryType(uint32_t bits, VkMemoryPropertyFlags required,
                            VkMemoryPropertyFlags preferred, bool &isCoherent) const;
    void allocateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool preferDeviceLocal,
                        VkBuffer &buffer, VkDeviceMemory &memory, void *&mapped, bool &coherent) const;
    void destroyBuffer(VkBuffer &buffer, VkDeviceMemory &memory, void *&mapped, VkDeviceSize &capacity) const;
    void ensureVertexBuffer(FrameResources &frame, VkDeviceSize requiredBytes);
    void ensureStagingBuffer(FrameResources &frame, VkDeviceSize requiredBytes);

    void uploadTextureFull(Texture &texture, int width, int height, const unsigned char *rgba);
    void recordPendingUploads(FrameResources &frame);
    void transitionImage(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout,
                         VkImageLayout newLayout, bool toPresent) const;
    void destroyTexture(Texture &texture);
    std::vector<char> readShader(const char *name) const;
};
