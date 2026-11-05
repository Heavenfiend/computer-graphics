#version 450

layout(location = 0) out vec4 outColor;

layout(location = 0) in vec4 markerColor;

void main() {
    // Маркеры и линии конуса имеют собственный постоянный цвет.
    outColor = markerColor;
}
