#include "application.hpp"
#include "dodecahedron.hpp"
#include "scene_math.hpp"

#include <imgui.h>
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace application {
namespace {
using Vec4 = std::array<float, 4>;
constexpr float minimum_scale = 0.01f;
constexpr float marker_size = 0.09f;
float background_color[4] = {0.04f, 0.05f, 0.07f, 1};

struct Camera {
    Vec3 position = {0, 0, 6};
    Vec3 rotation = {0, 0, 0};
    float fov = 45;
    float near_plane = 0.1f, far_plane = 100;
    float orthographic_height = 2.5f;
    float speed = 2.5f;
    int projection = 0;
} camera;

struct SceneObject {
    Vec3 position{}, rotation = {-17.2f, 25.8f, 0}, scale = {0.5f, 0.5f, 0.5f};
    Vec3 albedo = {1, 1, 1}, specular = {0.3f, 0.3f, 0.3f};
    float shininess = 32;
    bool vertex_colors = true, animation_playing = false;
    double animation_time = 0;
    float animation_speed = 1, trajectory_radius = 0.8f, trajectory_height = 0.5f, spin_speed = 40;
    Vec3 animated_position{};
    float animated_angle = 0;
};
std::array<SceneObject, 2> objects;
int selected_object = 0;

struct Light {
    Vec3 position = {0, 2, 2}, color = {1, 1, 1};
    float intensity = 4;
};
struct SpotLight : Light {
    Vec3 direction = {0, -1, -1};
    float inner_angle = 15, outer_angle = 30;
};
std::vector<Light> point_lights;
std::vector<SpotLight> spot_lights;
Vec3 ambient_color = {1, 1, 1}, directional_color = {1, 1, 1};
Vec3 directional_direction = {0.5f, -0.8f, -1};
float ambient_intensity = 0.08f, directional_intensity = 0.6f;
bool show_markers = true;
float cone_length = 1.4f;
VkDeviceSize max_storage_range = 0;

// vec4-поля явно повторяют std140/std430 в GLSL, включая padding mat3.
struct SceneUniform {
    Matrix view_projection;
    Vec4 camera_position, camera_view_direction, ambient, directional_direction, directional_color;
    std::array<uint32_t, 4> light_counts;
};
struct ModelUniform {
    Matrix model;
    std::array<float, 12> normal_matrix;
    Vec4 albedo, specular;
};
struct PointUniform { Vec4 position, color; };
struct SpotUniform { Vec4 position, direction, color, angles; };
struct MarkerUniform { Matrix mvp; Vec4 color; };
static_assert(sizeof(SceneUniform) == 160 && offsetof(SceneUniform, light_counts) == 144);
static_assert(sizeof(ModelUniform) == 144 && offsetof(ModelUniform, albedo) == 112);
static_assert(sizeof(PointUniform) == 32 && sizeof(SpotUniform) == 64);
static_assert(sizeof(MarkerUniform) == 80);

struct Buffer {
    VkBuffer handle{};
    VmaAllocation allocation{};
    void* mapped{};
    VkDeviceSize size{};
};
Buffer vertices, indices, cube_vertices, cone_vertices, scene_buffer, point_buffer, spot_buffer;
std::array<Buffer, objects.size()> model_buffers;
VkDescriptorSetLayout scene_layout{}, model_layout{};
VkDescriptorPool descriptor_pool{};
VkDescriptorSet scene_set{};
std::array<VkDescriptorSet, objects.size()> model_sets{};
VkPipelineLayout pipeline_layout{}, marker_layout{};
VkPipeline pipeline{}, marker_pipeline{}, line_pipeline{};
uint32_t index_count = 0;

void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + ": VkResult " + std::to_string(result));
}

Vec4 vector4(const Vec3& v, float w = 0) { return {v[0], v[1], v[2], w}; }

void destroyBuffer(Buffer& buffer) {
    if (buffer.handle)
        vmaDestroyBuffer(graphics::internal::context.allocator, buffer.handle, buffer.allocation);
    buffer = {};
}

void upload(const Buffer& buffer, const void* data, VkDeviceSize size) {
    if (!size) return;
    if (size > buffer.size) throw std::runtime_error("Buffer upload exceeds allocation");
    std::memcpy(buffer.mapped, data, static_cast<size_t>(size));
    check(vmaFlushAllocation(graphics::internal::context.allocator, buffer.allocation, 0, size), "Flush buffer");
}

Buffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, const void* data = nullptr) {
    Buffer buffer{};
    const VkBufferCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    const VmaAllocationCreateInfo memory = {
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO,
    };
    VmaAllocationInfo allocated{};
    check(vmaCreateBuffer(graphics::internal::context.allocator, &info, &memory,
        &buffer.handle, &buffer.allocation, &allocated), "Create buffer");
    buffer.mapped = allocated.pMappedData;
    buffer.size = size;
    try {
        if (data) upload(buffer, data, size);
    } catch (...) {
        destroyBuffer(buffer);
        throw;
    }
    return buffer;
}

void writeDescriptor(VkDescriptorSet set, uint32_t binding, const Buffer& buffer, VkDescriptorType type) {
    const VkDescriptorBufferInfo info = {buffer.handle, 0, buffer.size};
    const VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = set, .dstBinding = binding, .descriptorCount = 1,
        .descriptorType = type, .pBufferInfo = &info,
    };
    vkUpdateDescriptorSets(graphics::internal::context.device, 1, &write, 0, nullptr);
}

// Вызывается после prepare(): единственный предыдущий кадр уже завершён.
// Пустому массиву тоже нужен валидный descriptor, но shader читает ровно count элементов.
void updateStorage(Buffer& buffer, const void* data, size_t count, size_t stride, uint32_t binding) {
    const VkDeviceSize required = static_cast<VkDeviceSize>(std::max(count, size_t{1})) * stride;
    if (required > max_storage_range) throw std::runtime_error("Storage buffer exceeds device limit");
    if (required > buffer.size) {
        const VkDeviceSize capacity = std::min(max_storage_range, std::max(required, buffer.size * 2));
        Buffer replacement = createBuffer(capacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        destroyBuffer(buffer);
        buffer = replacement;
        writeDescriptor(scene_set, binding, buffer, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    }
    upload(buffer, data, count * stride);
}

VkShaderModule loadShader(const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error(std::string("Cannot open shader: ") + path);
    const auto size = file.tellg();
    if (size <= 0 || size % 4 != 0) throw std::runtime_error("Invalid SPIR-V size");
    std::vector<uint32_t> code(static_cast<size_t>(size) / 4);
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(code.data()), size)) throw std::runtime_error("Cannot read shader");
    const VkShaderModuleCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = static_cast<size_t>(size), .pCode = code.data(),
    };
    VkShaderModule module{};
    check(vkCreateShaderModule(graphics::internal::context.device, &info, nullptr, &module), "Create shader");
    return module;
}

void createDescriptors() {
    const auto device = graphics::internal::context.device;
    constexpr VkShaderStageFlags both = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    const VkDescriptorSetLayoutBinding scene_bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, both, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    };
    const VkDescriptorSetLayoutCreateInfo scene_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 3, .pBindings = scene_bindings,
    };
    check(vkCreateDescriptorSetLayout(device, &scene_info, nullptr, &scene_layout), "Scene layout");
    const VkDescriptorSetLayoutBinding model_binding = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, both, nullptr};
    const VkDescriptorSetLayoutCreateInfo model_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &model_binding,
    };
    check(vkCreateDescriptorSetLayout(device, &model_info, nullptr, &model_layout), "Model layout");
    const uint32_t object_count = static_cast<uint32_t>(objects.size());
    const VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 + object_count},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2},
    };
    const VkDescriptorPoolCreateInfo pool = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1 + object_count, .poolSizeCount = 2, .pPoolSizes = sizes,
    };
    check(vkCreateDescriptorPool(device, &pool, nullptr, &descriptor_pool), "Descriptor pool");
    std::array<VkDescriptorSetLayout, 1 + objects.size()> layouts{};
    layouts[0] = scene_layout;
    std::fill(layouts.begin() + 1, layouts.end(), model_layout);
    std::array<VkDescriptorSet, layouts.size()> sets{};
    const VkDescriptorSetAllocateInfo allocate = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptor_pool, .descriptorSetCount = static_cast<uint32_t>(layouts.size()),
        .pSetLayouts = layouts.data(),
    };
    check(vkAllocateDescriptorSets(device, &allocate, sets.data()), "Allocate descriptors");
    scene_set = sets[0];
    scene_buffer = createBuffer(sizeof(SceneUniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    point_buffer = createBuffer(sizeof(PointUniform), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    spot_buffer = createBuffer(sizeof(SpotUniform), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    writeDescriptor(scene_set, 0, scene_buffer, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    writeDescriptor(scene_set, 1, point_buffer, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    writeDescriptor(scene_set, 2, spot_buffer, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    for (size_t i = 0; i < objects.size(); ++i) {
        model_sets[i] = sets[i + 1];
        model_buffers[i] = createBuffer(sizeof(ModelUniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        writeDescriptor(model_sets[i], 0, model_buffers[i], VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    }
}

VkPipeline createPipeline(const char* vertex_path, const char* fragment_path,
    VkPipelineLayout layout, VkPrimitiveTopology topology) {
    const auto& context = graphics::internal::context;
    VkShaderModule vertex{}, fragment{};
    VkPipeline result{};
    try {
        vertex = loadShader(vertex_path);
        fragment = loadShader(fragment_path);
        const VkPipelineShaderStageCreateInfo stages[] = {
            {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vertex, .pName = "main"},
            {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fragment, .pName = "main"},
        };
        const VkVertexInputBindingDescription binding = {0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
        const VkVertexInputAttributeDescription attributes[] = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
            {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
        };
        const VkPipelineVertexInputStateCreateInfo input = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
            .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &binding,
            .vertexAttributeDescriptionCount = layout == marker_layout ? 1u : 2u, .pVertexAttributeDescriptions = attributes,
        };
        const VkPipelineInputAssemblyStateCreateInfo assembly = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = topology,
        };
        const VkPipelineViewportStateCreateInfo viewport = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1,
        };
        const VkPipelineRasterizationStateCreateInfo raster = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
            .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1,
        };
        const VkPipelineMultisampleStateCreateInfo samples = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
        };
        const VkPipelineDepthStencilStateCreateInfo depth = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
            .depthTestEnable = VK_TRUE, .depthWriteEnable = VK_TRUE, .depthCompareOp = VK_COMPARE_OP_LESS,
        };
        const VkPipelineColorBlendAttachmentState attachment = {
            .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        };
        const VkPipelineColorBlendStateCreateInfo blend = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 1, .pAttachments = &attachment,
        };
        const VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        const VkPipelineDynamicStateCreateInfo dynamic = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = states,
        };
        const VkGraphicsPipelineCreateInfo info = {
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .stageCount = 2, .pStages = stages, .pVertexInputState = &input, .pInputAssemblyState = &assembly,
            .pViewportState = &viewport, .pRasterizationState = &raster, .pMultisampleState = &samples,
            .pDepthStencilState = &depth, .pColorBlendState = &blend, .pDynamicState = &dynamic,
            .layout = layout, .renderPass = context.render_pass,
        };
        check(vkCreateGraphicsPipelines(context.device, VK_NULL_HANDLE, 1, &info, nullptr, &result), "Create pipeline");
    } catch (...) {
        if (vertex) vkDestroyShaderModule(context.device, vertex, nullptr);
        if (fragment) vkDestroyShaderModule(context.device, fragment, nullptr);
        throw;
    }
    vkDestroyShaderModule(context.device, vertex, nullptr);
    vkDestroyShaderModule(context.device, fragment, nullptr);
    return result;
}

void createPipelines() {
    const auto device = graphics::internal::context.device;
    const VkDescriptorSetLayout layouts[] = {scene_layout, model_layout};
    const VkPipelineLayoutCreateInfo layout = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 2, .pSetLayouts = layouts,
    };
    check(vkCreatePipelineLayout(device, &layout, nullptr, &pipeline_layout), "Pipeline layout");
    const VkPushConstantRange push = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(MarkerUniform)};
    const VkPipelineLayoutCreateInfo marker = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1, .pPushConstantRanges = &push,
    };
    check(vkCreatePipelineLayout(device, &marker, nullptr, &marker_layout), "Marker layout");
    pipeline = createPipeline("shaders/dodecahedron.vert.spv", "shaders/dodecahedron.frag.spv",
        pipeline_layout, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    marker_pipeline = createPipeline("shaders/marker.vert.spv", "shaders/marker.frag.spv",
        marker_layout, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    line_pipeline = createPipeline("shaders/marker.vert.spv", "shaders/marker.frag.spv",
        marker_layout, VK_PRIMITIVE_TOPOLOGY_LINE_LIST);
}

void editDirection(const char* label, Vec3& direction) {
    ImGui::DragFloat3(label, direction.data(), 0.02f);
    if (dot(direction, direction) < 1e-8f)
        ImGui::TextUnformatted("Zero direction uses (0, 0, -1).");
}

void cameraControls(float delta) {
    const auto& io = ImGui::GetIO();
    if (!io.WantCaptureMouse && ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
        camera.rotation[1] += io.MouseDelta.x * 0.2f;
        camera.rotation[0] -= io.MouseDelta.y * 0.2f;
    }
    camera.rotation[0] = std::clamp(camera.rotation[0], -89.0f, 89.0f);
    camera.rotation[1] = std::remainder(camera.rotation[1], 360.0f);
    if (!io.WantCaptureKeyboard) {
        const auto basis = cameraBasis(camera.rotation);
        Vec3 movement{};
        if (ImGui::IsKeyDown(ImGuiKey_W)) movement = add(movement, basis.forward);
        if (ImGui::IsKeyDown(ImGuiKey_S)) movement = subtract(movement, basis.forward);
        if (ImGui::IsKeyDown(ImGuiKey_D)) movement = add(movement, basis.right);
        if (ImGui::IsKeyDown(ImGuiKey_A)) movement = subtract(movement, basis.right);
        if (ImGui::IsKeyDown(ImGuiKey_E)) movement = add(movement, basis.up);
        if (ImGui::IsKeyDown(ImGuiKey_Q)) movement = subtract(movement, basis.up);
        if (dot(movement, movement) > 0)
            camera.position = add(camera.position, scaleVector(normalize(movement), camera.speed * delta));
    }
}

void cameraUI() {
    if (!ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::DragFloat3("Camera position", camera.position.data(), 0.05f);
    ImGui::SliderFloat("Pitch", &camera.rotation[0], -89, 89, "%.1f deg", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SliderFloat("Yaw", &camera.rotation[1], -180, 180, "%.1f deg", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SliderFloat("Roll", &camera.rotation[2], -180, 180, "%.1f deg", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SliderFloat("FOV", &camera.fov, 5, 150, "%.1f deg", ImGuiSliderFlags_AlwaysClamp);
    ImGui::Combo("Projection", &camera.projection, "Perspective\0Orthographic\0");
    if (camera.projection == 1)
        ImGui::DragFloat("Ortho half height", &camera.orthographic_height, 0.05f, 0.1f, 100, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::DragFloat("Near plane", &camera.near_plane, 0.01f, 0.01f, 10, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::DragFloat("Far plane", &camera.far_plane, 0.5f, 0.02f, 1000, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    camera.far_plane = std::max(camera.far_plane, camera.near_plane + 0.01f);
    ImGui::SliderFloat("Move speed", &camera.speed, 0.1f, 20, "%.1f", ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::Button("Reset camera")) camera = Camera{};
}

void objectUI() {
    if (!ImGui::CollapsingHeader("Objects and materials", ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::Combo("Object", &selected_object, "Dodecahedron 1\0Dodecahedron 2\0");
    auto& object = objects[selected_object];
    ImGui::DragFloat3("Position", object.position.data(), 0.02f);
    ImGui::SliderFloat3("Rotation", object.rotation.data(), -180, 180, "%.1f deg");
    ImGui::DragFloat3("Scale", object.scale.data(), 0.01f, minimum_scale, 4, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::ColorEdit3("Albedo", object.albedo.data());
    ImGui::ColorEdit3("Specular color", object.specular.data());
    ImGui::SliderFloat("Shininess", &object.shininess, 1, 256, "%.1f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::Checkbox("Vertex colors", &object.vertex_colors);
    if (ImGui::Button("Reset transform")) {
        object.position = selected_object == 0 ? Vec3{-0.8f, -0.25f, 0} : Vec3{1.6f, 0.25f, 0};
        object.rotation = selected_object == 0 ? Vec3{-17.2f, 25.8f, 0} : Vec3{15, -30, 0};
        object.scale = {0.5f, 0.5f, 0.5f};
    }
    if (ImGui::TreeNode("Animation")) {
        if (ImGui::Button(object.animation_playing ? "Pause" : "Play")) object.animation_playing = !object.animation_playing;
        ImGui::SameLine();
        if (ImGui::Button("Reset animation")) {
            object.animation_playing = false;
            object.animation_time = 0;
            object.animated_angle = 0;
        }
        ImGui::SliderFloat("Speed", &object.animation_speed, 0.1f, 3, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SliderFloat("Radius", &object.trajectory_radius, 0, 1.5f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SliderFloat("Height", &object.trajectory_height, 0, 1, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SliderFloat("Spin", &object.spin_speed, 0, 120, "%.0f deg/s", ImGuiSliderFlags_AlwaysClamp);
        ImGui::TreePop();
    }
}

void lightUI() {
    if (ImGui::CollapsingHeader("Ambient and directional")) {
        ImGui::ColorEdit3("Ambient color", ambient_color.data());
        ImGui::SliderFloat("Ambient intensity", &ambient_intensity, 0, 1, "%.3f", ImGuiSliderFlags_AlwaysClamp);
        editDirection("Ray direction", directional_direction);
        ImGui::ColorEdit3("Directional color", directional_color.data());
        ImGui::DragFloat("Directional intensity", &directional_intensity, 0.01f, 0, 10, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    }
    if (ImGui::CollapsingHeader("Point lights")) {
        const bool full = point_lights.size() >= max_storage_range / sizeof(PointUniform);
        ImGui::BeginDisabled(full);
        if (ImGui::Button("Add point light")) point_lights.emplace_back();
        ImGui::EndDisabled();
        for (size_t i = 0; i < point_lights.size();) {
            ImGui::PushID(static_cast<int>(i));
            const bool open = ImGui::TreeNode("Point", "Point %zu", i + 1);
            bool remove = false;
            if (open) {
                auto& light = point_lights[i];
                ImGui::DragFloat3("Position", light.position.data(), 0.02f);
                ImGui::ColorEdit3("Color", light.color.data());
                ImGui::DragFloat("Intensity", &light.intensity, 0.05f, 0, 1000, "%.2f", ImGuiSliderFlags_AlwaysClamp);
                remove = ImGui::Button("Remove point light");
                ImGui::TreePop();
            }
            ImGui::PopID();
            if (remove) point_lights.erase(point_lights.begin() + static_cast<std::ptrdiff_t>(i));
            else ++i;
        }
    }
    if (ImGui::CollapsingHeader("Spot lights")) {
        const bool full = spot_lights.size() >= max_storage_range / sizeof(SpotUniform);
        ImGui::BeginDisabled(full);
        if (ImGui::Button("Add spot light")) spot_lights.emplace_back();
        ImGui::EndDisabled();
        for (size_t i = 0; i < spot_lights.size();) {
            ImGui::PushID(static_cast<int>(i));
            const bool open = ImGui::TreeNode("Spot", "Spot %zu", i + 1);
            bool remove = false;
            if (open) {
                auto& light = spot_lights[i];
                ImGui::DragFloat3("Position", light.position.data(), 0.02f);
                editDirection("Direction", light.direction);
                ImGui::ColorEdit3("Color", light.color.data());
                ImGui::DragFloat("Intensity", &light.intensity, 0.05f, 0, 1000, "%.2f", ImGuiSliderFlags_AlwaysClamp);
                ImGui::SliderFloat("Inner half angle", &light.inner_angle, 0, 89, "%.1f deg", ImGuiSliderFlags_AlwaysClamp);
                ImGui::SliderFloat("Outer half angle", &light.outer_angle, 0, 89, "%.1f deg", ImGuiSliderFlags_AlwaysClamp);
                light.inner_angle = std::min(light.inner_angle, light.outer_angle);
                remove = ImGui::Button("Remove spot light");
                ImGui::TreePop();
            }
            ImGui::PopID();
            if (remove) spot_lights.erase(spot_lights.begin() + static_cast<std::ptrdiff_t>(i));
            else ++i;
        }
    }
    ImGui::Checkbox("Show light markers and cones", &show_markers);
    if (show_markers)
        ImGui::SliderFloat("Cone display length", &cone_length, 0.1f, 5, "%.1f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::Text("Point: %zu, spot: %zu", point_lights.size(), spot_lights.size());
}

Matrix viewProjection(float aspect) {
    const Matrix projection = camera.projection == 0
        ? perspective(camera.fov, aspect, camera.near_plane, camera.far_plane)
        : orthographic(camera.orthographic_height, aspect, camera.near_plane, camera.far_plane);
    return multiply(projection, viewMatrix(camera.position, camera.rotation));
}

void drawMarkers(VkCommandBuffer commands, const Matrix& vp) {
    if (!show_markers) return;
    constexpr VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    const VkDeviceSize offset = 0;
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, marker_pipeline);
    vkCmdBindVertexBuffers(commands, 0, 1, &cube_vertices.handle, &offset);
    const auto drawLight = [&](const Light& light) {
        const MarkerUniform marker = {
            multiply(vp, modelMatrix(light.position, {0, 0, 0}, {marker_size, marker_size, marker_size})),
            vector4(light.color, 1),
        };
        vkCmdPushConstants(commands, marker_layout, stages, 0, sizeof(marker), &marker);
        vkCmdDraw(commands, 36, 1, 0, 0);
    };
    for (const auto& light : point_lights) drawLight(light);
    for (const auto& light : spot_lights) drawLight(light);
    std::vector<Vertex> lines;
    for (const auto& light : spot_lights)
        for (const auto& position : coneLines(light.position, light.direction, light.outer_angle, cone_length))
            lines.push_back({{position[0], position[1], position[2]}, {0, 0, 0}});
    if (lines.empty()) return;
    const VkDeviceSize size = lines.size() * sizeof(Vertex);
    if (cone_vertices.size < size) {
        Buffer replacement = createBuffer(size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        destroyBuffer(cone_vertices);
        cone_vertices = replacement;
    }
    upload(cone_vertices, lines.data(), size);
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, line_pipeline);
    vkCmdBindVertexBuffers(commands, 0, 1, &cone_vertices.handle, &offset);
    const MarkerUniform cone = {vp, {1, 1, 1, 1}};
    vkCmdPushConstants(commands, marker_layout, stages, 0, sizeof(cone), &cone);
    vkCmdDraw(commands, static_cast<uint32_t>(lines.size()), 1, 0, 0);
}
} // namespace

bool initialize() {
    try {
        objects[0].position = {-0.8f, -0.25f, 0};
        objects[1].position = {1.6f, 0.25f, 0};
        objects[1].rotation = {15, -30, 0};
        objects[1].albedo = {1, 0.7f, 0.35f};
        objects[1].trajectory_radius = 0.5f;
        point_lights.push_back({{-1.1f, 1.3f, 1}, {1, 0.55f, 0.25f}, 1.5f});
        SpotLight spot;
        spot.position = {2, 1.4f, 1.5f};
        spot.direction = {-0.4f, -1, -1};
        spot.color = {0.35f, 0.6f, 1};
        spot.intensity = 3;
        spot_lights.push_back(spot);
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(graphics::internal::context.physical_device, &properties);
        max_storage_range = properties.limits.maxStorageBufferRange;
        const auto mesh = makeDodecahedron();
        index_count = static_cast<uint32_t>(mesh.indices.size());
        vertices = createBuffer(sizeof(mesh.vertices), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, mesh.vertices.data());
        indices = createBuffer(sizeof(mesh.indices), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, mesh.indices.data());
        const Vec3 corners[] = {{-1,-1,-1}, {1,-1,-1}, {1,1,-1}, {-1,1,-1},
            {-1,-1,1}, {1,-1,1}, {1,1,1}, {-1,1,1}};
        const unsigned cube_indices[] = {0,2,1,0,3,2, 4,5,6,4,6,7, 0,1,5,0,5,4,
            3,7,6,3,6,2, 0,4,7,0,7,3, 1,2,6,1,6,5};
        std::array<Vertex, std::size(cube_indices)> cube{};
        for (size_t i = 0; i < cube.size(); ++i)
            std::copy(corners[cube_indices[i]].begin(), corners[cube_indices[i]].end(), cube[i].position);
        cube_vertices = createBuffer(sizeof(cube), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, cube.data());
        createDescriptors();
        createPipelines();
        return true;
    } catch (const std::exception& error) {
        std::cerr << "Application initialization: " << error.what() << '\n';
        shutdown();
        return false;
    }
}

void shutdown() {
    const auto& context = graphics::internal::context;
    vkDeviceWaitIdle(context.device);
    for (const auto handle : {pipeline, marker_pipeline, line_pipeline})
        if (handle) vkDestroyPipeline(context.device, handle, nullptr);
    for (const auto layout : {pipeline_layout, marker_layout})
        if (layout) vkDestroyPipelineLayout(context.device, layout, nullptr);
    if (descriptor_pool) vkDestroyDescriptorPool(context.device, descriptor_pool, nullptr);
    for (auto& buffer : model_buffers) destroyBuffer(buffer);
    for (Buffer* buffer : {&vertices, &indices, &cube_vertices, &cone_vertices, &scene_buffer, &point_buffer, &spot_buffer})
        destroyBuffer(*buffer);
    for (const auto layout : {scene_layout, model_layout})
        if (layout) vkDestroyDescriptorSetLayout(context.device, layout, nullptr);
    pipeline = marker_pipeline = line_pipeline = VK_NULL_HANDLE;
    pipeline_layout = marker_layout = VK_NULL_HANDLE;
    scene_layout = model_layout = VK_NULL_HANDLE;
    descriptor_pool = VK_NULL_HANDLE;
}

void update(double time) {
    static double previous_time = time;
    const float delta = static_cast<float>(std::clamp(time - previous_time, 0.0, 0.1));
    previous_time = time;
    cameraControls(delta);
    const auto& io = ImGui::GetIO();
    ImGui::SetNextWindowPos({12, 12}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({std::min(400.0f, io.DisplaySize.x - 24), std::max(100.0f, io.DisplaySize.y - 24)}, ImGuiCond_FirstUseEver);
    const ImVec2 available = {std::max(1.0f, io.DisplaySize.x - 24), std::max(1.0f, io.DisplaySize.y - 24)};
    ImGui::SetNextWindowSizeConstraints({std::min(240.0f, available.x), std::min(100.0f, available.y)}, available);
    ImGui::Begin("Lab 2 - Camera and Lighting");
    ImGui::TextUnformatted("Variant 10: regular dodecahedron");
    ImGui::TextUnformatted("WASD / Q,E: move; right drag: look");
    cameraUI();
    objectUI();
    lightUI();
    ImGui::ColorEdit3("Background", background_color);
    ImGui::Text("%.1f FPS", io.Framerate);
    ImGui::End();
    for (auto& object : objects) {
        if (object.animation_playing) {
            object.animation_time += delta * object.animation_speed;
            object.animated_angle = std::fmod(object.animated_angle + delta * object.animation_speed * object.spin_speed, 360.0f);
        }
        const float phase = static_cast<float>(std::fmod(object.animation_time, 6.28318530718));
        object.animated_position = {object.trajectory_radius * std::sin(phase),
            object.trajectory_height * std::sin(2 * phase), object.trajectory_radius * 0.5f * std::sin(3 * phase)};
    }
}

void render(const graphics::internal::FrameData& fd) {
    const auto& context = graphics::internal::context;
    const float aspect = static_cast<float>(context.swapchain_extent.width) / context.swapchain_extent.height;
    const Matrix vp = viewProjection(aspect);
    std::vector<PointUniform> points;
    for (const auto& light : point_lights)
        points.push_back({vector4(light.position), vector4(light.color, light.intensity)});
    std::vector<SpotUniform> spots;
    for (const auto& light : spot_lights)
        spots.push_back({vector4(light.position), vector4(normalize(light.direction)), vector4(light.color, light.intensity),
            {std::cos(radians(light.inner_angle)), std::cos(radians(light.outer_angle)), 0, 0}});
    updateStorage(point_buffer, points.data(), points.size(), sizeof(PointUniform), 1);
    updateStorage(spot_buffer, spots.data(), spots.size(), sizeof(SpotUniform), 2);
    const SceneUniform scene = {vp, vector4(camera.position, 1),
        vector4(scaleVector(cameraBasis(camera.rotation).forward, -1), camera.projection == 1 ? 1.0f : 0.0f),
        vector4(ambient_color, ambient_intensity),
        vector4(normalize(directional_direction)), vector4(directional_color, directional_intensity),
        {static_cast<uint32_t>(points.size()), static_cast<uint32_t>(spots.size()), 0, 0}};
    upload(scene_buffer, &scene, sizeof(scene));
    check(vkResetCommandBuffer(fd.command_buffer, 0), "Reset commands");
    const VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    check(vkBeginCommandBuffer(fd.command_buffer, &begin), "Begin commands");
    const VkClearValue clear[] = {
        {.color = {.float32 = {background_color[0], background_color[1], background_color[2], 1}}},
        {.depthStencil = {1, 0}},
    };
    const VkRenderPassBeginInfo pass = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = context.render_pass, .framebuffer = fd.framebuffer,
        .renderArea = {.extent = context.swapchain_extent}, .clearValueCount = 2, .pClearValues = clear,
    };
    vkCmdBeginRenderPass(fd.command_buffer, &pass, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport viewport = {0, 0, static_cast<float>(context.swapchain_extent.width),
        static_cast<float>(context.swapchain_extent.height), 0, 1};
    const VkRect2D scissor = {.extent = context.swapchain_extent};
    vkCmdSetViewport(fd.command_buffer, 0, 1, &viewport);
    vkCmdSetScissor(fd.command_buffer, 0, 1, &scissor);
    vkCmdBindPipeline(fd.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(fd.command_buffer, 0, 1, &vertices.handle, &offset);
    vkCmdBindIndexBuffer(fd.command_buffer, indices.handle, 0, VK_INDEX_TYPE_UINT16);
    for (size_t i = 0; i < objects.size(); ++i) {
        const auto& object = objects[i];
        Vec3 rotation = object.rotation;
        rotation[1] += object.animated_angle;
        const Matrix model = modelMatrix(add(object.position, object.animated_position), rotation, object.scale);
        const ModelUniform uniform = {model, normalMatrix(model, object.scale),
            vector4(object.albedo, object.vertex_colors ? 1.0f : 0.0f), vector4(object.specular, object.shininess)};
        upload(model_buffers[i], &uniform, sizeof(uniform));
        const VkDescriptorSet sets[] = {scene_set, model_sets[i]};
        vkCmdBindDescriptorSets(fd.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 2, sets, 0, nullptr);
        vkCmdDrawIndexed(fd.command_buffer, index_count, 1, 0, 0, 0);
    }
    drawMarkers(fd.command_buffer, vp);
    vkCmdEndRenderPass(fd.command_buffer);
    check(vkEndCommandBuffer(fd.command_buffer), "End commands");
}
} // namespace application
