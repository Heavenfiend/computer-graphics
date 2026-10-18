#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_vulkan.h>

#include "graphics_internal.hpp"
#include "application.hpp"

namespace {

struct Options {
	int width = 1280;
	int height = 720;
	uint32_t frame_limit = 0;
	bool hidden = false;
	bool help = false;
};

uint32_t positiveNumber(std::string_view text, uint32_t maximum) {
	uint32_t value = 0;
	const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
	if (error != std::errc{} || end != text.data() + text.size() || value == 0 || value > maximum)
		throw std::invalid_argument("Expected a positive integer within the supported range");
	return value;
}

Options parseOptions(int argc, char** argv) {
	Options options;
	for (int i = 1; i < argc; ++i) {
		const std::string_view argument = argv[i];
		if (argument == "--frames" && i + 1 < argc) {
			options.frame_limit = positiveNumber(argv[++i], std::numeric_limits<uint32_t>::max());
		} else if (argument == "--size" && i + 2 < argc) {
			options.width = static_cast<int>(positiveNumber(argv[++i], std::numeric_limits<int>::max()));
			options.height = static_cast<int>(positiveNumber(argv[++i], std::numeric_limits<int>::max()));
		} else if (argument == "--hidden") {
			options.hidden = true;
		} else if (argument == "--help") {
			options.help = true;
		} else {
			throw std::invalid_argument("Unknown or incomplete option: " + std::string(argument));
		}
	}
	if (options.hidden && options.frame_limit == 0)
		throw std::invalid_argument("--hidden requires --frames N so the process can finish automatically");
	return options;
}

} // namespace

int main(int argc, char** argv) {
	int status = EXIT_SUCCESS;
	GLFWwindow* window = nullptr;
	bool glfw_initialized = false;
	bool imgui_glfw_initialized = false;
	bool graphics_started = false;
	bool application_started = false;

	try {
		const Options options = parseOptions(argc, argv);
		if (options.help) {
			std::cout << "Usage: lab2 [--size WIDTH HEIGHT] [--frames N] [--hidden]\n";
			return EXIT_SUCCESS;
		}
		if (!glfwInit())
			throw std::runtime_error("Failed to initialize GLFW");
		glfw_initialized = true;
		glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
		glfwWindowHint(GLFW_VISIBLE, options.hidden ? GLFW_FALSE : GLFW_TRUE);
		window = glfwCreateWindow(options.width, options.height,
			"Lab 2 - Camera and Lighting", nullptr, nullptr);
		if (window == nullptr)
			throw std::runtime_error("Failed to create the GLFW window");
		glfwSetFramebufferSizeCallback(window, [](GLFWwindow*, int width, int height) {
			if (width > 0 && height > 0)
				graphics::internal::resize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
		});
		if (ImGui::CreateContext() == nullptr)
			throw std::runtime_error("Failed to create the ImGui context");
		if (!ImGui_ImplGlfw_InitForVulkan(window, true))
			throw std::runtime_error("Failed to initialize the ImGui GLFW backend");
		imgui_glfw_initialized = true;
		graphics_started = true;
		if (!graphics::internal::initialize(window))
			throw std::runtime_error("Failed to initialize graphics");
		application_started = true;
		if (!application::initialize())
			throw std::runtime_error("Failed to initialize the application");

		uint32_t rendered_frames = 0;
		while (!glfwWindowShouldClose(window)) {
			glfwPollEvents();
			int width = 0, height = 0;
			glfwGetFramebufferSize(window, &width, &height);
			if (width == 0 || height == 0) {
				glfwWaitEvents();
				continue;
			}
			// Сначала дожидаемся GPU и обрабатываем resize, затем создаём UI текущего кадра.
			const auto frame = graphics::internal::prepare();
			if (frame.command_buffer == VK_NULL_HANDLE)
				continue;
			ImGui_ImplVulkan_NewFrame();
			ImGui_ImplGlfw_NewFrame();
			ImGui::NewFrame();
			application::update(glfwGetTime());
			ImGui::Render();
			application::render(frame);
			graphics::internal::submitAndPresent();
			++rendered_frames;
			if (options.frame_limit != 0 && rendered_frames >= options.frame_limit)
				break;
		}
		if (options.frame_limit != 0)
			std::cout << "Rendered " << rendered_frames << " frames\n";
	} catch (const std::exception& error) {
		std::cerr << error.what() << '\n';
		status = EXIT_FAILURE;
	}

	// Ресурсы приложения могут ещё использоваться последним отправленным кадром.
	if (graphics::internal::context.device != VK_NULL_HANDLE) {
		const VkResult result = vkDeviceWaitIdle(graphics::internal::context.device);
		if (result != VK_SUCCESS) {
			std::cerr << "Failed to wait for the device during shutdown: " << result << '\n';
			status = EXIT_FAILURE;
		}
	}
	if (application_started)
		application::shutdown();
	if (graphics_started)
		graphics::internal::shutdown();
	if (imgui_glfw_initialized)
		ImGui_ImplGlfw_Shutdown();
	if (ImGui::GetCurrentContext() != nullptr)
		ImGui::DestroyContext();
	if (window != nullptr)
		glfwDestroyWindow(window);
	if (glfw_initialized)
		glfwTerminate();
	return status;
}
