cmake_minimum_required(VERSION 3.20)
include(FetchContent)

project(vulkan-starter-app LANGUAGES C CXX)

# Повторно используем уже загруженные версии библиотек первой работы, если они есть.
# На другом компьютере FetchContent получит те же закреплённые версии обычным способом.
foreach(_dependency glfw vk-bootstrap vulkanmemoryallocator imgui)
	string(TOUPPER "${_dependency}" _dependency_upper)
	set(_existing "${CMAKE_CURRENT_SOURCE_DIR}/../lab1/build-debug/_deps/${_dependency}-src")
	if(EXISTS "${_existing}/CMakeLists.txt" OR (_dependency STREQUAL "imgui" AND EXISTS "${_existing}/imgui.h"))
		set(FETCHCONTENT_SOURCE_DIR_${_dependency_upper} "${_existing}" CACHE PATH "Existing dependency checkout")
	endif()
endforeach()

set(LAB3_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../lab3")
add_executable(${PROJECT_NAME}
	source/main.cpp source/graphics_internal.cpp source/application.cpp source/shadow_maps.cpp
	"${LAB3_DIR}/source/texture_resources.cpp")
target_include_directories(${PROJECT_NAME} PRIVATE "${LAB3_DIR}/source" "${LAB3_DIR}/third_party")

if(LINUX)
	target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
elseif(APPLE)
	target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
elseif(WIN32)
	if(MSVC)
		target_compile_options(${PROJECT_NAME} PRIVATE /W4 /wd4201)
		target_link_options(${PROJECT_NAME} PRIVATE /subsystem:windows /entry:mainCRTStartup)
	else()
		target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
		target_link_options(${PROJECT_NAME} PRIVATE -mwindows)
	endif()
	target_compile_definitions(${PROJECT_NAME} PRIVATE -D_USE_MATH_DEFINES)
else()
	message(FATAL_ERROR "${CMAKE_SYSTEM_NAME} is not supported")
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES CXX_STANDARD_REQUIRED TRUE CXX_STANDARD 20)

FetchContent_Declare(
	glfw
	GIT_REPOSITORY https://github.com/glfw/glfw.git
	GIT_TAG 3.5.1
)

FetchContent_Declare(
	vk-bootstrap
	GIT_REPOSITORY https://github.com/charles-lunarg/vk-bootstrap.git
	GIT_TAG v1.4.321
)

FetchContent_Declare(
	VulkanMemoryAllocator
	GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
	GIT_TAG v3.4.0
)

FetchContent_Declare(
	imgui
	GIT_REPOSITORY https://github.com/ocornut/imgui.git
	GIT_TAG v1.92.9
)

# ищем sdk и тогда, когда ide не получила переменную VULKAN_SDK
include("${CMAKE_CURRENT_SOURCE_DIR}/VulkanSDK.local.cmake" OPTIONAL)
set(_VULKAN_SDK_HINTS "$ENV{VULKAN_SDK}")
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
	list(APPEND _VULKAN_SDK_HINTS "$ENV{LOCALAPPDATA}/VulkanSDK/1.4.321.1")
	find_path(Vulkan_INCLUDE_DIR NAMES vulkan/vulkan.h
		HINTS ${_VULKAN_SDK_HINTS} PATH_SUFFIXES Include)
	find_library(Vulkan_LIBRARY NAMES vulkan-1
		HINTS ${_VULKAN_SDK_HINTS} PATH_SUFFIXES Lib)
endif()
find_package(Vulkan REQUIRED)

set(GLFW_LIBRARY_TYPE STATIC)
set(GLFW_BUILD_EXAMPLES OFF)
set(GLFW_BUILD_TESTS OFF)
set(GLFW_BUILD_DOCS OFF)

FetchContent_MakeAvailable(
	glfw
	vk-bootstrap
	VulkanMemoryAllocator
	imgui
)

target_link_libraries(${PROJECT_NAME} PRIVATE
	glfw
	Vulkan::Vulkan
	vk-bootstrap::vk-bootstrap
	GPUOpen::VulkanMemoryAllocator
)

target_include_directories(${PROJECT_NAME} PRIVATE ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_sources(${PROJECT_NAME} PRIVATE
	${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
	${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
	${imgui_SOURCE_DIR}/imgui.cpp
	${imgui_SOURCE_DIR}/imgui_draw.cpp
	${imgui_SOURCE_DIR}/imgui_tables.cpp
	${imgui_SOURCE_DIR}/imgui_widgets.cpp
	${imgui_SOURCE_DIR}/imgui_demo.cpp
)

# собираем шейдеры в .spv
find_program(GLSLC_FOUND glslc HINTS ${_VULKAN_SDK_HINTS} PATH_SUFFIXES Bin)
if(GLSLC_FOUND)
	set(_SHADER_BINARIES)

	macro(compile_shader SHADER_SOURCE_PATH)
		set(SHADER_SOURCE "${SHADER_SOURCE_PATH}")
		get_filename_component(SHADER_FILE "${SHADER_SOURCE}" NAME)
		set(SHADER_BINARY ${SHADER_FILE}.spv)
		set(SHADER_BINARY_PATH ${CMAKE_CURRENT_BINARY_DIR}/shaders/${SHADER_BINARY})

		add_custom_command(
			OUTPUT ${SHADER_BINARY_PATH}
			COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/shaders
			COMMAND ${GLSLC_FOUND} ${SHADER_SOURCE} -o ${SHADER_BINARY_PATH}
			DEPENDS ${SHADER_SOURCE}
			COMMENT "Compiling ${SHADER_FILE} shader"
			VERBATIM
		)

		list(APPEND _SHADER_BINARIES ${SHADER_BINARY_PATH})
	endmacro()

	compile_shader("${LAB3_DIR}/shaders/dodecahedron.vert")
	compile_shader("${CMAKE_CURRENT_SOURCE_DIR}/shaders/dodecahedron.frag")
	compile_shader("${LAB3_DIR}/shaders/marker.vert")
	compile_shader("${LAB3_DIR}/shaders/marker.frag")
	compile_shader("${CMAKE_CURRENT_SOURCE_DIR}/shaders/shadow.vert")

	add_custom_target(shaders DEPENDS ${_SHADER_BINARIES})
	add_dependencies(${PROJECT_NAME} shaders)
else()
	message(FATAL_ERROR "glslc was not found. Check the Vulkan SDK installation.")
endif()

# Самодостаточная папка запуска: никаких абсолютных путей к ресурсам в бинарнике.
add_custom_target(runtime-resources ALL
	COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_BINARY_DIR}/shaders $<TARGET_FILE_DIR:${PROJECT_NAME}>/shaders
	COMMAND ${CMAKE_COMMAND} -E copy_directory "${LAB3_DIR}/resources/textures" $<TARGET_FILE_DIR:${PROJECT_NAME}>/resources/textures
	VERBATIM
)
add_dependencies(runtime-resources shaders)
add_dependencies(${PROJECT_NAME} runtime-resources)

include(CTest)
if(BUILD_TESTING)
	add_executable(lab4-shadow-math-tests tests/shadow_math_tests.cpp)
	target_compile_features(lab4-shadow-math-tests PRIVATE cxx_std_20)
	target_include_directories(lab4-shadow-math-tests PRIVATE source "${LAB3_DIR}/source")
	add_test(NAME lab4-shadow-math COMMAND lab4-shadow-math-tests)
endif()
