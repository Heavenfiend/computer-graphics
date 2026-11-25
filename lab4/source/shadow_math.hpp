#pragma once

#include "scene_math.hpp"

namespace application {

inline Matrix lookAt(Vec3 eye, Vec3 target) {
    const Vec3 forward = normalize(subtract(target, eye));
    const Vec3 reference = std::abs(forward[1]) < 0.99f ? Vec3{0, 1, 0} : Vec3{0, 0, 1};
    const Vec3 right = normalize(cross(forward, reference));
    const Vec3 up = cross(right, forward);
    return {right[0], up[0], -forward[0], 0,
        right[1], up[1], -forward[1], 0,
        right[2], up[2], -forward[2], 0,
        -dot(right, eye), -dot(up, eye), dot(forward, eye), 1};
}

} // namespace application
