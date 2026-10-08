#version 450

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 0) out float brightness;
layout(location = 1) out vec3 vertexColor;

layout(set = 0, binding = 0, std140) uniform Transform {
    // для каждой фигуры подключаем свой набор дескрипторов со своим буфером
    mat4 mvp;
    mat3 normalMatrix;
    vec4 tint;
} transform;

void main() {
    // переводим координаты вершины из фигуры в пространство отсечения
    gl_Position = transform.mvp * vec4(position, 1.0);
    // чем сильнее грань повёрнута к свету, тем она ярче; немного света оставляем и в тени
    vec3 light = normalize(vec3(-0.5, 0.8, 1.0));
    // при повороте и растяжении пересчитываем нормаль, чтобы свет оставался на своём месте
    vec3 worldNormal = normalize(transform.normalMatrix * normal);
    brightness = 0.25 + 0.75 * max(dot(worldNormal, light), 0.0);
    // координаты от -phi до phi переводим в цвета от 0 до 1
    // берём координаты внутри фигуры, поэтому цвета при движении остаются на своих местах
    vec3 proceduralColor = clamp(position / (2.0 * 1.61803398875) + 0.5, 0.0, 1.0);
    vec3 baseColor = transform.tint.w > 0.5 ? proceduralColor : vec3(1.0);
    vertexColor = baseColor * transform.tint.rgb;
}
