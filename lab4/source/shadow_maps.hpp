#pragma once

#include "graphics_internal.hpp"

namespace application {

struct ShadowMap {
    VkImage image{};
    VmaAllocation allocation{};
    VkImageView view{};
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

inline constexpr VkExtent2D shadow_extent{2048, 2048};
inline constexpr VkImageLayout shadow_read_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

VkFormat selectShadowFormat();
VkSampler createShadowSampler();
void createShadowMap(ShadowMap& map, VkFormat format);
void destroyShadowMap(ShadowMap& map);
void beginShadowPass(VkCommandBuffer commands, ShadowMap& map);
void endShadowPass(VkCommandBuffer commands, ShadowMap& map);

} // namespace application
