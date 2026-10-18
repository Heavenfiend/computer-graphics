#pragma once

#include <array>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace application {

using Vec3 = std::array<float, 3>;
// Индекс элемента: column * 4 + row, как у mat4 в GLSL.
using Matrix = std::array<float, 16>;

inline Vec3 add(Vec3 a, Vec3 b) {
	return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}

inline Vec3 subtract(Vec3 a, Vec3 b) {
	return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

inline Vec3 scaleVector(Vec3 value, float factor) {
	return {value[0] * factor, value[1] * factor, value[2] * factor};
}

inline float dot(Vec3 a, Vec3 b) {
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

inline Vec3 cross(Vec3 a, Vec3 b) {
	return {a[1] * b[2] - a[2] * b[1],
		a[2] * b[0] - a[0] * b[2],
		a[0] * b[1] - a[1] * b[0]};
}

inline Vec3 normalize(Vec3 value, Vec3 fallback = {0.0f, 0.0f, -1.0f}) {
	// double в длине сохраняет работоспособность для очень малых и больших float.
	for (int attempt = 0; attempt < 2; ++attempt) {
		const double length = std::hypot(double(value[0]), double(value[1]), double(value[2]));
		if (std::isfinite(length) && length > 0.0)
			return {float(value[0] / length), float(value[1] / length), float(value[2] / length)};
		value = fallback;
	}
	return {0.0f, 0.0f, -1.0f};
}

inline float radians(float degrees) {
	constexpr float pi = 3.14159265358979323846f;
	return degrees * (pi / 180.0f);
}

inline Matrix identity() {
	return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
}

inline Matrix multiply(Matrix a, Matrix b) {
	Matrix result{};
	for (int column = 0; column < 4; ++column)
		for (int row = 0; row < 4; ++row)
			for (int inner = 0; inner < 4; ++inner)
				result[column * 4 + row] += a[inner * 4 + row] * b[column * 4 + inner];
	return result;
}

inline Matrix modelMatrix(Vec3 position, Vec3 rotationDegrees, Vec3 scale) {
	const float cx = std::cos(radians(rotationDegrees[0]));
	const float sx = std::sin(radians(rotationDegrees[0]));
	const float cy = std::cos(radians(rotationDegrees[1]));
	const float sy = std::sin(radians(rotationDegrees[1]));
	const float cz = std::cos(radians(rotationDegrees[2]));
	const float sz = std::sin(radians(rotationDegrees[2]));
	const Matrix rx{1, 0, 0, 0, 0, cx, sx, 0, 0, -sx, cx, 0, 0, 0, 0, 1};
	const Matrix ry{cy, 0, -sy, 0, 0, 1, 0, 0, sy, 0, cy, 0, 0, 0, 0, 1};
	const Matrix rz{cz, sz, 0, 0, -sz, cz, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
	// Локально: масштаб, повороты X/Y/Z, затем перенос в мировые координаты.
	Matrix result = multiply(rz, multiply(ry, rx));
	for (int column = 0; column < 3; ++column)
		for (int row = 0; row < 3; ++row)
			result[column * 4 + row] *= scale[column];
	for (int axis = 0; axis < 3; ++axis)
		result[12 + axis] = position[axis];
	return result;
}

inline std::array<float, 12> normalMatrix(Matrix model, Vec3 scale) {
	std::array<float, 12> result{};
	// Для M = T*R*S обратная транспонированная линейная часть равна R*S^-1.
	// Каждый столбец mat3 занимает vec4 в std140; четвёртые компоненты остаются 0.
	for (int column = 0; column < 3; ++column)
		for (int row = 0; row < 3; ++row)
			result[column * 4 + row] = model[column * 4 + row] / scale[column] / scale[column];
	return result;
}

struct Basis {
	Vec3 right;
	Vec3 up;
	Vec3 forward;
};

inline Basis cameraBasis(Vec3 rotationDegrees) {
	const float pitch = radians(rotationDegrees[0]);
	const float yaw = radians(rotationDegrees[1]);
	const float roll = radians(rotationDegrees[2]);
	const Vec3 right{std::cos(yaw), 0.0f, std::sin(yaw)};
	const Vec3 forward{std::sin(yaw) * std::cos(pitch), std::sin(pitch),
		-std::cos(yaw) * std::cos(pitch)};
	const Vec3 up = cross(right, forward);
	// Roll выполняется вокруг локальной +Z (противоположной forward).
	return {add(scaleVector(right, std::cos(roll)), scaleVector(up, std::sin(roll))),
		add(scaleVector(up, std::cos(roll)), scaleVector(right, -std::sin(roll))), forward};
}

inline Matrix viewMatrix(Vec3 position, Vec3 rotationDegrees) {
	const Basis basis = cameraBasis(rotationDegrees);
	return {basis.right[0], basis.up[0], -basis.forward[0], 0,
		basis.right[1], basis.up[1], -basis.forward[1], 0,
		basis.right[2], basis.up[2], -basis.forward[2], 0,
		-dot(basis.right, position), -dot(basis.up, position), dot(basis.forward, position), 1};
}

inline Matrix perspective(float fovDegrees, float aspect, float nearPlane, float farPlane) {
	if (!std::isfinite(fovDegrees) || !std::isfinite(aspect) || !std::isfinite(nearPlane)
		|| !std::isfinite(farPlane) || fovDegrees <= 0.0f || fovDegrees >= 180.0f
		|| aspect <= 0.0f || nearPlane <= 0.0f || farPlane <= nearPlane)
		throw std::invalid_argument("Invalid perspective projection parameters");
	const float focalLength = 1.0f / std::tan(radians(fovDegrees) * 0.5f);
	Matrix result{};
	result[0] = focalLength / aspect;
	result[5] = -focalLength; // Положительная высота viewport Vulkan направлена вниз.
	result[10] = farPlane / (nearPlane - farPlane);
	result[11] = -1.0f;
	result[14] = nearPlane * farPlane / (nearPlane - farPlane);
	return result;
}

inline Matrix orthographic(float halfHeight, float aspect, float nearPlane, float farPlane) {
	if (!std::isfinite(halfHeight) || !std::isfinite(aspect) || !std::isfinite(nearPlane)
		|| !std::isfinite(farPlane) || halfHeight <= 0.0f || aspect <= 0.0f
		|| nearPlane < 0.0f || farPlane <= nearPlane)
		throw std::invalid_argument("Invalid orthographic projection parameters");
	Matrix result = identity();
	result[0] = 1.0f / (halfHeight * aspect);
	result[5] = -1.0f / halfHeight;
	result[10] = 1.0f / (nearPlane - farPlane);
	result[14] = nearPlane / (nearPlane - farPlane);
	return result;
}

inline std::vector<Vec3> coneLines(Vec3 position, Vec3 direction, float outerAngleDegrees, float length) {
	if (!std::isfinite(outerAngleDegrees) || !std::isfinite(length)
		|| outerAngleDegrees < 0.0f || outerAngleDegrees >= 90.0f || length <= 0.0f)
		return {};
	const Vec3 axis = normalize(direction);
	const Vec3 center = add(position, scaleVector(axis, length));
	if (outerAngleDegrees == 0.0f)
		return {position, center};
	// Выбираем опору, которая не параллельна оси, включая вертикальное направление.
	const Vec3 reference = std::abs(axis[2]) < 0.9f ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
	const Vec3 side = normalize(cross(axis, reference));
	const Vec3 up = cross(side, axis);
	const float radius = length * std::tan(radians(outerAngleDegrees));
	constexpr int segments = 32;
	constexpr int sideCount = 4;
	std::vector<Vec3> result;
	result.reserve((segments + sideCount + 1) * 2);
	result.push_back(position);
	result.push_back(center);
	const auto pointOnRim = [&](int segment) {
		const float angle = radians(360.0f * float(segment) / float(segments));
		return add(center, add(scaleVector(side, radius * std::cos(angle)),
			scaleVector(up, radius * std::sin(angle))));
	};
	for (int segment = 0; segment < segments; ++segment) {
		const Vec3 point = pointOnRim(segment);
		result.push_back(point);
		result.push_back(pointOnRim(segment + 1));
		if (segment % (segments / sideCount) == 0) {
			result.push_back(position);
			result.push_back(point);
		}
	}
	return result;
}

} // namespace application
