#version 450

layout(location = 0) out vec4 outColor;

layout(push_constant) uniform MarkerPush {
    mat4 mvp;
    vec4 color;
} marker;

void main() {
    // Маркеры и линии конуса имеют собственный постоянный цвет.
    outColor = marker.color;
}
