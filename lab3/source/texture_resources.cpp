#include "texture_resources.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace application {
namespace {

std::vector<MaterialTextures> loaded_materials;
Texture black;
VkSampler sampler{};

void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + ": VkResult " + std::to_string(result));
}

void destroy(Texture& texture) {
    const auto& context = graphics::internal::context;
    if (texture.view) vkDestroyImageView(context.device, texture.view, nullptr);
    if (texture.image) vmaDestroyImage(context.allocator, texture.image, texture.allocation);
    texture = {};
}

// Живёт до завершения GPU-копирования, поэтому staging не освобождается раньше fence.
struct Upload {
    VkBuffer staging{};
    VmaAllocation allocation{};
    VkCommandPool pool{};
    VkFence fence{};
    bool submitted = false;

    ~Upload() {
        const auto& context = graphics::internal::context;
        if (submitted) vkWaitForFences(context.device, 1, &fence, VK_TRUE, UINT64_MAX);
        if (fence) vkDestroyFence(context.device, fence, nullptr);
        if (pool) vkDestroyCommandPool(context.device, pool, nullptr);
        if (staging) vmaDestroyBuffer(context.allocator, staging, allocation);
    }
};

Texture loadTexture(const char* path) {
    const auto& context = graphics::internal::context;
    int width = 0, height = 0, channels = 0;
    if (!stbi_info(path, &width, &height, &channels))
        throw std::runtime_error(std::string("Cannot read texture ") + path + ": " + stbi_failure_reason());
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(context.physical_device, &properties);
    if (width <= 0 || height <= 0 || static_cast<uint32_t>(width) > properties.limits.maxImageDimension2D ||
        static_cast<uint32_t>(height) > properties.limits.maxImageDimension2D)
        throw std::runtime_error(std::string("Texture dimensions exceed device limits: ") + path);

    // Произведение вычисляется в 64 битах ещё до создания staging.
    const VkDeviceSize size = VkDeviceSize(width) * VkDeviceSize(height) * 4;
    std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(
        stbi_load(path, &width, &height, &channels, STBI_rgb_alpha), stbi_image_free);
    if (!pixels)
        throw std::runtime_error(std::string("Cannot decode texture ") + path + ": " + stbi_failure_reason());

    constexpr VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    VkFormatProperties format_properties{};
    vkGetPhysicalDeviceFormatProperties(context.physical_device, format, &format_properties);
    constexpr VkFormatFeatureFlags required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if ((format_properties.optimalTilingFeatures & required) != required)
        throw std::runtime_error("RGBA8 UNORM sampled/linear/transfer texture format is unavailable");

    Upload upload;
    const VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    const VmaAllocationCreateInfo staging_memory = {
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO,
    };
    VmaAllocationInfo staging_info{};
    check(vmaCreateBuffer(context.allocator, &buffer_info, &staging_memory,
        &upload.staging, &upload.allocation, &staging_info), "Create texture staging");
    std::memcpy(staging_info.pMappedData, pixels.get(), static_cast<size_t>(size));
    check(vmaFlushAllocation(context.allocator, upload.allocation, 0, size), "Flush texture staging");

    Texture texture;
    try {
        const VkImageCreateInfo image_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D, .format = format,
            .extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1},
            .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        const VmaAllocationCreateInfo image_memory = {
            .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
            .requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        };
        check(vmaCreateImage(context.allocator, &image_info, &image_memory,
            &texture.image, &texture.allocation, nullptr), "Create device-local texture");

        const VkCommandPoolCreateInfo pool_info = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
            .queueFamilyIndex = context.graphics_queue_index,
        };
        check(vkCreateCommandPool(context.device, &pool_info, nullptr, &upload.pool), "Create texture upload pool");
        const VkCommandBufferAllocateInfo allocate_info = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = upload.pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1,
        };
        VkCommandBuffer commands{};
        check(vkAllocateCommandBuffers(context.device, &allocate_info, &commands), "Allocate texture upload commands");
        const VkCommandBufferBeginInfo begin_info = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        check(vkBeginCommandBuffer(commands, &begin_info), "Begin texture upload");
        VkImageMemoryBarrier barrier = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = texture.image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);
        const VkBufferImageCopy copy = {
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageExtent = image_info.extent,
        };
        vkCmdCopyBufferToImage(commands, upload.staging, texture.image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);
        check(vkEndCommandBuffer(commands), "End texture upload");
        const VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        check(vkCreateFence(context.device, &fence_info, nullptr, &upload.fence), "Create texture upload fence");
        const VkSubmitInfo submit = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1, .pCommandBuffers = &commands,
        };
        check(vkQueueSubmit(context.graphics_queue, 1, &submit, upload.fence), "Submit texture upload");
        upload.submitted = true;
        check(vkWaitForFences(context.device, 1, &upload.fence, VK_TRUE, UINT64_MAX), "Wait for texture upload");
        upload.submitted = false;

        const VkImageViewCreateInfo view_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = texture.image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = format,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        check(vkCreateImageView(context.device, &view_info, nullptr, &texture.view), "Create texture view");
        std::cout << "Texture: " << path << " (" << width << 'x' << height << ", DEVICE_LOCAL)\n";
        return texture;
    } catch (...) {
        // При ошибке ожидания очередь могла ещё использовать изображение.
        if (upload.submitted) vkDeviceWaitIdle(context.device);
        destroy(texture);
        throw;
    }
}

} // namespace

void initializeTextures() {
    if (!loaded_materials.empty() || sampler || black.image)
        throw std::runtime_error("Textures are already initialized");
    try {
        const VkSamplerCreateInfo info = {
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .maxAnisotropy = 1.0f, .compareOp = VK_COMPARE_OP_ALWAYS,
            .minLod = 0.0f, .maxLod = 0.0f,
        };
        check(vkCreateSampler(graphics::internal::context.device, &info, nullptr, &sampler), "Create texture sampler");
        black = loadTexture("resources/textures/black.png");
        // Записываем материал до загрузки карт, чтобы shutdown удалил и частичный результат.
        loaded_materials.reserve(3);
        loaded_materials.push_back({"Brick"});
        loaded_materials.back().albedo = loadTexture("resources/textures/brick.png");
        loaded_materials.push_back({"Metal + specular + emissive"});
        auto& metal = loaded_materials.back();
        metal.albedo = loadTexture("resources/textures/metal.png");
        metal.specular = loadTexture("resources/textures/metal_specular.png");
        metal.emissive = loadTexture("resources/textures/metal_emissive.png");
        loaded_materials.push_back({"Tiles"});
        loaded_materials.back().albedo = loadTexture("resources/textures/tiles.png");
    } catch (...) {
        shutdownTextures();
        throw;
    }
}

void shutdownTextures() {
    for (auto& material : loaded_materials) {
        destroy(material.albedo);
        destroy(material.specular);
        destroy(material.emissive);
    }
    loaded_materials.clear();
    destroy(black);
    if (sampler) vkDestroySampler(graphics::internal::context.device, sampler, nullptr);
    sampler = VK_NULL_HANDLE;
}

const std::vector<MaterialTextures>& materials() { return loaded_materials; }
VkSampler textureSampler() { return sampler; }
const Texture& blackTexture() { return black; }

} // namespace application
