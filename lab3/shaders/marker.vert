#version 450

layout(location = 0) in vec3 position;

layout(location = 0) out vec4 markerColor;
layout(std140, set = 0, binding = 0) uniform SceneUniforms {
    mat4 viewProjection;
} scene;
layout(std140, set = 1, binding = 0) uniform ModelUniforms {
    mat4 model;
    mat3 normalMatrix;
    vec4 albedo;
    vec4 specular;
} object;

void main() {
    gl_Position = scene.viewProjection * object.model * vec4(position, 1.0);
    markerColor = object.albedo;
}
