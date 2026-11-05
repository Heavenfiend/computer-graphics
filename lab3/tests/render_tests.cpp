// Проверяем настоящий renderer и его ресурсы, не вводя тестовый API в приложение.
// Проверка пикселей и внешнего вида выполняется отдельно при визуальном осмотре.
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_vulkan.h>

#include "../source/application.cpp"

#include <functional>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error("GPU regression: " + message);
}

void requireDeviceLocal(VmaAllocation allocation, const char* name) {
    require(allocation != nullptr, std::string(name) + " has no allocation");
    VkMemoryPropertyFlags properties{};
    vmaGetAllocationMemoryProperties(graphics::internal::context.allocator, allocation, &properties);
    require((properties & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0,
        std::string(name) + " is not DEVICE_LOCAL");
}

void checkResources() {
    using namespace application;
    requireDeviceLocal(vertices.allocation, "dodecahedron vertices");
    requireDeviceLocal(indices.allocation, "dodecahedron indices");
    requireDeviceLocal(cube_vertices.allocation, "light marker vertices");
    if (cone_vertices.handle) requireDeviceLocal(cone_vertices.allocation, "cone vertices");
    for (const auto& material : materials()) {
        requireDeviceLocal(material.albedo.allocation, "albedo texture");
        if (material.specular.image) requireDeviceLocal(material.specular.allocation, "specular texture");
        if (material.emissive.image) requireDeviceLocal(material.emissive.allocation, "emissive texture");
    }
    requireDeviceLocal(blackTexture().allocation, "black fallback texture");
    require(material_sets.size() == materials().size(), "one descriptor set per material");
    require(model_buffer.handle && model_buffer.mapped, "shared model UBO exists and is mapped");
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(graphics::internal::context.physical_device, &properties);
    require(model_stride >= sizeof(ModelUniform), "dynamic stride covers structure");
    require(model_stride % properties.limits.minUniformBufferOffsetAlignment == 0,
        "dynamic offsets respect physical-device alignment");
    const size_t model_count = objects.size() + point_lights.size() + spot_lights.size() + 1;
    require(model_capacity >= model_count, "model UBO capacity covers models and markers");
    for (size_t i = 0; i < model_count; ++i) {
        const VkDeviceSize offset = i * model_stride;
        require(offset <= std::numeric_limits<uint32_t>::max(), "dynamic offset fits uint32_t");
        require(offset + sizeof(ModelUniform) <= model_buffer.size,
            "descriptor structure range plus dynamic offset stays inside shared UBO");
    }
    // Две модели одного материала должны иметь разные матрицы в одном UBO.
    ModelUniform first{}, second{};
    std::memcpy(&first, model_buffer.mapped, sizeof(first));
    std::memcpy(&second, static_cast<const char*>(model_buffer.mapped) + model_stride, sizeof(second));
    require(first.model != second.model, "different objects retain independent transforms in UBO");
}

} // namespace

int main() {
    using namespace application;
    GLFWwindow* window = nullptr;
    bool glfw_started = false, imgui_glfw_started = false, graphics_started = false, app_started = false;
    int status = EXIT_SUCCESS;
    size_t rendered_frames = 0, scenario_count = 0;
    try {
        require(glfwInit() == GLFW_TRUE, "GLFW initialization");
        glfw_started = true;
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        window = glfwCreateWindow(1280, 720, "Lab 3 GPU regression", nullptr, nullptr);
        require(window != nullptr, "hidden Vulkan window");
        glfwSetFramebufferSizeCallback(window, [](GLFWwindow*, int width, int height) {
            if (width > 0 && height > 0)
                graphics::internal::resize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
        });
        require(ImGui::CreateContext() != nullptr, "ImGui context");
        ImGui::GetIO().IniFilename = nullptr;
        require(ImGui_ImplGlfw_InitForVulkan(window, true), "ImGui GLFW backend");
        imgui_glfw_started = true;
        graphics_started = true;
        require(graphics::internal::initialize(window), "Vulkan initialization");
        app_started = true;
        require(application::initialize(), "application initialization");

        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(graphics::internal::context.physical_device, &properties);
        std::cout << "GPU regression device: " << properties.deviceName << '\n';
        std::cout << "Model bytes: " << sizeof(ModelUniform) << ", stride: " << model_stride
                  << ", minUniformBufferOffsetAlignment: " << properties.limits.minUniformBufferOffsetAlignment << '\n';
        double clock = 0;
        const auto frames = [&](const std::string& label, int count = 4) {
            for (int frame_index = 0; frame_index < count; ++frame_index) {
                glfwPollEvents();
                const auto frame = graphics::internal::prepare();
                require(frame.command_buffer != VK_NULL_HANDLE, "frame available in nonzero hidden window");
                ImGui_ImplVulkan_NewFrame();
                ImGui_ImplGlfw_NewFrame();
                ImGui::NewFrame();
                clock += 1.0 / 60.0;
                application::update(clock);
                ImGui::Render();
                application::render(frame);
                checkResources();
                graphics::internal::submitAndPresent();
                ++rendered_frames;
            }
            ++scenario_count;
            std::cout << "PASS: " << label << " (" << count << " frames)\n";
        };

        frames("initial scene with animated material and light markers");
        const auto initial_objects = objects;
        const auto initial_points = point_lights;
        const auto initial_spots = spot_lights;
        const auto initial_camera = camera;
        for (size_t a = 0; a < materials().size(); ++a)
            for (size_t b = 0; b < materials().size(); ++b) {
                objects[0].material = static_cast<int>(a);
                objects[1].material = static_cast<int>(b);
                frames("material pair " + std::to_string(a) + "/" + std::to_string(b));
            }

        objects = initial_objects;
        objects[0].material = objects[1].material = 1;
        objects[0].animated_sampling = objects[1].animated_sampling = true;
        pause_sampling = true;
        for (const double time : {0.0, 0.5, 99.999, 100.0, 100.001}) {
            sampling_time = time;
            for (const float strength : {0.0f, 1.0f}) {
                objects[0].sampling_strength = objects[1].sampling_strength = strength;
                frames("sampling time " + std::to_string(time) + ", strength " + std::to_string(strength));
                require(sampling_time == time, "paused sampling clock does not advance");
            }
        }
        pause_sampling = false;
        objects[0].sampling_speed = objects[1].sampling_speed = 0;
        frames("zero sampling speed");
        objects[0].sampling_speed = 5;
        objects[1].sampling_speed = 1;
        frames("independent animated sampling speeds");
        for (const bool specular : {false, true})
            for (const bool emissive : {false, true}) {
                for (auto& object : objects) {
                    object.animated_sampling = false;
                    object.specular_map = specular;
                    object.emissive_map = emissive;
                }
                frames("specular " + std::to_string(specular) + ", emissive " + std::to_string(emissive));
            }

        ambient_intensity = directional_intensity = 0;
        point_lights.clear();
        spot_lights.clear();
        show_markers = false;
        frames("zero lighting, emissive material enabled");
        for (auto& object : objects) object.emissive_map = false;
        frames("zero lighting and disabled emission");
        for (auto& object : objects) {
            object.material = 0;
            object.specular_map = object.emissive_map = true;
        }
        frames("missing maps use black fallback");

        objects = initial_objects;
        point_lights = initial_points;
        spot_lights = initial_spots;
        ambient_intensity = 0.08f;
        directional_intensity = 0.6f;
        show_markers = true;
        objects[0].scale = {minimum_scale, 0.25f, 1.5f};
        objects[1].scale = {1.2f, 0.35f, minimum_scale};
        objects[0].rotation = {45, 70, -15};
        objects[1].rotation = {-60, 25, 15};
        spot_lights[0].direction = {0, 0, 0};
        spot_lights[0].inner_angle = spot_lights[0].outer_angle = 30;
        for (int projection = 0; projection < 2; ++projection) {
            camera.projection = projection;
            frames(projection == 0 ? "perspective, thin nonuniform scale, zero spot direction, equal cutoff angles"
                                   : "orthographic, thin nonuniform scale, zero spot direction, equal cutoff angles");
        }
        objects = initial_objects;
        camera = initial_camera;
        objects[0].animation_playing = objects[1].animation_playing = true;
        const double previous_animation_time = objects[0].animation_time;
        frames("object trajectory and rotation animation", 12);
        require(objects[0].animation_time > previous_animation_time, "object animation advances");
        objects[0].animation_playing = objects[1].animation_playing = false;
        const double paused_animation_time = objects[0].animation_time;
        frames("object animation paused");
        require(objects[0].animation_time == paused_animation_time, "paused object animation stays fixed");

        point_lights.assign(24, Light{});
        spot_lights.assign(16, SpotLight{});
        for (size_t i = 0; i < point_lights.size(); ++i)
            point_lights[i].position = {static_cast<float>(i % 6) - 3, 1, static_cast<float>(i / 6)};
        for (size_t i = 0; i < spot_lights.size(); ++i)
            spot_lights[i].position = {static_cast<float>(i % 4) - 2, 2, static_cast<float>(i / 4)};
        const size_t capacity_before_stress = model_capacity;
        frames("24 point lights and 16 spots grow storage, model UBO and cone geometry", 8);
        require(model_capacity > capacity_before_stress, "shared dynamic UBO grew for marker models");
        require(point_buffer.size >= point_lights.size() * sizeof(PointUniform), "point storage grew");
        require(spot_buffer.size >= spot_lights.size() * sizeof(SpotUniform), "spot storage grew");
        show_markers = false;
        frames("many lights with marker visualization disabled");
        show_markers = true;
        point_lights.clear();
        spot_lights.clear();
        frames("remove every point light and spot after resource growth");
        point_lights = initial_points;
        spot_lights = initial_spots;
        frames("re-add lights after empty arrays");

        for (const auto size : {std::array<int, 2>{320, 240}, {960, 540}, {900, 900}, {1280, 720}}) {
            glfwSetWindowSize(window, size[0], size[1]);
            glfwPollEvents();
            // Явное уведомление повторяет framebuffer callback и гарантирует проверку rebuild.
            int width = 0, height = 0;
            glfwGetFramebufferSize(window, &width, &height);
            require(width > 0 && height > 0, "nonzero framebuffer after resize");
            graphics::internal::resize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
            frames("swapchain resize " + std::to_string(width) + "x" + std::to_string(height), 8);
            require(graphics::internal::context.swapchain_extent.width == static_cast<uint32_t>(width)
                    && graphics::internal::context.swapchain_extent.height == static_cast<uint32_t>(height),
                "swapchain extent matches resized framebuffer");
        }
        std::cout << "GPU regression passed: " << scenario_count << " scenarios, " << rendered_frames
                  << " rendered frames; DEVICE_LOCAL allocations and shared dynamic UBO bounds checked.\n";
        std::cout << "Pixel appearance is checked separately by visual inspection.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        status = EXIT_FAILURE;
    }

    if (graphics::internal::context.device != VK_NULL_HANDLE) {
        const VkResult wait = vkDeviceWaitIdle(graphics::internal::context.device);
        if (wait != VK_SUCCESS) {
            std::cerr << "Device wait during regression cleanup failed: " << wait << '\n';
            status = EXIT_FAILURE;
        }
    }
    if (app_started) application::shutdown();
    if (graphics_started) graphics::internal::shutdown();
    if (imgui_glfw_started) ImGui_ImplGlfw_Shutdown();
    if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
    if (window) glfwDestroyWindow(window);
    if (glfw_started) glfwTerminate();
    return status;
}
