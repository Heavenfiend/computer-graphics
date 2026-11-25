#include "shadow_maps.hpp"

#include <stdexcept>
#include <string>

namespace application {
namespace {

using graphics::internal::context;
constexpr VkImageUsageFlags usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
constexpr VkImageSubresourceRange depth_range{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
constexpr VkImageLayout write_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
constexpr VkPipelineStageFlags depth_stages =
    VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
constexpr VkAccessFlags depth_write = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + ": VkResult " + std::to_string(result));
}

void transition(VkCommandBuffer commands, ShadowMap& map, VkImageLayout next,
    VkPipelineStageFlags src_stage, VkAccessFlags src_access,
    VkPipelineStageFlags dst_stage, VkAccessFlags dst_access) {
    const VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src_access, .dstAccessMask = dst_access,
        .oldLayout = map.layout, .newLayout = next,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = map.image, .subresourceRange = depth_range,
    };
    vkCmdPipelineBarrier(commands, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    map.layout = next;
}

} // namespace

VkFormat selectShadowFormat() {
    constexpr VkFormat candidates[] = {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM};
    constexpr VkFormatFeatureFlags2 required = VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT
        | VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_DEPTH_COMPARISON_BIT;
    for (const VkFormat format : candidates) {
        VkFormatProperties3 extended = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
        VkFormatProperties2 properties = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &extended};
        vkGetPhysicalDeviceFormatProperties2(context.physical_device, format, &properties);
        if ((extended.optimalTilingFeatures & required) != required) continue;
        VkImageFormatProperties image{};
        const auto result = vkGetPhysicalDeviceImageFormatProperties(context.physical_device,
            format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage, 0, &image);
        if (result == VK_SUCCESS && image.maxExtent.width >= shadow_extent.width
            && image.maxExtent.height >= shadow_extent.height) return format;
    }
    throw std::runtime_error("No sampled depth format with comparison support for the shadow maps");
}

VkSampler createShadowSampler() {
    // Шейдер усредняет отдельные сравнения, поэтому достаточно nearest.
    const VkSamplerCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
        .compareEnable = VK_TRUE, .compareOp = VK_COMPARE_OP_LESS,
        .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,
    };
    VkSampler sampler{};
    check(vkCreateSampler(context.device, &info, nullptr, &sampler), "Create shadow comparison sampler");
    return sampler;
}

void createShadowMap(ShadowMap& map, VkFormat format) {
    const VkImageCreateInfo image = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = format, .extent = {shadow_extent.width, shadow_extent.height, 1},
        .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
    };
    const VmaAllocationCreateInfo allocation = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    check(vmaCreateImage(context.allocator, &image, &allocation, &map.image, &map.allocation, nullptr),
        "Create shadow depth image");
    const VkImageViewCreateInfo view = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = map.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = format, .subresourceRange = depth_range,
    };
    check(vkCreateImageView(context.device, &view, nullptr, &map.view), "Create shadow depth view");
}

void destroyShadowMap(ShadowMap& map) {
    if (map.view) vkDestroyImageView(context.device, map.view, nullptr);
    if (map.image) vmaDestroyImage(context.allocator, map.image, map.allocation);
    map = {};
}

void beginShadowPass(VkCommandBuffer commands, ShadowMap& map) {
    const bool first_use = map.layout == VK_IMAGE_LAYOUT_UNDEFINED;
    transition(commands, map, write_layout,
        first_use ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        first_use ? 0 : VK_ACCESS_SHADER_READ_BIT, depth_stages,
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | depth_write);
    const VkRenderingAttachmentInfo depth = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = map.view, .imageLayout = write_layout,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = {.depthStencil = {1, 0}},
    };
    const VkRenderingInfo rendering = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {.extent = shadow_extent},
        .layerCount = 1, .pDepthAttachment = &depth,
    };
    vkCmdBeginRendering(commands, &rendering);
}

void endShadowPass(VkCommandBuffer commands, ShadowMap& map) {
    vkCmdEndRendering(commands);
    transition(commands, map, shadow_read_layout, depth_stages, depth_write,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
}

} // namespace application
