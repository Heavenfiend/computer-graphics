#pragma once

#include "graphics_internal.hpp"
#include <vector>

namespace application {

struct Texture {
    VkImage image{};
    VmaAllocation allocation{};
    VkImageView view{};
};

// Пустая карта specular/emissive означает использование общей blackTexture().
// Texture владеет ресурсами только внутри этого модуля; не уничтожайте копии.
struct MaterialTextures {
    const char* name;
    Texture albedo, specular, emissive;
};

void initializeTextures();
void shutdownTextures();
const std::vector<MaterialTextures>& materials();
VkSampler textureSampler();
const Texture& blackTexture();

} // namespace application
