#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace armature_probe
{
using affine_matrix = std::array<float, 16>;
using t48_matrix = std::array<float, 12>;

inline affine_matrix decode_file64(const void *bytes)
{
	affine_matrix matrix {};
	std::memcpy(matrix.data(), bytes, sizeof(matrix));
	return matrix;
}

inline void encode_file64(const affine_matrix &matrix, void *bytes)
{
	std::memcpy(bytes, matrix.data(), sizeof(matrix));
}

inline affine_matrix decode_t48(const void *bytes)
{
	t48_matrix packed {};
	std::memcpy(packed.data(), bytes, sizeof(packed));
	return {
		packed[0], packed[4], packed[8], 0.0f,
		packed[1], packed[5], packed[9], 0.0f,
		packed[2], packed[6], packed[10], 0.0f,
		packed[3], packed[7], packed[11], 1.0f
	};
}

inline void encode_t48(const affine_matrix &matrix, void *bytes)
{
	const t48_matrix packed {
		matrix[0], matrix[4], matrix[8], matrix[12],
		matrix[1], matrix[5], matrix[9], matrix[13],
		matrix[2], matrix[6], matrix[10], matrix[14]
	};
	std::memcpy(bytes, packed.data(), sizeof(packed));
}

inline void file64_to_t48(const void *file64, void *t48)
{
	encode_t48(decode_file64(file64), t48);
}

inline void translate_world_t48(const void *source, float x, float y, float z, void *destination)
{
	auto matrix = decode_t48(source);
	for (size_t column = 0; column < 3; ++column)
		matrix[12 + column] += x * matrix[column] + y * matrix[4 + column] +
			z * matrix[8 + column];
	encode_t48(matrix, destination);
}
}
