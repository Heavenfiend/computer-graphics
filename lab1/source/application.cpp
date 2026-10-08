#include "application.hpp"
#include "dodecahedron.hpp"

#include <imgui.h>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace application {

namespace {
// красный, зелёный, синий и прозрачность; значения от 0 до 1
float background_color[4] = { 0.04f, 0.05f, 0.07f, 1.0f };
int projection_type = 0; // 0 — перспектива, 1 — ортографическая проекция

// у каждой фигуры свои настройки
struct SceneObject {
	std::array<float, 3> figure_color = {1, 1, 1};
	bool vertex_colors = true;
	std::array<float, 3> position = {0, 0, 0};
	std::array<float, 3> rotation = {-17.2f, 25.8f, 0};
	std::array<float, 3> scale = {0.5f, 0.5f, 0.5f};
	bool animation_playing = false;
	double animation_time = 0;
	float animation_speed = 1.0f;
	float trajectory_radius = 0.8f;
	float trajectory_height = 0.5f;
	float spin_speed = 40.0f; // градусов за секунду анимации
	std::array<float, 3> animated_position{};
	float animated_angle = 0;
};
std::array<SceneObject, 2> objects;
int selected_object = 0;

// дескриптор указывает шейдеру, из какого буфера брать настройки конкретной фигуры
struct ObjectResources {
	VkBuffer uniform_buffer{};
	VmaAllocation allocation{};
	void* mapped{};
	VkDescriptorSet descriptor_set{};
};
std::array<ObjectResources, 2> object_resources{};
VkDescriptorSetLayout descriptor_layout{};
VkDescriptorPool descriptor_pool{};

// в одном буфере лежат вершины, в другом - номера вершин для треугольников
VkBuffer vertex_buffer{}, index_buffer{};
VmaAllocation vertex_allocation{}, index_allocation{};
VkPipelineLayout pipeline_layout{};
VkPipeline pipeline{};

void check(VkResult result, const char* operation) {
	// если vulkan вернул ошибку, дальше с этим ресурсом работать нельзя
	if (result != VK_SUCCESS)
		throw std::runtime_error(std::string(operation) + ": VkResult " + std::to_string(result));
}

// храним матрицу 4 на 4 по столбцам, как её ожидает шейдер
using Matrix = std::array<float, 16>;
struct Transform {
	Matrix mvp;
	// у матрицы нормалей 3 столбца, каждый занимает 4 float из-за выравнивания в шейдере
	std::array<float, 12> normal_matrix;
	std::array<float, 4> tint;
};
static_assert(sizeof(Transform) == 128);
static_assert(offsetof(Transform, normal_matrix) == 64 && offsetof(Transform, tint) == 112);
Matrix multiply(const Matrix& a, const Matrix& b) {
	Matrix result{};
	for (int column = 0; column < 4; ++column)
		for (int row = 0; row < 4; ++row)
			for (int k = 0; k < 4; ++k)
				result[column*4 + row] += a[k*4 + row] * b[column*4 + k];
	return result;
}

Matrix modelMatrix(const SceneObject& object) {
	const auto& position = object.position;
	const auto& rotation = object.rotation;
	const auto& scale = object.scale;
	const auto& animated_position = object.animated_position;
	const float animated_angle = object.animated_angle;
	// в интерфейсе градусы, а sin и cos ждут радианы
	constexpr float radians = 3.14159265359f / 180.0f;
	const float x = rotation[0]*radians, y = (rotation[1]+animated_angle)*radians, z = rotation[2]*radians;
	const Matrix rotate_x = {1,0,0,0, 0,std::cos(x),std::sin(x),0,
		0,-std::sin(x),std::cos(x),0, 0,0,0,1};
	const Matrix rotate_y = {std::cos(y),0,-std::sin(y),0, 0,1,0,0,
		std::sin(y),0,std::cos(y),0, 0,0,0,1};
	const Matrix rotate_z = {std::cos(z),std::sin(z),0,0, -std::sin(z),std::cos(z),0,0,
		0,0,1,0, 0,0,0,1};
	const Matrix stretch = {scale[0],0,0,0, 0,scale[1],0,0, 0,0,scale[2],0, 0,0,0,1};
	const Matrix translation = {1,0,0,0, 0,1,0,0, 0,0,1,0,
		position[0]+animated_position[0],position[1]+animated_position[1],position[2]+animated_position[2],1};
	// сначала растягиваем, потом поворачиваем вокруг x, y, z и переносим на нужное место
	return multiply(translation, multiply(rotate_z, multiply(rotate_y, multiply(rotate_x, stretch))));
}

Matrix sceneMatrix(float aspect) {
	// сдвигаем сцену на 6 единиц от камеры
	const Matrix view = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,-6,1};
	const float near_plane = 0.1f, far_plane = 100.0f;
	// угол обзора сейчас 45 градусов; aspect — ширина окна, делённая на высоту
	const float f = 1.0f / std::tan(0.785398163f / 2.0f);
	// в vulkan глубина от 0 до 1, а знак y меняем, чтобы картинка не была перевёрнутой
	Matrix projection = {f/aspect,0,0,0, 0,-f,0,0,
		0,0,far_plane/(near_plane-far_plane),-1,
		0,0,near_plane*far_plane/(near_plane-far_plane),0};
	if (projection_type == 1) {
		// подбираем размер области под перспективу на расстоянии 6 от камеры
		const float half_height = 6.0f / f;
		const float half_width = half_height * aspect;
		// здесь размер на экране не зависит от расстояния до камеры
		projection = {1/half_width,0,0,0, 0,-1/half_height,0,0,
			0,0,1/(near_plane-far_plane),0,
			0,0,near_plane/(near_plane-far_plane),1};
	}
	// положение и поворот самой фигуры задаются отдельно в modelMatrix
	return multiply(projection, view);
}

void createBuffer(const void* data, VkDeviceSize size, VkBufferUsageFlags usage,
	VkBuffer& buffer, VmaAllocation& allocation) {
	const VkBufferCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = size, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	// просим память, в которую можно записать данные прямо из нашей программы
	const VmaAllocationCreateInfo memory = {
		.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
		.usage = VMA_MEMORY_USAGE_AUTO,
	};
	VmaAllocationInfo allocated{};
	auto allocator = graphics::internal::context.allocator;
	check(vmaCreateBuffer(allocator, &info, &memory, &buffer, &allocation, &allocated), "Create mesh buffer");
	std::memcpy(allocated.pMappedData, data, static_cast<size_t>(size));
	// делаем записанные данные видимыми для видеокарты, если память требует этого
	check(vmaFlushAllocation(allocator, allocation, 0, size), "Flush mesh buffer");
}

VkShaderModule loadShader(const char* path) {
	// читаем уже скомпилированный шейдер .spv, а не исходный текст .vert или .frag
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	if (!file) throw std::runtime_error(std::string("Cannot open shader: ") + path);
	const auto size = file.tellg();
	if (size <= 0 || size % 4 != 0) throw std::runtime_error("Invalid SPIR-V size");
	// код spir-v состоит из чисел по 4 байта
	std::vector<uint32_t> code(static_cast<size_t>(size) / 4);
	file.seekg(0);
	if (!file.read(reinterpret_cast<char*>(code.data()), size))
		throw std::runtime_error("Cannot read shader");
	const VkShaderModuleCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = static_cast<size_t>(size), .pCode = code.data(),
	};
	VkShaderModule shader{};
	check(vkCreateShaderModule(graphics::internal::context.device, &info, nullptr, &shader), "Create shader");
	return shader;
}

void createDescriptors() {
	const auto& context = graphics::internal::context;
	// у всех объектов одинаковый формат данных: один uniform-буфер в binding 0
	const VkDescriptorSetLayoutBinding binding = {
		.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		.descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
	};
	const VkDescriptorSetLayoutCreateInfo layout = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 1, .pBindings = &binding,
	};
	check(vkCreateDescriptorSetLayout(context.device, &layout, nullptr, &descriptor_layout), "Create descriptor layout");
	// выделяем место ровно под два набора, по одному на каждую фигуру
	const VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2};
	const VkDescriptorPoolCreateInfo pool = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 2, .poolSizeCount = 1, .pPoolSizes = &size,
	};
	check(vkCreateDescriptorPool(context.device, &pool, nullptr, &descriptor_pool), "Create descriptor pool");
	const VkDescriptorSetLayout layouts[] = {descriptor_layout, descriptor_layout};
	const VkDescriptorSetAllocateInfo allocate = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = descriptor_pool, .descriptorSetCount = 2, .pSetLayouts = layouts,
	};
	VkDescriptorSet sets[2]{};
	check(vkAllocateDescriptorSets(context.device, &allocate, sets), "Allocate object descriptor sets");
	for (size_t i = 0; i < object_resources.size(); ++i) {
		auto& resource = object_resources[i];
		resource.descriptor_set = sets[i];
		const Transform initial{};
		createBuffer(&initial, sizeof(initial), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
			resource.uniform_buffer, resource.allocation);
		VmaAllocationInfo memory{};
		vmaGetAllocationInfo(context.allocator, resource.allocation, &memory);
		resource.mapped = memory.pMappedData;
		const VkDescriptorBufferInfo buffer = {resource.uniform_buffer, 0, sizeof(Transform)};
		const VkWriteDescriptorSet write = {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = resource.descriptor_set, .dstBinding = 0, .descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &buffer,
		};
		vkUpdateDescriptorSets(context.device, 1, &write, 0, nullptr);
	}
}

void createPipeline() {
	const auto& context = graphics::internal::context;
	// настройки фигуры передаём через набор дескрипторов
	const VkPipelineLayoutCreateInfo layout = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1, .pSetLayouts = &descriptor_layout,
	};
	check(vkCreatePipelineLayout(context.device, &layout, nullptr, &pipeline_layout), "Create pipeline layout");
	VkShaderModule vertex{}, fragment{};
	try {
		vertex = loadShader("shaders/dodecahedron.vert.spv");
		fragment = loadShader("shaders/dodecahedron.frag.spv");
		const VkPipelineShaderStageCreateInfo stages[] = {
			{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			 .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vertex, .pName = "main"},
			{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			 .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fragment, .pName = "main"},
		};
		// объясняем vulkan, где внутри Vertex находятся координаты и нормаль
		const VkVertexInputBindingDescription binding = {0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
		const VkVertexInputAttributeDescription attributes[] = {
			{0,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(Vertex, position)},
			{1,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(Vertex, normal)},
		};
		const VkPipelineVertexInputStateCreateInfo input = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
			.vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &binding,
			.vertexAttributeDescriptionCount = 2, .pVertexAttributeDescriptions = attributes,
		};
		const VkPipelineInputAssemblyStateCreateInfo assembly = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
			.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
		};
		const VkPipelineViewportStateCreateInfo viewport = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
			.viewportCount = 1, .scissorCount = 1,
		};
		const VkPipelineRasterizationStateCreateInfo raster = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
			.polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
			.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f,
		};
		const VkPipelineMultisampleStateCreateInfo samples = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
			.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
		};
		// ближняя поверхность закрывает дальнюю, даже если нарисована раньше
		const VkPipelineDepthStencilStateCreateInfo depth = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
			.depthTestEnable = VK_TRUE, .depthWriteEnable = VK_TRUE,
			.depthCompareOp = VK_COMPARE_OP_LESS,
		};
		const VkPipelineColorBlendAttachmentState attachment = {
			.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
				VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
		};
		const VkPipelineColorBlendStateCreateInfo blend = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
			.attachmentCount = 1, .pAttachments = &attachment,
		};
		// размер области рисования будем задавать каждый кадр под размер окна
		const VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
		const VkPipelineDynamicStateCreateInfo dynamic = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
			.dynamicStateCount = 2, .pDynamicStates = states,
		};
		const VkGraphicsPipelineCreateInfo info = {
			.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
			.stageCount = 2, .pStages = stages, .pVertexInputState = &input,
			.pInputAssemblyState = &assembly, .pViewportState = &viewport,
			.pRasterizationState = &raster, .pMultisampleState = &samples,
			.pDepthStencilState = &depth, .pColorBlendState = &blend,
			.pDynamicState = &dynamic, .layout = pipeline_layout, .renderPass = context.render_pass,
		};
		check(vkCreateGraphicsPipelines(context.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline), "Create graphics pipeline");
	} catch (...) {
		if (vertex) vkDestroyShaderModule(context.device, vertex, nullptr);
		if (fragment) vkDestroyShaderModule(context.device, fragment, nullptr);
		throw;
	}
	// конвейер уже создан, отдельные модули шейдеров больше не нужны
	vkDestroyShaderModule(context.device, vertex, nullptr);
	vkDestroyShaderModule(context.device, fragment, nullptr);
}
}

bool initialize() { // при запуске создаём всё, что нужно для рисования
	try {
		objects[0].position = {-0.8f, -0.25f, 0};
		objects[1].position = {1.6f, 0.25f, 0};
		objects[1].rotation = {15, -30, 0};
		objects[1].figure_color = {1.0f, 0.7f, 0.35f};
		objects[1].trajectory_radius = 0.5f;
		const auto mesh = makeDodecahedron();
		createBuffer(mesh.vertices.data(), sizeof(mesh.vertices), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
			vertex_buffer, vertex_allocation);
		createBuffer(mesh.indices.data(), sizeof(mesh.indices), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
			index_buffer, index_allocation);
		createDescriptors();
		createPipeline();
		return true;
	} catch (const std::exception& error) {
		std::cerr << "Application initialization: " << error.what() << '\n';
		shutdown();
		return false;
	}
}

void shutdown() { // при закрытии освобождаем то, что создали
	auto& context = graphics::internal::context;
	// сначала ждём, пока видеокарта закончит использовать наши ресурсы
	vkQueueWaitIdle(context.graphics_queue);
	if (pipeline) vkDestroyPipeline(context.device, pipeline, nullptr);
	if (pipeline_layout) vkDestroyPipelineLayout(context.device, pipeline_layout, nullptr);
	// вместе с пулом освобождаются оба набора дескрипторов
	if (descriptor_pool) vkDestroyDescriptorPool(context.device, descriptor_pool, nullptr);
	for (auto& resource : object_resources) {
		if (resource.uniform_buffer)
			vmaDestroyBuffer(context.allocator, resource.uniform_buffer, resource.allocation);
		resource = {};
	}
	if (descriptor_layout) vkDestroyDescriptorSetLayout(context.device, descriptor_layout, nullptr);
	if (index_buffer) vmaDestroyBuffer(context.allocator, index_buffer, index_allocation);
	if (vertex_buffer) vmaDestroyBuffer(context.allocator, vertex_buffer, vertex_allocation);
	pipeline = VK_NULL_HANDLE;
	pipeline_layout = VK_NULL_HANDLE;
	descriptor_pool = VK_NULL_HANDLE;
	descriptor_layout = VK_NULL_HANDLE;
	index_buffer = vertex_buffer = VK_NULL_HANDLE;
}

void update(double time) { // каждый кадр обновляем интерфейс и анимацию
	// считаем время между кадрами, чтобы скорость не зависела от fps
	static double last_time = time;
	const double delta = time - last_time;
	last_time = time;
	ImGui::Begin("Lab 1 - Dodecahedron", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
	ImGui::TextUnformatted("Variant 10: regular dodecahedron");
	ImGui::TextUnformatted("20 vertices, 30 edges, 12 pentagonal faces");
	ImGui::TextUnformatted("36 triangles per object, 2 descriptor sets");
	ImGui::Separator();
	// выбранный пункт определяет, какую матрицу собираем в sceneMatrix
	ImGui::Combo("Projection", &projection_type, "Perspective\0Orthographic\0");
	ImGui::Combo("Object", &selected_object, "Dodecahedron 1\0Dodecahedron 2\0");
	// ссылки меняют именно выбранную фигуру, а не копии её настроек
	auto& object = objects[selected_object];
	auto& position = object.position;
	auto& rotation = object.rotation;
	auto& scale = object.scale;
	auto& animation_playing = object.animation_playing;
	auto& animation_time = object.animation_time;
	auto& animation_speed = object.animation_speed;
	auto& trajectory_radius = object.trajectory_radius;
	auto& trajectory_height = object.trajectory_height;
	auto& spin_speed = object.spin_speed;
	auto& figure_color = object.figure_color;
	auto& vertex_colors = object.vertex_colors;
	ImGui::Separator();
	ImGui::TextUnformatted("X                    Y                    Z");
	ImGui::SliderFloat3("Position", position.data(), -3.0f, 3.0f, "%.2f");
	ImGui::SliderFloat3("Rotation", rotation.data(), -180.0f, 180.0f, "%.1f deg");
	// не даём масштабу стать нулевым, иначе фигура схлопнется и нормаль нельзя будет пересчитать
	ImGui::SliderFloat3("Scale", scale.data(), 0.1f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (ImGui::Button("Reset transform")) {
		position = selected_object == 0 ? std::array<float,3>{-0.8f,-0.25f,0} : std::array<float,3>{1.6f,0.25f,0};
		rotation = selected_object == 0 ? std::array<float,3>{-17.2f,25.8f,0} : std::array<float,3>{15,-30,0};
		scale = {0.5f, 0.5f, 0.5f};
	}
	ImGui::Separator();
	ImGui::TextUnformatted("Animation");
	if (ImGui::Button(animation_playing ? "Pause" : "Play"))
		animation_playing = !animation_playing;
	ImGui::SameLine();
	if (ImGui::Button("Reset animation")) {
		animation_playing = false;
		animation_time = 0;
	}
	ImGui::SliderFloat("Speed", &animation_speed, 0.1f, 3.0f, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
	ImGui::SliderFloat("Radius", &trajectory_radius, 0.0f, 1.5f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	ImGui::SliderFloat("Height", &trajectory_height, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	ImGui::SliderFloat("Spin", &spin_speed, 0.0f, 120.0f, "%.0f deg/s", ImGuiSliderFlags_AlwaysClamp);
	ImGui::Separator();
	ImGui::ColorEdit3("Figure color", figure_color.data());
	ImGui::Checkbox("Vertex colors", &vertex_colors);
	if (ImGui::Button("Reset figure color"))
		figure_color = {1, 1, 1}; // белый множитель оставляет исходные цвета вершин
	ImGui::ColorEdit3("Background", background_color);
	ImGui::End();

	// на паузе время анимации стоит, поэтому продолжение идёт с того же места
	for (auto& animated : objects) {
		if (animated.animation_playing)
			animated.animation_time += delta * animated.animation_speed;
		const float phase = static_cast<float>(std::fmod(animated.animation_time, 6.28318530718));
		// по трём осям колеблемся с разной частотой, получается петляющая объёмная траектория
		// при сбросе все синусы равны нулю, поэтому возвращаемся к ручной позиции
		animated.animated_position = {animated.trajectory_radius * std::sin(phase),
			animated.trajectory_height * std::sin(2.0f*phase), animated.trajectory_radius * 0.5f * std::sin(3.0f*phase)};
		// угол накапливаем отдельно, чтобы изменение скорости вращения не давало скачка
		if (animated.animation_playing)
			animated.animated_angle = std::fmod(animated.animated_angle + static_cast<float>(delta * animated.animation_speed) * animated.spin_speed, 360.0f);
		if (animated.animation_time == 0)
			animated.animated_angle = 0;
	}
}

void render(const graphics::internal::FrameData& fd) { // записываем команды для текущего кадра

	const auto& context = graphics::internal::context;
	// очищаем старую запись и начинаем записывать новые команды
	vkResetCommandBuffer(fd.command_buffer, 0);
	const VkCommandBufferBeginInfo begin = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	vkBeginCommandBuffer(fd.command_buffer, &begin);
	// перед рисованием закрашиваем фон и очищаем глубину до самого дальнего значения
	const VkClearValue clear_values[] = {
		{ .color = { .float32 = {
			background_color[0], background_color[1], background_color[2], background_color[3]
		} } },
		{ .depthStencil = { 1.0f, 0 } },
	};
	const VkRenderPassBeginInfo render_pass = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = context.render_pass,
		.framebuffer = fd.framebuffer,
		.renderArea = { .extent = context.swapchain_extent },
		.clearValueCount = 2,
		.pClearValues = clear_values,
	};
	vkCmdBeginRenderPass(fd.command_buffer, &render_pass, VK_SUBPASS_CONTENTS_INLINE);
	const VkViewport viewport = {0, 0, static_cast<float>(context.swapchain_extent.width),
		static_cast<float>(context.swapchain_extent.height), 0, 1};
	const VkRect2D scissor = {.extent = context.swapchain_extent};
	vkCmdSetViewport(fd.command_buffer, 0, 1, &viewport);
	vkCmdSetScissor(fd.command_buffer, 0, 1, &scissor);
	// выбираем настройки рисования и подключаем геометрию
	vkCmdBindPipeline(fd.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	const VkDeviceSize offset = 0;
	vkCmdBindVertexBuffers(fd.command_buffer, 0, 1, &vertex_buffer, &offset);
	vkCmdBindIndexBuffer(fd.command_buffer, index_buffer, 0, VK_INDEX_TYPE_UINT16);
	// геометрия общая, но для каждого объекта подключаем свой буфер настроек
	const Matrix scene = sceneMatrix(viewport.width / viewport.height);
	for (size_t i = 0; i < objects.size(); ++i) {
		const auto& object = objects[i];
		const auto& scale = object.scale;
		const auto& figure_color = object.figure_color;
		auto& resource = object_resources[i];
		const Matrix model = modelMatrix(object);
		Transform transform{};
		transform.mvp = multiply(scene, model);
		// для поворота и растяжения нормаль считаем через обратный масштаб
		for (int column = 0; column < 3; ++column)
			for (int row = 0; row < 3; ++row)
				transform.normal_matrix[column*4 + row] = model[column*4 + row] / (scale[column]*scale[column]);
		// четвёртое число здесь — флажок цветов вершин, прозрачность фигуры не меняем
		transform.tint = {figure_color[0], figure_color[1], figure_color[2], object.vertex_colors ? 1.0f : 0.0f};
		// prepare уже дождался прошлого кадра, поэтому этот буфер можно безопасно перезаписать
		std::memcpy(resource.mapped, &transform, sizeof(transform));
		check(vmaFlushAllocation(context.allocator, resource.allocation, 0, sizeof(transform)), "Flush object uniform buffer");
		vkCmdBindDescriptorSets(fd.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
			pipeline_layout, 0, 1, &resource.descriptor_set, 0, nullptr);
		// 108 индексов = 36 треугольников, отдельный вызов для каждой фигуры
		vkCmdDrawIndexed(fd.command_buffer, 108, 1, 0, 0, 0);
	}
	vkCmdEndRenderPass(fd.command_buffer);
	vkEndCommandBuffer(fd.command_buffer);
}

}
