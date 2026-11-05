#include "dodecahedron.hpp"
#include "scene_math.hpp"

#include <iostream>
#include <limits>
#include <string>

namespace {

using namespace application;

int failures = 0;

void check(bool condition, const std::string& description) {
	if (!condition) {
		std::cerr << "FAIL: " << description << '\n';
		++failures;
	}
}

bool near(float actual, float expected, float tolerance = 0.0001f) {
	return std::isfinite(actual) && std::abs(actual - expected) <= tolerance;
}

bool near(Vec3 actual, Vec3 expected, float tolerance = 0.0001f) {
	return near(actual[0], expected[0], tolerance) && near(actual[1], expected[1], tolerance)
		&& near(actual[2], expected[2], tolerance);
}

std::array<float, 4> transform(Matrix matrix, Vec3 point, float w = 1.0f) {
	std::array<float, 4> result{};
	for (int row = 0; row < 4; ++row)
		result[row] = matrix[row] * point[0] + matrix[4 + row] * point[1]
			+ matrix[8 + row] * point[2] + matrix[12 + row] * w;
	return result;
}

Vec3 xyz(std::array<float, 4> value) {
	return {value[0], value[1], value[2]};
}

Vec3 projected(Matrix matrix, Vec3 point) {
	const auto clip = transform(matrix, point);
	return {clip[0] / clip[3], clip[1] / clip[3], clip[2] / clip[3]};
}

template<typename Action>
void checkInvalid(Action action, const std::string& description) {
	bool rejected = false;
	try {
		action();
	} catch (const std::invalid_argument&) {
		rejected = true;
	}
	check(rejected, description);
}

void testVectorsAndCamera() {
	check(near(normalize({0, 0, 0}), {0, 0, -1}), "zero direction uses default fallback");
	check(near(normalize({0, 0, 0}, {0, 4, 0}), {0, 1, 0}), "fallback is normalized");
	check(near(normalize({0, 0, 0}, {0, 0, 0}), {0, 0, -1}), "zero fallback remains safe");
	check(near(normalize({std::numeric_limits<float>::infinity(), 0, 0}), {0, 0, -1}),
		"nonfinite direction remains safe");
	check(near(normalize({1.0e-30f, 0, 0}), {1, 0, 0}), "small finite direction can be normalized");
	check(near(normalize({1.0e30f, 0, 0}), {1, 0, 0}), "large finite direction can be normalized");
	check(near(cameraBasis({0, 0, 0}).forward, {0, 0, -1}), "default camera looks toward -Z");
	check(near(cameraBasis({0, 90, 0}).forward, {1, 0, 0}), "positive yaw looks toward +X");
	check(near(cameraBasis({90, 0, 0}).forward, {0, 1, 0}), "positive pitch looks upward");
	check(near(cameraBasis({0, 0, 90}).right, {0, 1, 0}), "roll rotates right toward local up");
	check(near(cameraBasis({0, 0, 90}).up, {-1, 0, 0}), "roll rotates up consistently");

	const Vec3 position{4.3f, -2.1f, 8.0f};
	for (const Vec3 angles : {Vec3{0, 0, 0}, Vec3{23, -57, 41}, Vec3{90, 70, -31}, Vec3{-90, -90, 90}}) {
		const Basis basis = cameraBasis(angles);
		check(near(dot(basis.right, basis.right), 1) && near(dot(basis.up, basis.up), 1)
			&& near(dot(basis.forward, basis.forward), 1), "camera basis vectors have unit length");
		check(near(dot(basis.right, basis.up), 0) && near(dot(basis.up, basis.forward), 0)
			&& near(dot(basis.forward, basis.right), 0), "camera basis vectors are perpendicular");
		check(near(cross(basis.right, basis.up), scaleVector(basis.forward, -1)),
			"camera basis is right-handed");
		const Matrix view = viewMatrix(position, angles);
		check(near(xyz(transform(view, position)), {0, 0, 0}), "camera position maps to view origin");
		check(near(xyz(transform(view, add(position, scaleVector(basis.forward, 3)))), {0, 0, -3}),
			"point ahead of camera maps to negative view Z");
		check(near(xyz(transform(view, add(position, basis.right))), {1, 0, 0}),
			"camera right maps to positive view X");
		check(near(xyz(transform(view, add(position, basis.up))), {0, 1, 0}),
			"camera up maps to positive view Y");
	}
}

void testProjections() {
	constexpr float nearPlane = 0.3f;
	constexpr float farPlane = 75.0f;
	constexpr float fov = 64.0f;
	for (const float aspect : {0.6f, 1.0f, 2.4f}) {
		const Matrix projection = perspective(fov, aspect, nearPlane, farPlane);
		check(near(projected(projection, {0, 0, -nearPlane})[2], 0), "perspective near maps to Vulkan depth 0");
		check(near(projected(projection, {0, 0, -farPlane})[2], 1), "perspective far maps to Vulkan depth 1");
		const float halfHeight = nearPlane * std::tan(radians(fov) * 0.5f);
		check(near(projected(projection, {halfHeight * aspect, 0, -nearPlane})[0], 1),
			"perspective horizontal field depends on actual aspect");
		check(near(projected(projection, {0, halfHeight, -nearPlane})[1], -1),
			"perspective maps positive Y upward in Vulkan viewport");
		const float closeX = projected(projection, {0.2f, 0, -1})[0];
		check(near(projected(projection, {0.2f, 0, -2})[0], closeX * 0.5f),
			"perspective size decreases with distance");

		const Matrix ortho = orthographic(3.0f, aspect, nearPlane, farPlane);
		check(near(projected(ortho, {0, 0, -nearPlane})[2], 0), "orthographic near maps to Vulkan depth 0");
		check(near(projected(ortho, {0, 0, -farPlane})[2], 1), "orthographic far maps to Vulkan depth 1");
		check(near(projected(ortho, {3.0f * aspect, 0, -nearPlane})[0], 1),
			"orthographic horizontal extent depends on actual aspect");
		check(near(projected(ortho, {0, 3.0f, -nearPlane})[1], -1), "orthographic flips Y for Vulkan");
		check(near(projected(ortho, {1, 0, -1})[0], projected(ortho, {1, 0, -10})[0]),
			"orthographic size is independent of distance");
	}
	const Matrix projection = perspective(fov, 1.5f, nearPlane, farPlane);
	const Matrix view = viewMatrix({2, 3, 8}, {17, 45, 13});
	const Vec3 point{3, 5, -4};
	check(near(projected(multiply(projection, view), point), projected(projection, xyz(transform(view, point)))),
		"column-major multiplication agrees with sequential view and projection");
	checkInvalid([] { perspective(0, 1, 0.1f, 10); }, "zero FOV is rejected");
	checkInvalid([] { perspective(180, 1, 0.1f, 10); }, "180 degree FOV is rejected");
	checkInvalid([] { perspective(60, 0, 0.1f, 10); }, "zero aspect is rejected");
	checkInvalid([] { perspective(60, 1, 1, 1); }, "coincident clipping planes are rejected");
	checkInvalid([] { perspective(60, 1, 0, 10); }, "perspective near plane must be positive");
	checkInvalid([] { orthographic(0, 1, 0.1f, 10); }, "zero orthographic extent is rejected");
	checkInvalid([] { orthographic(1, -1, 0.1f, 10); }, "negative orthographic aspect is rejected");
}

void testModelsAndNormals() {
	const Vec3 position{5, -2, 7};
	const Vec3 scale{2, 0.75f, 3};
	const Matrix model = modelMatrix(position, {24, 63, -38}, scale);
	check(near(xyz(transform(model, {0, 0, 0})), position), "model origin maps to object position");
	for (int axis = 0; axis < 3; ++axis) {
		Vec3 unit{};
		unit[axis] = 1;
		const Vec3 transformed = xyz(transform(model, unit, 0));
		check(near(std::sqrt(dot(transformed, transformed)), scale[axis]), "model preserves requested axis scale");
	}
	check(near(xyz(transform(modelMatrix({0, 0, 0}, {0, 0, 90}, {1, 1, 1}), {1, 0, 0})), {0, 1, 0}),
		"positive object rotation uses right-hand rule");
	const auto normals = normalMatrix(model, scale);
	const Vec3 sourceNormal = normalize({1, 2, 3});
	const Vec3 firstTangent = normalize(cross(sourceNormal, {0, 1, 0}));
	const Vec3 secondTangent = cross(sourceNormal, firstTangent);
	const Vec3 transformedNormal{
		normals[0] * sourceNormal[0] + normals[4] * sourceNormal[1] + normals[8] * sourceNormal[2],
		normals[1] * sourceNormal[0] + normals[5] * sourceNormal[1] + normals[9] * sourceNormal[2],
		normals[2] * sourceNormal[0] + normals[6] * sourceNormal[1] + normals[10] * sourceNormal[2]};
	const Vec3 firstWorldTangent = xyz(transform(model, firstTangent, 0));
	const Vec3 secondWorldTangent = xyz(transform(model, secondTangent, 0));
	check(near(dot(transformedNormal, firstWorldTangent), 0)
		&& near(dot(transformedNormal, secondWorldTangent), 0),
		"inverse transpose normal remains perpendicular under nonuniform scale");
	check(near(normalize(transformedNormal), normalize(cross(firstWorldTangent, secondWorldTangent))),
		"normal matrix agrees with independently transformed surface geometry");
	check(near(normals[3], 0) && near(normals[7], 0) && near(normals[11], 0),
		"normal matrix has explicit zero std140 padding");
	const auto pureScaleNormals = normalMatrix(modelMatrix({0, 0, 0}, {0, 0, 0}, scale), scale);
	check(near(pureScaleNormals[0], 1 / scale[0]) && near(pureScaleNormals[5], 1 / scale[1])
		&& near(pureScaleNormals[10], 1 / scale[2]), "normal scale is reciprocal object scale");
}

void testDodecahedron() {
	const Mesh mesh = makeDodecahedron();
	check(mesh.vertices.size() == 60 && mesh.indices.size() == 108, "dodecahedron has 12 flat pentagonal faces and 36 triangles");
	for (const Vertex& vertex : mesh.vertices) {
		const Vec3 position{vertex.position[0], vertex.position[1], vertex.position[2]};
		const Vec3 normal{vertex.normal[0], vertex.normal[1], vertex.normal[2]};
		check(near(dot(normal, normal), 1), "mesh normal has unit length");
		check(dot(position, normal) > 0, "mesh normal points outward");
	}
	for (std::size_t triangle = 0; triangle < mesh.indices.size(); triangle += 3) {
		const auto i0 = mesh.indices[triangle];
		const auto i1 = mesh.indices[triangle + 1];
		const auto i2 = mesh.indices[triangle + 2];
		const bool valid = i0 < mesh.vertices.size() && i1 < mesh.vertices.size() && i2 < mesh.vertices.size();
		check(valid, "mesh indices stay inside vertex array");
		if (!valid)
			continue;
		const Vertex& a = mesh.vertices[i0];
		const Vertex& b = mesh.vertices[i1];
		const Vertex& c = mesh.vertices[i2];
		const Vec3 p0{a.position[0], a.position[1], a.position[2]};
		const Vec3 p1{b.position[0], b.position[1], b.position[2]};
		const Vec3 p2{c.position[0], c.position[1], c.position[2]};
		const Vec3 n0{a.normal[0], a.normal[1], a.normal[2]};
		const Vec3 n1{b.normal[0], b.normal[1], b.normal[2]};
		const Vec3 n2{c.normal[0], c.normal[1], c.normal[2]};
		check(near(n0, n1) && near(n0, n2), "each triangle belongs to a flat face");
		const Vec3 geometricNormal = cross(subtract(p1, p0), subtract(p2, p0));
		check(dot(geometricNormal, n0) > 0.0001f, "triangle winding and nonzero area agree with outward normal");
	}
}

void testCone() {
	const Vec3 apex{3, 2, -4};
	constexpr float length = 3.5f;
	constexpr float halfAngle = 27.0f;
	for (const Vec3 direction : {Vec3{0, -1, 0}, Vec3{0, 1, 0}, Vec3{0, 0, -1}, Vec3{2, -3, 4}, Vec3{0, 0, 0}}) {
		const Vec3 axis = normalize(direction);
		const std::vector<Vec3> lines = coneLines(apex, direction, halfAngle, length);
		check(lines.size() == 74, "cone has closed 32-segment rim, four sides and central axis");
		check(lines.size() >= 2 && near(lines[0], apex)
			&& near(lines[1], add(apex, scaleVector(axis, length))),
			"central axis starts at light and follows spotlight direction");
		std::size_t apexCount = 0;
		for (std::size_t index = 2; index < lines.size(); ++index) {
			const Vec3 point = lines[index];
			if (near(point, apex)) {
				++apexCount;
				continue;
			}
			const Vec3 fromApex = subtract(point, apex);
			const float axialDistance = dot(fromApex, axis);
			const Vec3 radial = subtract(fromApex, scaleVector(axis, axialDistance));
			check(near(axialDistance, length), "cone rim lies in base plane perpendicular to direction");
			check(near(std::sqrt(dot(radial, radial)), length * std::tan(radians(halfAngle))),
				"cone radius follows outer half-angle");
			check(near(dot(normalize(fromApex), axis), std::cos(radians(halfAngle))),
				"cone side has requested angle to spotlight direction");
		}
		check(apexCount == 4, "four cone sides start at light position");
	}
	const auto zeroAngleLines = coneLines(apex, {0, -1, 0}, 0, length);
	check(zeroAngleLines.size() == 2 && near(zeroAngleLines[0], apex)
		&& near(zeroAngleLines[1], add(apex, {0, -length, 0})),
		"zero cone angle retains its visible direction axis");
	check(coneLines(apex, {0, -1, 0}, -1, length).empty(), "negative cone angle is skipped");
	check(coneLines(apex, {0, -1, 0}, 90, length).empty(), "unbounded cone angle is skipped");
	check(coneLines(apex, {0, -1, 0}, halfAngle, 0).empty(), "zero cone length is skipped");
}

} // namespace

int main() {
	testVectorsAndCamera();
	testProjections();
	testModelsAndNormals();
	testDodecahedron();
	testCone();
	if (failures != 0) {
		std::cerr << failures << " check(s) failed\n";
		return 1;
	}
	std::cout << "Camera, projections, normal transforms, mesh and cone checks passed\n";
	return 0;
}
