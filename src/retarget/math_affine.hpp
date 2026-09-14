#pragma once

#include "../ib_layout.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace hd2aa::retarget
{
using vector3 = std::array<double, 3>;
using linear3 = std::array<double, 9>;
using matrix4 = std::array<double, 16>;

constexpr double default_condition_limit = 1.0e6;

enum class math_error
{
	none,
	non_finite,
	not_affine,
	singular,
	ill_conditioned,
	float32_overflow
};

template <typename T>
struct math_result
{
	math_error failure = math_error::none;
	T value {};
	double condition = 0.0;

	explicit operator bool() const { return failure == math_error::none; }
};

inline matrix4 identity()
{
	return { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
}

inline matrix4 translation(const vector3 &value)
{
	matrix4 result = identity();
	result[3] = value[0];
	result[7] = value[1];
	result[11] = value[2];
	return result;
}

inline bool finite(const vector3 &value)
{
	return std::all_of(value.begin(), value.end(), [](double item) {
		return std::isfinite(item);
	});
}

inline bool finite(const linear3 &value)
{
	return std::all_of(value.begin(), value.end(), [](double item) {
		return std::isfinite(item);
	});
}

inline bool finite(const matrix4 &value)
{
	return std::all_of(value.begin(), value.end(), [](double item) {
		return std::isfinite(item);
	});
}

inline bool affine(const matrix4 &value, double tolerance = 1.0e-10)
{
	return finite(value) && std::fabs(value[12]) <= tolerance &&
		std::fabs(value[13]) <= tolerance && std::fabs(value[14]) <= tolerance &&
		std::fabs(value[15] - 1.0) <= tolerance;
}

inline linear3 linear_part(const matrix4 &value)
{
	return { value[0], value[1], value[2], value[4], value[5], value[6],
		value[8], value[9], value[10] };
}

inline vector3 translation_part(const matrix4 &value)
{
	return { value[3], value[7], value[11] };
}

inline matrix4 multiply(const matrix4 &left, const matrix4 &right)
{
	matrix4 result {};
	for (size_t row = 0; row < 4; ++row)
		for (size_t column = 0; column < 4; ++column)
			for (size_t inner = 0; inner < 4; ++inner)
				result[row * 4 + column] += left[row * 4 + inner] *
					right[inner * 4 + column];
	return result;
}

inline linear3 multiply(const linear3 &left, const linear3 &right)
{
	linear3 result {};
	for (size_t row = 0; row < 3; ++row)
		for (size_t column = 0; column < 3; ++column)
			for (size_t inner = 0; inner < 3; ++inner)
				result[row * 3 + column] += left[row * 3 + inner] *
					right[inner * 3 + column];
	return result;
}

inline vector3 multiply(const linear3 &matrix, const vector3 &column)
{
	return {
		matrix[0] * column[0] + matrix[1] * column[1] + matrix[2] * column[2],
		matrix[3] * column[0] + matrix[4] * column[1] + matrix[5] * column[2],
		matrix[6] * column[0] + matrix[7] * column[1] + matrix[8] * column[2]
	};
}

inline double infinity_norm(const linear3 &value)
{
	double result = 0.0;
	for (size_t row = 0; row < 3; ++row)
		result = std::max(result, std::fabs(value[row * 3]) +
			std::fabs(value[row * 3 + 1]) + std::fabs(value[row * 3 + 2]));
	return result;
}

inline math_result<linear3> inverse(const linear3 &value,
	double condition_limit = default_condition_limit)
{
	math_result<linear3> result;
	if (!finite(value) || !std::isfinite(condition_limit) || condition_limit <= 0.0)
	{
		result.failure = math_error::non_finite;
		return result;
	}
	const double a = value[0], b = value[1], c = value[2];
	const double d = value[3], e = value[4], f = value[5];
	const double g = value[6], h = value[7], i = value[8];
	const double determinant = a * (e * i - f * h) - b * (d * i - f * g) +
		c * (d * h - e * g);
	if (determinant == 0.0 || !std::isfinite(determinant))
	{
		result.failure = math_error::singular;
		return result;
	}
	const double scale = 1.0 / determinant;
	result.value = {
		(e * i - f * h) * scale, (c * h - b * i) * scale, (b * f - c * e) * scale,
		(f * g - d * i) * scale, (a * i - c * g) * scale, (c * d - a * f) * scale,
		(d * h - e * g) * scale, (b * g - a * h) * scale, (a * e - b * d) * scale
	};
	result.condition = infinity_norm(value) * infinity_norm(result.value);
	if (!finite(result.value))
		result.failure = math_error::singular;
	else if (!std::isfinite(result.condition) || result.condition > condition_limit)
		result.failure = math_error::ill_conditioned;
	return result;
}

inline math_result<matrix4> inverse(const matrix4 &value,
	double condition_limit = default_condition_limit)
{
	math_result<matrix4> result;
	if (!affine(value))
	{
		result.failure = finite(value) ? math_error::not_affine : math_error::non_finite;
		return result;
	}
	const auto linear_inverse = inverse(linear_part(value), condition_limit);
	if (!linear_inverse)
	{
		result.failure = linear_inverse.failure;
		result.condition = linear_inverse.condition;
		return result;
	}
	result.value = identity();
	for (size_t row = 0; row < 3; ++row)
		for (size_t column = 0; column < 3; ++column)
			result.value[row * 4 + column] = linear_inverse.value[row * 3 + column];
	const vector3 position = translation_part(value);
	const vector3 inverted = multiply(linear_inverse.value, position);
	result.value[3] = -inverted[0];
	result.value[7] = -inverted[1];
	result.value[11] = -inverted[2];
	result.condition = linear_inverse.condition;
	return result;
}

inline matrix4 transpose_existing_row(const armature_probe::affine_matrix &value)
{
	matrix4 result {};
	for (size_t row = 0; row < 4; ++row)
		for (size_t column = 0; column < 4; ++column)
			result[row * 4 + column] = value[column * 4 + row];
	return result;
}

inline armature_probe::affine_matrix transpose_to_existing_row(const matrix4 &value)
{
	armature_probe::affine_matrix result {};
	for (size_t row = 0; row < 4; ++row)
		for (size_t column = 0; column < 4; ++column)
			result[row * 4 + column] = static_cast<float>(value[column * 4 + row]);
	return result;
}

inline math_result<matrix4> decode_t48_column(const void *bytes)
{
	math_result<matrix4> result;
	result.value = transpose_existing_row(armature_probe::decode_t48(bytes));
	if (!affine(result.value, 1.0e-5))
		result.failure = finite(result.value) ? math_error::not_affine : math_error::non_finite;
	return result;
}

inline math_error encode_t48_column(const matrix4 &value, void *bytes)
{
	if (!affine(value))
		return finite(value) ? math_error::not_affine : math_error::non_finite;
	constexpr double limit = static_cast<double>(std::numeric_limits<float>::max());
	for (size_t row = 0; row < 3; ++row)
		for (size_t column = 0; column < 4; ++column)
			if (std::fabs(value[row * 4 + column]) > limit)
				return math_error::float32_overflow;
	armature_probe::encode_t48(transpose_to_existing_row(value), bytes);
	return math_error::none;
}
}
