#pragma once

#include "ib_layout.hpp"

#include <algorithm>
#include <cmath>

namespace armature_probe
{
inline affine_matrix multiply(const affine_matrix &a, const affine_matrix &b)
{
	affine_matrix out {};
	for (size_t row = 0; row < 4; ++row)
		for (size_t column = 0; column < 4; ++column)
			for (size_t inner = 0; inner < 4; ++inner)
				out[row * 4 + column] += a[row * 4 + inner] * b[inner * 4 + column];
	return out;
}

inline bool affine_inverse(const affine_matrix &source, affine_matrix &out)
{
	const float a = source[0], b = source[1], c = source[2];
	const float d = source[4], e = source[5], f = source[6];
	const float g = source[8], h = source[9], i = source[10];
	const float determinant = a * (e * i - f * h) - b * (d * i - f * g) +
		c * (d * h - e * g);
	if (!std::isfinite(determinant) || std::fabs(determinant) < 1.0e-8f)
		return false;
	const float reciprocal = 1.0f / determinant;
	out = {
		(e * i - f * h) * reciprocal, (c * h - b * i) * reciprocal,
		(b * f - c * e) * reciprocal, 0.0f,
		(f * g - d * i) * reciprocal, (a * i - c * g) * reciprocal,
		(c * d - a * f) * reciprocal, 0.0f,
		(d * h - e * g) * reciprocal, (b * g - a * h) * reciprocal,
		(a * e - b * d) * reciprocal, 0.0f,
		0.0f, 0.0f, 0.0f, 1.0f
	};
	for (size_t column = 0; column < 3; ++column)
		out[12 + column] = -(source[12] * out[column] +
			source[13] * out[4 + column] + source[14] * out[8 + column]);
	return std::all_of(out.begin(), out.end(), [](float value) { return std::isfinite(value); });
}

inline affine_matrix translation(float x, float y, float z)
{
	return { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1 };
}

// Recover the unmodified skin transform from the palette and the exact IB
// revision which produced it, then put one common correction on its output side.
inline bool drive_inverse_bind(const affine_matrix &baseline_ib,
	const affine_matrix &used_ib, const affine_matrix &observed_skin,
	const affine_matrix &correction, affine_matrix &out)
{
	affine_matrix used_inverse {}, skin_inverse {};
	if (!affine_inverse(used_ib, used_inverse))
		return false;
	const affine_matrix native_skin = multiply(multiply(baseline_ib, used_inverse), observed_skin);
	if (!affine_inverse(native_skin, skin_inverse))
		return false;
	out = multiply(multiply(multiply(native_skin, correction), skin_inverse), baseline_ib);
	return std::all_of(out.begin(), out.end(), [](float value) {
		return std::isfinite(value) && std::fabs(value) < 1.0e6f;
	});
}
}
