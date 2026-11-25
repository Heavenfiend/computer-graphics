#include "shadow_math.hpp"

#include <cmath>
#include <iostream>

namespace {

using namespace application;

std::array<float, 4> transform(const Matrix& matrix, Vec3 p) {
    std::array<float, 4> result{};
    for (int row = 0; row < 4; ++row)
        result[row] = matrix[row] * p[0] + matrix[4 + row] * p[1] + matrix[8 + row] * p[2] + matrix[12 + row];
    return result;
}

bool near(float left, float right) {
    return std::abs(left - right) < 1e-5f;
}

} // namespace

int main() {
    int failures = 0;
    const auto expect = [&](bool condition, const char* description) {
        if (!condition) {
            std::cerr << description << '\n';
            ++failures;
        }
    };
    const Vec3 eye{2, 5, 3};
    const Vec3 target{2, 0, 3};
    const Matrix view = lookAt(eye, target);
    const auto origin = transform(view, eye);
    expect(near(origin[0], 0) && near(origin[1], 0) && near(origin[2], 0), "LookAt maps the light position to the origin");
    const auto center = transform(view, target);
    expect(near(center[0], 0) && near(center[1], 0) && near(center[2], -5), "LookAt supports a vertical light direction");

    const Matrix sun = multiply(orthographic(6, 1, 0, 10), view);
    const auto sun_near = transform(sun, eye);
    const auto sun_far = transform(sun, {2, -5, 3});
    expect(near(sun_near[2], 0) && near(sun_far[2], 1), "Sun maps near and far to Vulkan depth [0, 1]");
    const auto sun_edge = transform(sun, {8, 0, 3});
    expect(near(sun_edge[0], -1) && near(sun_edge[3], 1), "Sun coverage is twice its orthographic half extent");

    const Matrix spot = multiply(perspective(60, 1, 0.05f, 10), view);
    const float cone_radius = 5 * std::tan(radians(30));
    const auto spot_edge = transform(spot, {2, 0, 3 + cone_radius});
    expect(near(spot_edge[1] / spot_edge[3], -1), "Outer half angle matches the vertical edge of the spot map");
    const auto behind = transform(spot, {2, 6, 3});
    expect(behind[3] < 0, "Points behind the spot are distinguishable after projection");
    return failures == 0 ? 0 : 1;
}
