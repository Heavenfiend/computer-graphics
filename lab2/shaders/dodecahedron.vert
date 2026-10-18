#version 450

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;

layout(location = 0) out vec3 worldPosition;
layout(location = 1) out vec3 worldNormal;
layout(location = 2) out vec3 vertexColor;

layout(std140, set = 0, binding = 0) uniform SceneUniforms {
    mat4 viewProjection;
    vec4 cameraPosition;
    vec4 cameraViewDirection;
    vec4 ambient;
    vec4 directionalDirection;
    vec4 directionalColor;
    uvec4 lightCounts;
} scene;

layout(std140, set = 1, binding = 0) uniform ModelUniforms {
    mat4 model;
    mat3 normalMatrix;
    vec4 albedo;
    vec4 specular;
} object;

void main() {
    vec4 world = object.model * vec4(position, 1.0);
    worldPosition = world.xyz;
    // Обратная транспонированная матрица на CPU учитывает неравномерный масштаб.
    worldNormal = object.normalMatrix * normal;
    gl_Position = scene.viewProjection * world;

    // Сохраняем окраску по координатам из первой лабораторной.
    const float goldenRatio = 1.61803398875;
    vec3 proceduralColor = clamp(position / (2.0 * goldenRatio) + 0.5, 0.0, 1.0);
    vertexColor = object.albedo.w > 0.5 ? proceduralColor : vec3(1.0);
}
