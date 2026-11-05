#include "dodecahedron.hpp"
#include "scene_math.hpp"
#include "texture_math.hpp"

#include <algorithm>
#include <array>
#include <cmath>
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

bool near(float a, float b, float tolerance = 0.0001f) {
	return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= tolerance;
}

Vec3 position(const Vertex& vertex) {
	return {vertex.position[0], vertex.position[1], vertex.position[2]};
}

float uvAreaTwice(const Vertex& a, const Vertex& b, const Vertex& c) {
	return (b.uv[0] - a.uv[0]) * (c.uv[1] - a.uv[1])
		- (b.uv[1] - a.uv[1]) * (c.uv[0] - a.uv[0]);
}

void testFaceUv() {
	const Mesh mesh = makeDodecahedron();
	for (const Vertex& vertex : mesh.vertices)
		for (float coordinate : vertex.uv)
			check(std::isfinite(coordinate) && coordinate >= -0.00001f && coordinate <= 1.00001f,
				"UV coordinates are finite and lie in [0, 1]");

	for (std::size_t face = 0; face < 12; ++face) {
		const std::size_t first = face * 5;
		const Vertex& a = mesh.vertices[first];
		const Vertex& b = mesh.vertices[first + 1];
		const Vertex& c = mesh.vertices[first + 2];
		const Vec3 edge1 = subtract(position(b), position(a));
		const Vec3 edge2 = subtract(position(c), position(a));
		const float d11 = dot(edge1, edge1);
		const float d12 = dot(edge1, edge2);
		const float d22 = dot(edge2, edge2);
		const float determinant = d11 * d22 - d12 * d12;
		check(determinant > 0.0001f, "face provides an independent geometric basis");
		if (determinant <= 0.0001f)
			continue;

		// Recover the affine map from three vertices independently of the UV generator.
		// It must predict the remaining vertices, hence every triangle and shared edge.
		for (std::size_t corner = 0; corner < 5; ++corner) {
			const Vertex& vertex = mesh.vertices[first + corner];
			const Vec3 delta = subtract(position(vertex), position(a));
			const float alpha = (dot(delta, edge1) * d22 - dot(delta, edge2) * d12) / determinant;
			const float beta = (dot(delta, edge2) * d11 - dot(delta, edge1) * d12) / determinant;
			for (int axis = 0; axis < 2; ++axis) {
				const float predicted = a.uv[axis] + alpha * (b.uv[axis] - a.uv[axis])
					+ beta * (c.uv[axis] - a.uv[axis]);
				check(near(predicted, vertex.uv[axis]), "one affine UV map covers the entire face");
			}
		}

		float polygonArea = 0;
		std::array<float, 2> average{};
		float triangleArea = 0;
		for (std::size_t corner = 0; corner < 5; ++corner) {
			const Vertex& vertex = mesh.vertices[first + corner];
			const Vertex& next = mesh.vertices[first + (corner + 1) % 5];
			polygonArea += vertex.uv[0] * next.uv[1] - vertex.uv[1] * next.uv[0];
			for (int axis = 0; axis < 2; ++axis)
				average[axis] += vertex.uv[axis] / 5.0f;
		}
		check(near(average[0], 0.5f) && near(average[1], 0.5f), "face UV map is centered at (0.5, 0.5)");
		check(polygonArea > 0.0001f, "UV face is nondegenerate and preserves outward winding");
		for (std::size_t triangle = 0; triangle < 3; ++triangle) {
			const std::size_t offset = face * 9 + triangle * 3;
			const auto i0 = mesh.indices[offset];
			const auto i1 = mesh.indices[offset + 1];
			const auto i2 = mesh.indices[offset + 2];
			const bool inFace = i0 >= first && i0 < first + 5 && i1 >= first && i1 < first + 5
				&& i2 >= first && i2 < first + 5;
			check(inFace, "triangles only reference their own face UV chart");
			if (!inFace)
				continue;
			const float area = uvAreaTwice(mesh.vertices[i0], mesh.vertices[i1], mesh.vertices[i2]);
			check(area > 0.0001f, "every UV triangle has nonzero area and consistent winding");
			triangleArea += area;
		}
		check(near(triangleArea, polygonArea), "UV triangles cover the pentagon without gaps or excess area");
	}
}

template<class Exception, class Action>
void checkThrows(Action action, const std::string& message) {
	bool caught = false;
	try {
		action();
	} catch (const Exception&) {
		caught = true;
	} catch (...) {
	}
	check(caught, message);
}

void testUniformAlignment() {
	for (const auto [alignment, expected] : std::array<std::array<std::size_t, 2>, 4>{{
		{1, 144}, {16, 144}, {64, 192}, {256, 256}}}) {
		const std::size_t stride = alignedUniformStride(144, alignment);
		check(stride == expected, "144-byte uniform block has the expected aligned stride");
		for (std::size_t model = 0; model < 1024; ++model) {
			const std::size_t offset = model * stride;
			check(offset % alignment == 0, "every dynamic model offset meets device alignment");
			check(offset + 144 <= (model + 1) * stride, "uniform payload fits its slot without overlap");
		}
	}
	check(alignedUniformStride(256, 256) == 256, "already aligned size is unchanged");
	check(alignedUniformStride(1, 256) == 256, "small nonzero block receives a full aligned slot");
	checkThrows<std::invalid_argument>([] { alignedUniformStride(144, 0); }, "zero alignment is rejected");
	checkThrows<std::invalid_argument>([] { alignedUniformStride(144, 3); }, "non-power-of-two alignment is rejected");
	checkThrows<std::invalid_argument>([] { alignedUniformStride(0, 16); }, "empty uniform block is rejected");
	const auto maximum = std::numeric_limits<std::size_t>::max();
	check(alignedUniformStride(maximum, 1) == maximum, "alignment one does not overflow at maximum size");
	checkThrows<std::overflow_error>([maximum] { alignedUniformStride(maximum, 16); },
		"alignment overflow is rejected instead of wrapping");
	check(alignedUniformStride(maximum - 15, 16) == maximum - 15,
		"largest aligned block remains valid");
}

} // namespace

int main() {
	testFaceUv();
	testUniformAlignment();
	if (failures != 0) {
		std::cerr << failures << " texture check(s) failed\n";
		return 1;
	}
	std::cout << "Texture UV continuity, coverage and dynamic uniform alignment checks passed\n";
	return 0;
}
