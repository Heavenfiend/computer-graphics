#version 450

layout(location = 0) in vec3 position;

layout(push_constant) uniform MarkerPush {
    mat4 mvp;
    vec4 color;
} marker;

void main() {
    gl_Position = marker.mvp * vec4(position, 1.0);
}
