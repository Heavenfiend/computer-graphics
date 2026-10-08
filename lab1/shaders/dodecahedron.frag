#version 450

layout(location = 0) in float brightness;
layout(location = 1) in vec3 vertexColor;
layout(location = 0) out vec4 outColor;

void main() {
    // между вершинами цвет плавно смешивается; освещение делает грани различимыми
    outColor = vec4(vertexColor * brightness, 1.0);
}
