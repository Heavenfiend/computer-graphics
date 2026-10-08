#pragma once

#include <array>
#include <cmath>
#include <cstdint>

namespace application {

struct Vertex {
	float position[3];
	float normal[3]; // направление наружу от грани, нужно для освещения
};

struct Mesh {
	// точек в фигуре 20, но для каждой грани делаем свои копии, чтобы грани не сглаживались
	std::array<Vertex, 60> vertices;
	// 12 граней по 3 треугольника, в каждом треугольнике по 3 индекса
	std::array<uint16_t, 108> indices;
};

inline Mesh makeDodecahedron() {
	// золотое сечение и обратное к нему число задают координаты правильного додекаэдра
	constexpr float phi = 1.61803398875f;
	constexpr float inv = 1.0f / phi;
	constexpr float points[20][3] = {
		{-1,-1,-1}, {-1,-1,1}, {-1,1,-1}, {-1,1,1},
		{1,-1,-1}, {1,-1,1}, {1,1,-1}, {1,1,1},
		{0,-inv,-phi}, {-inv,-phi,0}, {-phi,0,-inv},
		{0,-inv,phi}, {-inv,phi,0}, {-phi,0,inv},
		{0,inv,-phi}, {inv,-phi,0}, {phi,0,-inv},
		{0,inv,phi}, {inv,phi,0}, {phi,0,inv},
	};
	// каждая строка — номера пяти точек грани, по порядку против часовой стрелки снаружи
	constexpr uint16_t faces[12][5] = {
		{13,10,0,9,1}, {14,8,0,10,2}, {15,9,0,8,4},
		{3,13,1,11,17}, {5,11,1,9,15}, {3,12,2,10,13},
		{6,14,2,12,18}, {18,12,3,17,7}, {5,15,4,16,19},
		{6,16,4,8,14}, {17,11,5,19,7}, {19,16,6,18,7},
	};
	Mesh mesh{};
	for (uint16_t face = 0; face < 12; ++face) {
		// у этой фигуры направление из центра к центру грани совпадает с её нормалью
		float normal[3]{};
		for (const auto point : faces[face])
			for (int axis = 0; axis < 3; ++axis)
				normal[axis] += points[point][axis];
		// делим на длину, чтобы получить вектор длиной 1
		const float length = std::sqrt(normal[0]*normal[0] + normal[1]*normal[1] + normal[2]*normal[2]);
		for (uint16_t corner = 0; corner < 5; ++corner)
			for (int axis = 0; axis < 3; ++axis) {
				mesh.vertices[face*5 + corner].position[axis] = points[faces[face][corner]][axis];
				mesh.vertices[face*5 + corner].normal[axis] = normal[axis] / length;
			}
		// разбиваем пятиугольник на (0,1,2), (0,2,3) и (0,3,4), с общей вершиной 0
		for (uint16_t triangle = 0; triangle < 3; ++triangle) {
			mesh.indices[face*9 + triangle*3] = face*5;
			mesh.indices[face*9 + triangle*3 + 1] = face*5 + triangle + 1;
			mesh.indices[face*9 + triangle*3 + 2] = face*5 + triangle + 2;
		}
	}
	return mesh;
}

} // конец application
