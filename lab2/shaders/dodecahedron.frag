#version 450

layout(location = 0) in vec3 worldPosition;
layout(location = 1) in vec3 worldNormal;
layout(location = 2) in vec3 vertexColor;
layout(location = 0) out vec4 outColor;

layout(std140, set = 0, binding = 0) uniform SceneUniforms {
    mat4 viewProjection;
    vec4 cameraPosition;
    vec4 cameraViewDirection;
    vec4 ambient;
    vec4 directionalDirection;
    vec4 directionalColor;
    uvec4 lightCounts;
} scene;

struct PointLight {
    vec4 position;
    vec4 color;
};

struct SpotLight {
    vec4 position;
    vec4 direction;
    vec4 color;
    vec4 angles;
};

layout(std430, set = 0, binding = 1) readonly buffer PointLights {
    PointLight pointLights[];
};

layout(std430, set = 0, binding = 2) readonly buffer SpotLights {
    SpotLight spotLights[];
};

layout(std140, set = 1, binding = 0) uniform ModelUniforms {
    mat4 model;
    mat3 normalMatrix;
    vec4 albedo;
    vec4 specular;
} object;

vec3 safeNormalize(vec3 value) {
    float lengthSquared = dot(value, value);
    return lengthSquared > 0.0 ? value * inversesqrt(lengthSquared) : vec3(0.0);
}

vec3 blinnPhong(vec3 normal, vec3 viewDirection, vec3 lightDirection,
                vec3 radiance, vec3 albedo) {
    float diffuse = max(dot(normal, lightDirection), 0.0);
    // Источник за поверхностью не должен давать видимый блик.
    if (diffuse <= 0.0) return vec3(0.0);
    vec3 halfwayDirection = safeNormalize(viewDirection + lightDirection);
    float specular = pow(max(dot(normal, halfwayDirection), 0.0), object.specular.w);
    return (albedo * diffuse + object.specular.rgb * specular) * radiance;
}

void main() {
    vec3 normal = safeNormalize(worldNormal);
    // У ортографической камеры все лучи параллельны, даже при её перемещении.
    vec3 viewDirection = scene.cameraViewDirection.w > 0.5
        ? scene.cameraViewDirection.xyz
        : safeNormalize(scene.cameraPosition.xyz - worldPosition);
    vec3 albedo = object.albedo.rgb * vertexColor;
    vec3 result = scene.ambient.rgb * scene.ambient.w * albedo;

    result += blinnPhong(normal, viewDirection,
                         safeNormalize(-scene.directionalDirection.xyz),
                         scene.directionalColor.rgb * scene.directionalColor.w, albedo);

    // Защита только от сингулярности при совпадении источника с фрагментом.
    const float minimumDistanceSquared = 1e-8;
    for (uint i = 0; i < scene.lightCounts.x; ++i) {
        vec3 toLight = pointLights[i].position.xyz - worldPosition;
        float distanceSquared = max(dot(toLight, toLight), minimumDistanceSquared);
        vec3 radiance = pointLights[i].color.rgb * pointLights[i].color.w / distanceSquared;
        result += blinnPhong(normal, viewDirection, safeNormalize(toLight), radiance, albedo);
    }

    for (uint i = 0; i < scene.lightCounts.y; ++i) {
        vec3 toLight = spotLights[i].position.xyz - worldPosition;
        float distanceSquared = max(dot(toLight, toLight), minimumDistanceSquared);
        vec3 lightDirection = safeNormalize(toLight);
        float cosine = dot(-lightDirection, safeNormalize(spotLights[i].direction.xyz));
        float innerCosine = spotLights[i].angles.x;
        float outerCosine = spotLights[i].angles.y;
        // Одинаковые углы задают резкий край без деления на ноль.
        float cone = innerCosine > outerCosine
            ? clamp((cosine - outerCosine) / (innerCosine - outerCosine), 0.0, 1.0)
            : (cosine >= outerCosine ? 1.0 : 0.0);
        vec3 radiance = spotLights[i].color.rgb * spotLights[i].color.w * cone / distanceSquared;
        result += blinnPhong(normal, viewDirection, lightDirection, radiance, albedo);
    }

    outColor = vec4(result, 1.0);
}
