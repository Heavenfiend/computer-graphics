#version 450

layout(location = 0) in vec3 position;

layout(std140, set = 0, binding = 3) uniform ShadowUniforms {
    mat4 lightViewProjection[2];
    uvec4 options;
} shadows;

layout(std140, set = 1, binding = 0) uniform ModelUniforms {
    mat4 model;
} object;

layout(push_constant) uniform ShadowPass {
    // Первые 24 байта заняты параметрами текстур основного прохода.
    layout(offset = 24) uint mapIndex;
} shadowPass;

void main() {
    gl_Position = shadows.lightViewProjection[shadowPass.mapIndex] * object.model * vec4(position, 1.0);
}
