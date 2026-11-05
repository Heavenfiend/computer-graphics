#include "graphics_internal.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include <GLFW/glfw3.h>

#include <VkBootstrap.h>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4100 4189 4324)
#endif // _MSC_VER
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif // _MSC_VER

#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>

namespace graphics::internal {

namespace {

VkInstance vk_instance;
VkDebugUtilsMessengerEXT vk_debug_messenger;
uint32_t vk_api_version;
VkSurfaceKHR vk_surface;
GLFWwindow* glfw_window;
VkQueue vk_present_queue;
uint32_t vk_present_queue_index;

VkSwapchainKHR vk_swapchain;
VkColorSpaceKHR vk_swapchain_color_space;
std::vector<VkImage> vk_swapchain_images;
std::vector<VkImageView> vk_swapchain_image_views;
uint32_t vk_swapchain_current_image;

uint32_t vk_swapchain_resize_width;
uint32_t vk_swapchain_resize_height;
bool vk_swapchain_resize_require;

VkFormat vk_depth_buffer_format = VK_FORMAT_UNDEFINED;
VkImage vk_image_depth_buffer;
VmaAllocation vma_allocation_depth_buffer;
VkImageView vk_image_view_depth_buffer;

std::vector<VkFramebuffer> vk_framebuffers;

VkSemaphore vk_semaphore_image_available;
std::vector<VkSemaphore> vk_semaphores_image_finished;
VkFence vk_fence_frame_in_flight;

VkCommandPool vk_command_pool;
VkCommandBuffer vk_command_buffer;

VkDescriptorPool vk_imgui_descriptor_pool;
VkRenderPass vk_imgui_render_pass;
std::vector<VkFramebuffer> vk_imgui_framebuffers;
VkCommandPool vk_imgui_command_pool;
VkCommandBuffer vk_imgui_command_buffer;

void check(VkResult result, const char* operation) {
	if (result != VK_SUCCESS)
		throw std::runtime_error(std::string(operation) + ": Vulkan result " + std::to_string(result));
}

bool initializeImGUIBackend() {
	const uint32_t image_count = static_cast<uint32_t>(vk_swapchain_images.size());
	if (image_count < 2) {
		std::cerr << "ImGui requires at least two swapchain images\n";
		return false;
	}
	ImGui_ImplVulkan_InitInfo init = {
		.ApiVersion = vk_api_version,
		.Instance = vk_instance,
		.PhysicalDevice = context.physical_device,
		.Device = context.device,
		.QueueFamily = context.graphics_queue_index,
		.Queue = context.graphics_queue,
		.DescriptorPool = vk_imgui_descriptor_pool,
		.MinImageCount = image_count,
		.ImageCount = image_count,
		.PipelineInfoMain = {
			.RenderPass = vk_imgui_render_pass,
		},
		.CheckVkResultFn = [](VkResult result) { check(result, "ImGui Vulkan backend"); },
	};
	return ImGui_ImplVulkan_Init(&init);
}

bool createImageFinishedSemaphores() {
	for (VkSemaphore semaphore : vk_semaphores_image_finished)
		vkDestroySemaphore(context.device, semaphore, nullptr);
	vk_semaphores_image_finished.assign(vk_swapchain_images.size(), VK_NULL_HANDLE);
	const VkSemaphoreCreateInfo create_info = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
	for (auto& semaphore : vk_semaphores_image_finished) {
		if (vkCreateSemaphore(context.device, &create_info, nullptr, &semaphore) != VK_SUCCESS) {
			std::cerr << "Failed to create a semaphore for a finished swapchain image\n";
			return false;
		}
	}
	return true;
}

bool readSwapchainImages(vkb::Swapchain& swapchain) {
	auto images = swapchain.get_images();
	if (!images) {
		std::cerr << images.error().message() << '\n';
		return false;
	}
	vk_swapchain_images = std::move(images.value());
	vk_swapchain_image_views.assign(vk_swapchain_images.size(), VK_NULL_HANDLE);
	for (size_t i = 0; i < vk_swapchain_images.size(); ++i) {
		const VkImageViewCreateInfo view = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image = vk_swapchain_images[i],
			.viewType = VK_IMAGE_VIEW_TYPE_2D,
			.format = swapchain.image_format,
			.subresourceRange = {
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.levelCount = 1,
				.layerCount = 1,
			},
		};
		if (vkCreateImageView(context.device, &view, nullptr, &vk_swapchain_image_views[i]) != VK_SUCCESS) {
			std::cerr << "Failed to create a swapchain image view\n";
			return false;
		}
	}
	return true;
}

VkFormat selectDepthFormat(VkPhysicalDevice physical_device) {
	// сначала пробуем исходный формат глубины, потом другие со stencil
	const VkFormat candidates[] = {
		VK_FORMAT_D24_UNORM_S8_UINT,
		VK_FORMAT_D32_SFLOAT_S8_UINT,
	};

	for (VkFormat format : candidates) {
		VkFormatProperties properties{};
		vkGetPhysicalDeviceFormatProperties(physical_device, format, &properties);
		if (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
			return format;
		}
	}

	return VK_FORMAT_UNDEFINED;
}

bool initializeImGUI() {
	const VkDescriptorPoolSize descriptor_pool_sizes[] = {
		{
			.type = VK_DESCRIPTOR_TYPE_SAMPLER,
			.descriptorCount = IMGUI_IMPL_VULKAN_MINIMUM_SAMPLER_POOL_SIZE,
		},
		{
			.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
			.descriptorCount = IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE,
		},
	};

	const VkDescriptorPoolCreateInfo descriptor_pool = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
		// imgui нужны отдельные наборы дескрипторов для текстур и сэмплеров
		.maxSets = IMGUI_IMPL_VULKAN_MINIMUM_SAMPLER_POOL_SIZE
		         + IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE,
		.poolSizeCount = sizeof(descriptor_pool_sizes) / sizeof(descriptor_pool_sizes[0]),
		.pPoolSizes = descriptor_pool_sizes,
	};

	if (vkCreateDescriptorPool(context.device, &descriptor_pool, nullptr,
							   &vk_imgui_descriptor_pool) != VK_SUCCESS) {
		std::cerr << "Failed to create Vulkan descriptor pool for ImGUI rendering\n";
		return false;
	}

	const VkAttachmentDescription render_pass_attachment = {
		.format = context.swapchain_format,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
	};

	const VkAttachmentReference render_pass_attachment_ref = {
		.attachment = 0,
		.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	};

	const VkSubpassDescription render_pass_subpass = {
		.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = 1,
		.pColorAttachments = &render_pass_attachment_ref,
	};

	const VkSubpassDependency render_pass_dependency = {
		.srcSubpass = VK_SUBPASS_EXTERNAL,
		.dstSubpass = 0,
		// Цвет сцены должен быть записан до LOAD и смешивания интерфейса.
		.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
						 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT,
	};

	const VkRenderPassCreateInfo render_pass = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &render_pass_attachment,
		.subpassCount = 1,
		.pSubpasses = &render_pass_subpass,
		.dependencyCount = 1,
		.pDependencies = &render_pass_dependency,
	};

	if (vkCreateRenderPass(context.device, &render_pass, nullptr,
						   &vk_imgui_render_pass) != VK_SUCCESS) {
		std::cerr << "Failed to create Vulkan render pass for ImGUI rendering\n";
		return false;
	}

	const uint32_t swapchain_images_count = uint32_t(vk_swapchain_images.size());

	vk_imgui_framebuffers.resize(swapchain_images_count);

	VkFramebufferCreateInfo framebuffer = {
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.renderPass = vk_imgui_render_pass,
		.attachmentCount = 1,
		.width = context.swapchain_extent.width,
		.height = context.swapchain_extent.height,
		.layers = 1,
	};

	for (uint32_t i = 0; i < swapchain_images_count; ++i) {
		framebuffer.pAttachments = &vk_swapchain_image_views[i];

		if (vkCreateFramebuffer(context.device, &framebuffer, nullptr,
								&vk_imgui_framebuffers[i]) != VK_SUCCESS) {
			std::cerr << "Failed to create Vulkan framebuffer #" << i << " for ImGUI rendering\n";
			return false;
		}
	}

	const VkCommandPoolCreateInfo command_pool = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = context.graphics_queue_index,
	};

	if (vkCreateCommandPool(context.device, &command_pool, nullptr,
							&vk_imgui_command_pool) != VK_SUCCESS) {
		std::cerr << "Failed to create Vulkan command pool for ImGUI rendering\n";
		return false;
	}

	const VkCommandBufferAllocateInfo command_buffer = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = vk_imgui_command_pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};

	if (vkAllocateCommandBuffers(context.device, &command_buffer,
								 &vk_imgui_command_buffer) != VK_SUCCESS) {
		std::cerr << "Failed to create Vulkan command buffer for ImGUI rendering\n";
		return false;
	}

	return initializeImGUIBackend();
}

void drawImGUI() {
	check(vkResetCommandBuffer(vk_imgui_command_buffer, 0), "Reset ImGui command buffer");

	const VkCommandBufferBeginInfo command_buffer_begin = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};

	check(vkBeginCommandBuffer(vk_imgui_command_buffer, &command_buffer_begin), "Begin ImGui command buffer");

	const VkRenderPassBeginInfo render_pass_begin = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = vk_imgui_render_pass,
		.framebuffer = vk_imgui_framebuffers[vk_swapchain_current_image],
		.renderArea = { .extent = context.swapchain_extent },
	};

	vkCmdBeginRenderPass(vk_imgui_command_buffer, &render_pass_begin, VK_SUBPASS_CONTENTS_INLINE);

	ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), vk_imgui_command_buffer);

	vkCmdEndRenderPass(vk_imgui_command_buffer);

	check(vkEndCommandBuffer(vk_imgui_command_buffer), "End ImGui command buffer");
}

bool rebuildSwapchain(uint32_t width, uint32_t height) {
	// первый кадр может потребовать пересоздания swapchain ещё до изменения размера окна
	if (width == 0 || height == 0) {
		width = context.swapchain_extent.width;
		height = context.swapchain_extent.height;
	}
	if (width == 0 || height == 0) {
		return false;
	}
	check(vkDeviceWaitIdle(context.device), "Wait for device before swapchain recreation");

	vkb::SwapchainBuilder sb(context.physical_device, context.device, vk_surface,
	                         context.graphics_queue_index, vk_present_queue_index);

	auto sb_result = sb.set_desired_extent(width, height)
	                   .set_desired_format({context.swapchain_format, vk_swapchain_color_space})
					   .set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)
					   .use_default_image_usage_flags()
					   .set_old_swapchain(vk_swapchain)
					   .build();
	if (!sb_result) {
		std::cerr << sb_result.error().message() << '\n';
		return false;
	}

	auto vkb_swapchain = sb_result.value();
	if (vkb_swapchain.image_format != context.swapchain_format) {
		std::cerr << "The surface format changed; restart the application to recreate the render pipelines\n";
		vkDestroySwapchainKHR(context.device, vkb_swapchain.swapchain, nullptr);
		return false;
	}
	const size_t previous_image_count = vk_swapchain_images.size();

	// Attachment views и depth image освобождаются только после всех использующих их framebuffer.
	for (VkFramebuffer framebuffer : vk_imgui_framebuffers)
		vkDestroyFramebuffer(context.device, framebuffer, nullptr);
	vk_imgui_framebuffers.clear();
	for (VkFramebuffer framebuffer : vk_framebuffers)
		vkDestroyFramebuffer(context.device, framebuffer, nullptr);
	vk_framebuffers.clear();
	for (VkImageView view : vk_swapchain_image_views)
		vkDestroyImageView(context.device, view, nullptr);
	vk_swapchain_image_views.clear();

	vkDestroySwapchainKHR(context.device, vk_swapchain, nullptr);

	vk_swapchain = vkb_swapchain.swapchain;
	vk_swapchain_color_space = vkb_swapchain.color_space;
	context.swapchain_extent = vkb_swapchain.extent;
	if (!readSwapchainImages(vkb_swapchain))
		return false;

	vkDestroyImageView(context.device, vk_image_view_depth_buffer, nullptr);
	vmaDestroyImage(context.allocator, vk_image_depth_buffer, vma_allocation_depth_buffer);
	vk_image_view_depth_buffer = VK_NULL_HANDLE;
	vk_image_depth_buffer = VK_NULL_HANDLE;
	vma_allocation_depth_buffer = nullptr;

	const VkImageCreateInfo depth_buffer = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = vk_depth_buffer_format,
		.extent = { context.swapchain_extent.width, context.swapchain_extent.height, 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};

	const VmaAllocationCreateInfo depth_buffer_allocation = {
		.usage = VMA_MEMORY_USAGE_AUTO,
	};

	if (vmaCreateImage(context.allocator, &depth_buffer, &depth_buffer_allocation,
					   &vk_image_depth_buffer, &vma_allocation_depth_buffer,
					   nullptr) != VK_SUCCESS) {
		std::cerr << "Failed to allocate and create Vulkan image for depth buffer\n";
		return false;
	}

	const VkImageViewCreateInfo depth_buffer_view = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = vk_image_depth_buffer,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = vk_depth_buffer_format,
		.subresourceRange = {
			.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
			.baseMipLevel = 0,
			.levelCount = 1,
			.baseArrayLayer = 0,
			.layerCount = 1,
		},
	};

	if (vkCreateImageView(context.device, &depth_buffer_view, nullptr,
						  &vk_image_view_depth_buffer) != VK_SUCCESS) {
		std::cerr << "Failed to create Vulkan image view for depth buffer\n";
		return false;
	}

	const uint32_t swapchain_images_count = uint32_t(vk_swapchain_images.size());

	VkImageView framebuffer_attachments[] = {
		VK_NULL_HANDLE,
		vk_image_view_depth_buffer,
	};

	const VkFramebufferCreateInfo framebuffer = {
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.renderPass = context.render_pass,
		.attachmentCount = sizeof(framebuffer_attachments) / sizeof(framebuffer_attachments[0]),
		.pAttachments = framebuffer_attachments,
		.width = context.swapchain_extent.width,
		.height = context.swapchain_extent.height,
		.layers = 1,
	};

	vk_framebuffers.resize(swapchain_images_count);

	for (uint32_t i = 0; i < swapchain_images_count; ++i) {
		framebuffer_attachments[0] = vk_swapchain_image_views[i];

		if (vkCreateFramebuffer(context.device, &framebuffer, nullptr,
								&vk_framebuffers[i]) != VK_SUCCESS) {
			std::cerr << "Failed to create Vulkan framebuffer #" << i << '\n';
			return false;
		}
	}

	vk_imgui_framebuffers.resize(swapchain_images_count);

	VkFramebufferCreateInfo imgui_framebuffer = {
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.renderPass = vk_imgui_render_pass,
		.attachmentCount = 1,
		.width = context.swapchain_extent.width,
		.height = context.swapchain_extent.height,
		.layers = 1,
	};

	for (uint32_t i = 0; i < swapchain_images_count; ++i) {
		imgui_framebuffer.pAttachments = &vk_swapchain_image_views[i];

		if (vkCreateFramebuffer(context.device, &imgui_framebuffer, nullptr,
								&vk_imgui_framebuffers[i]) != VK_SUCCESS) {
			std::cerr << "Failed to create Vulkan framebuffer #" << i << " for ImGUI rendering\n";
			return false;
		}
	}

	if (!createImageFinishedSemaphores())
		return false;
	if (previous_image_count != vk_swapchain_images.size()) {
		// SetMinImageCount в ImGui 1.92 не меняет ImageCount внутренних render buffers.
		ImGui_ImplVulkan_Shutdown();
		if (!initializeImGUIBackend())
			return false;
	}
	vk_swapchain_resize_require = false;

	return true;
}

} // namespace

Context context;

bool initialize(GLFWwindow* const window) {
	glfw_window = window;
	auto system_info = vkb::SystemInfo::get_system_info();
	if (!system_info) {
		std::cerr << system_info.error().message() << '\n';
		return false;
	}
	const bool validation_available = system_info.value().is_layer_available("VK_LAYER_KHRONOS_validation");
	std::cout << "Validation layer: " << (validation_available ? "enabled" : "unavailable") << '\n';
	vkb::InstanceBuilder ib;

	auto ibr = ib.require_api_version(VK_MAKE_VERSION(1, 1, 0))
				 .enable_validation_layers(validation_available)
				 .use_default_debug_messenger()
				 .build();
	if (!ibr) {
		std::cerr << ibr.error().message() << '\n';
		return false;
	}

	auto vkb_instance = ibr.value();
	vk_instance = vkb_instance.instance;
	vk_debug_messenger = vkb_instance.debug_messenger;
	vk_api_version = vkb_instance.api_version;

	if (glfwCreateWindowSurface(vk_instance, window, nullptr, &vk_surface) != VK_SUCCESS) {
		const char *message = nullptr;
		glfwGetError(&message);
		std::cerr << message << '\n';
		return false;
	}

	vkb::PhysicalDeviceSelector pds(vkb_instance, vk_surface);

	auto pds_result = pds.prefer_gpu_device_type(vkb::PreferredDeviceType::discrete)
						 .require_present()
						 .select();
	if (!pds_result) {
		std::cerr << pds_result.error().message() << '\n';
		return false;
	}

	auto vkb_physical_device = pds_result.value();
	std::cout << "Vulkan device: " << vkb_physical_device.properties.deviceName << '\n';

	vk_depth_buffer_format = selectDepthFormat(vkb_physical_device.physical_device);
	if (vk_depth_buffer_format == VK_FORMAT_UNDEFINED) {
		std::cerr << "No supported depth/stencil attachment format found\n";
		return false;
	}

	vkb::DeviceBuilder db(vkb_physical_device);

	auto db_result = db.build();
	if (!db_result) {
		std::cerr << db_result.error().message() << '\n';
		return false;
	}

	auto vkb_device = db_result.value();

	context.physical_device = vkb_device.physical_device;
	context.device = vkb_device.device;

	if (auto result = vkb_device.get_queue(vkb::QueueType::graphics); result) {
		context.graphics_queue = result.value();
	} else {
		std::cerr << result.error().message() << '\n';
		return false;
	}

	if (auto result = vkb_device.get_queue_index(vkb::QueueType::graphics); result) {
		context.graphics_queue_index = result.value();
	} else {
		std::cerr << result.error().message() << '\n';
		return false;
	}
	if (auto result = vkb_device.get_queue(vkb::QueueType::present); result) {
		vk_present_queue = result.value();
	} else {
		std::cerr << result.error().message() << '\n';
		return false;
	}
	if (auto result = vkb_device.get_queue_index(vkb::QueueType::present); result) {
		vk_present_queue_index = result.value();
	} else {
		std::cerr << result.error().message() << '\n';
		return false;
	}

	const VmaAllocatorCreateInfo allocator = {
		.physicalDevice = context.physical_device,
		.device = context.device,
		.instance = vk_instance,
		.vulkanApiVersion = vk_api_version,
	};

	if (vmaCreateAllocator(&allocator, &context.allocator) != VK_SUCCESS) {
		std::cerr << "Failed to create Vulkan Memory Allocator\n";
		return false;
	}

	vkb::SwapchainBuilder sb(vkb_device);

	auto sb_result = sb.use_default_format_selection()
					   .set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)
					   .use_default_image_usage_flags()
					   .build();
	if (!sb_result) {
		std::cerr << sb_result.error().message() << '\n';
		return false;
	}

	auto vkb_swapchain = sb_result.value();

	vk_swapchain = vkb_swapchain.swapchain;
	vk_swapchain_color_space = vkb_swapchain.color_space;
	context.swapchain_format = vkb_swapchain.image_format;
	context.swapchain_extent = vkb_swapchain.extent;
	if (!readSwapchainImages(vkb_swapchain))
		return false;
	vk_swapchain_current_image = UINT32_MAX;

	const uint32_t swapchain_images_count = uint32_t(vk_swapchain_images.size());

	const VkImageCreateInfo depth_buffer = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = vk_depth_buffer_format,
		.extent = { context.swapchain_extent.width, context.swapchain_extent.height, 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};

	const VmaAllocationCreateInfo depth_buffer_allocation = {
		.usage = VMA_MEMORY_USAGE_AUTO,
	};

	if (vmaCreateImage(context.allocator, &depth_buffer, &depth_buffer_allocation,
					   &vk_image_depth_buffer, &vma_allocation_depth_buffer,
					   nullptr) != VK_SUCCESS) {
		std::cerr << "Failed to allocate and create Vulkan image for depth buffer\n";
		return false;
	}

	const VkImageViewCreateInfo depth_buffer_view = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = vk_image_depth_buffer,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = vk_depth_buffer_format,
		.subresourceRange = {
			.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
			.baseMipLevel = 0,
			.levelCount = 1,
			.baseArrayLayer = 0,
			.layerCount = 1,
		},
	};

	if (vkCreateImageView(context.device, &depth_buffer_view, nullptr,
						  &vk_image_view_depth_buffer) != VK_SUCCESS) {
		std::cerr << "Failed to create Vulkan image view for depth buffer\n";
		return false;
	}

	const VkAttachmentDescription render_pass_attachments[] = {
		{
			.format = context.swapchain_format,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		},
		{
			.format = vk_depth_buffer_format,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
		},
	};

	const VkAttachmentReference render_pass_color_attachment = {
		.attachment = 0,
		.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	};

	const VkAttachmentReference render_pass_depth_attachment = {
		.attachment = 1,
		.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
	};

	const VkSubpassDescription render_pass_subpass = {
		.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = 1,
		.pColorAttachments = &render_pass_color_attachment,
		.pDepthStencilAttachment = &render_pass_depth_attachment,
	};
	// Автоматический переход layout должен ждать acquire semaphore на COLOR_ATTACHMENT_OUTPUT.
	// Неявная dependency с TOP_OF_PIPE не входит в выбранный wait stage при submit.
	const VkSubpassDependency render_pass_dependency = {
		.srcSubpass = VK_SUBPASS_EXTERNAL,
		.dstSubpass = 0,
		.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
			VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
		.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
			VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
		.srcAccessMask = 0,
		.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
			VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
	};

	const VkRenderPassCreateInfo render_pass = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = sizeof(render_pass_attachments) / sizeof(render_pass_attachments[0]),
		.pAttachments = render_pass_attachments,
		.subpassCount = 1,
		.pSubpasses = &render_pass_subpass,
		.dependencyCount = 1,
		.pDependencies = &render_pass_dependency,
	};

	if (vkCreateRenderPass(context.device, &render_pass, nullptr, &context.render_pass) != VK_SUCCESS) {
		std::cerr << "Failed to create Vulkan render pass\n";
		return false;
	}

	VkImageView framebuffer_attachments[] = {
		VK_NULL_HANDLE,
		vk_image_view_depth_buffer
	};

	const VkFramebufferCreateInfo framebuffer = {
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.renderPass = context.render_pass,
		.attachmentCount = sizeof(framebuffer_attachments) / sizeof(framebuffer_attachments[0]),
		.pAttachments = framebuffer_attachments,
		.width = context.swapchain_extent.width,
		.height = context.swapchain_extent.height,
		.layers = 1,
	};

	vk_framebuffers.resize(swapchain_images_count);

	for (uint32_t i = 0; i < swapchain_images_count; ++i) {
		framebuffer_attachments[0] = vk_swapchain_image_views[i];

		if (vkCreateFramebuffer(context.device, &framebuffer, nullptr,
								&vk_framebuffers[i]) != VK_SUCCESS) {
			std::cerr << "Failed to create Vulkan framebuffer #" << i << '\n';
			return false;
		}
	}

	const VkSemaphoreCreateInfo semaphore = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };

	const VkFenceCreateInfo fence = {
		.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
		.flags = VK_FENCE_CREATE_SIGNALED_BIT,
	};

	if (!createImageFinishedSemaphores())
		return false;

	if (vkCreateSemaphore(context.device, &semaphore, NULL,
						  &vk_semaphore_image_available) != VK_SUCCESS) {
		std::cerr << "Failed to create Vulkan semaphore for available image\n";
		return false;
	}

	if (vkCreateFence(context.device, &fence, nullptr, &vk_fence_frame_in_flight) != VK_SUCCESS) {
		std::cerr << "Failed to create Vulkan semaphore for image in flight\n";
		return false;
	}

	const VkCommandPoolCreateInfo command_pool = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = context.graphics_queue_index,
	};

	if (vkCreateCommandPool(context.device, &command_pool, nullptr, &vk_command_pool) != VK_SUCCESS) {
		std::cerr << "Failed to create Vulkan command pool\n";
		return false;
	}

	const VkCommandBufferAllocateInfo command_buffers = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = vk_command_pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};

	if (vkAllocateCommandBuffers(context.device, &command_buffers, &vk_command_buffer) != VK_SUCCESS) {
		std::cerr << "Failed to allocate Vulkan command buffer\n";
		return false;
	}

	if (!initializeImGUI()) {
		std::cerr << "Failed to initialize ImGUI Vulkan rendering backend\n";
		return false;
	}

	return true;
}

void shutdown() {
	// initialize может завершиться на любом шаге; нулевые handles не требуют освобождения.
	if (context.device != VK_NULL_HANDLE) {
		vkDeviceWaitIdle(context.device);
		if (ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().BackendRendererUserData != nullptr)
			ImGui_ImplVulkan_Shutdown();
		vkDestroyCommandPool(context.device, vk_imgui_command_pool, nullptr);
		for (VkFramebuffer framebuffer : vk_imgui_framebuffers)
			vkDestroyFramebuffer(context.device, framebuffer, nullptr);
		vkDestroyRenderPass(context.device, vk_imgui_render_pass, nullptr);
		vkDestroyDescriptorPool(context.device, vk_imgui_descriptor_pool, nullptr);
		vkDestroyCommandPool(context.device, vk_command_pool, nullptr);
		vkDestroyFence(context.device, vk_fence_frame_in_flight, nullptr);
		for (VkSemaphore semaphore : vk_semaphores_image_finished)
			vkDestroySemaphore(context.device, semaphore, nullptr);
		vkDestroySemaphore(context.device, vk_semaphore_image_available, nullptr);
		for (VkFramebuffer framebuffer : vk_framebuffers)
			vkDestroyFramebuffer(context.device, framebuffer, nullptr);
		vkDestroyRenderPass(context.device, context.render_pass, nullptr);
		vkDestroyImageView(context.device, vk_image_view_depth_buffer, nullptr);
		if (vk_image_depth_buffer != VK_NULL_HANDLE)
			vmaDestroyImage(context.allocator, vk_image_depth_buffer, vma_allocation_depth_buffer);
		for (VkImageView view : vk_swapchain_image_views)
			vkDestroyImageView(context.device, view, nullptr);
		vkDestroySwapchainKHR(context.device, vk_swapchain, nullptr);
		if (context.allocator != nullptr)
			vmaDestroyAllocator(context.allocator);
		vkDestroyDevice(context.device, nullptr);
	}
	if (vk_instance != VK_NULL_HANDLE) {
		vkDestroySurfaceKHR(vk_instance, vk_surface, nullptr);
		if (vk_debug_messenger != VK_NULL_HANDLE)
			vkb::destroy_debug_utils_messenger(vk_instance, vk_debug_messenger);
		vkDestroyInstance(vk_instance, nullptr);
	}
	context = {};
	vk_instance = VK_NULL_HANDLE;
}

void resize(uint32_t width, uint32_t height) {
	if (width == 0 || height == 0) {
		return;
	}

	vk_swapchain_resize_width = width;
	vk_swapchain_resize_height = height;

	vk_swapchain_resize_require = true;
}

FrameData prepare() {
	check(vkWaitForFences(context.device, 1, &vk_fence_frame_in_flight, VK_TRUE, UINT64_MAX),
		"Wait for the previous frame");
	if (vk_swapchain_resize_require &&
		!rebuildSwapchain(vk_swapchain_resize_width, vk_swapchain_resize_height))
		throw std::runtime_error("Failed to resize the swapchain");

retry_acquire:
	const VkResult acquire_result = vkAcquireNextImageKHR(context.device, vk_swapchain, UINT64_MAX,
		vk_semaphore_image_available, VK_NULL_HANDLE, &vk_swapchain_current_image);
	switch (acquire_result) {
	case VK_SUCCESS:
		break;

	case VK_ERROR_OUT_OF_DATE_KHR: {
		int width = 0, height = 0;
		glfwGetFramebufferSize(glfw_window, &width, &height);
		if (width == 0 || height == 0)
			return {};
		if (!rebuildSwapchain(static_cast<uint32_t>(width), static_cast<uint32_t>(height)))
			throw std::runtime_error("Failed to recreate the out-of-date swapchain");
		goto retry_acquire;
	}

	case VK_SUBOPTIMAL_KHR:
		resize(context.swapchain_extent.width, context.swapchain_extent.height);
		break;

	default:
		check(acquire_result, "Acquire swapchain image");
	}

	return {
		.framebuffer = vk_framebuffers[vk_swapchain_current_image],
		.command_buffer = vk_command_buffer,
	};
}

void submitAndPresent() {
	drawImGUI();

	const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	const VkCommandBuffer command_buffers[] = {
		vk_command_buffer,
		vk_imgui_command_buffer,
	};

	const VkSubmitInfo submit = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.waitSemaphoreCount = 1,
		.pWaitSemaphores = &vk_semaphore_image_available,
		.pWaitDstStageMask = &stage,
		.commandBufferCount = sizeof(command_buffers) / sizeof(command_buffers[0]),
		.pCommandBuffers = command_buffers,
		.signalSemaphoreCount = 1,
		.pSignalSemaphores = &vk_semaphores_image_finished[vk_swapchain_current_image],
	};

	check(vkResetFences(context.device, 1, &vk_fence_frame_in_flight), "Reset frame fence");
	check(vkQueueSubmit(context.graphics_queue, 1, &submit, vk_fence_frame_in_flight), "Submit frame");

	const VkPresentInfoKHR present = {
		.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
		.waitSemaphoreCount = 1,
		.pWaitSemaphores = &vk_semaphores_image_finished[vk_swapchain_current_image],
		.swapchainCount = 1,
		.pSwapchains = &vk_swapchain,
		.pImageIndices = &vk_swapchain_current_image,
	};

	VkResult result = vkQueuePresentKHR(vk_present_queue, &present);
	if (result != VK_SUCCESS && result != VK_ERROR_OUT_OF_DATE_KHR && result != VK_SUBOPTIMAL_KHR)
		check(result, "Present swapchain image");
	if (result == VK_ERROR_OUT_OF_DATE_KHR ||
	    result == VK_SUBOPTIMAL_KHR ||
	    vk_swapchain_resize_require) {
		// Пересоздаём до NewFrame, чтобы ImGui draw data не ссылались на старые ресурсы backend.
		int width = 0, height = 0;
		glfwGetFramebufferSize(glfw_window, &width, &height);
		if (width > 0 && height > 0)
			resize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
	}
}

} // namespace graphics::internal
