#pragma once

#include "ib_layout.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace armature_probe::ib_replacement
{
using vector3 = std::array<double, 3>;
using linear3 = std::array<double, 9>;

constexpr size_t t48_entry_size = 48;
constexpr double default_condition_limit = 1.0e8;

enum class error
{
	none,
	invalid_table_size,
	slot_out_of_range,
	duplicate_slot,
	non_finite_input,
	singular_basis,
	ill_conditioned_basis
};

struct inverse_result
{
	error failure = error::none;
	linear3 value {};
	double condition = 0.0;
};

struct entry_result
{
	error failure = error::none;
	std::array<uint8_t, t48_entry_size> bytes {};
	vector3 pre_offset {};
	double condition = 0.0;
};

struct slot_request
{
	size_t slot = 0;
	linear3 observed_skin_linear {};
	vector3 output_displacement {};
};

struct table_plan
{
	error failure = error::none;
	size_t failed_request = std::numeric_limits<size_t>::max();
	std::vector<uint8_t> bytes;
	std::vector<size_t> changed_slots;
};

inline bool finite(const vector3 &value)
{
	return std::all_of(value.begin(), value.end(),
		[](double item) { return std::isfinite(item); });
}

inline bool finite(const linear3 &value)
{
	return std::all_of(value.begin(), value.end(),
		[](double item) { return std::isfinite(item); });
}

inline linear3 linear_part(const affine_matrix &matrix)
{
	return {
		matrix[0], matrix[1], matrix[2],
		matrix[4], matrix[5], matrix[6],
		matrix[8], matrix[9], matrix[10]
	};
}

inline double infinity_norm(const linear3 &matrix)
{
	double norm = 0.0;
	for (size_t row = 0; row < 3; ++row)
		norm = std::max(norm, std::fabs(matrix[row * 3]) +
			std::fabs(matrix[row * 3 + 1]) + std::fabs(matrix[row * 3 + 2]));
	return norm;
}

inline inverse_result invert(const linear3 &matrix,
	double condition_limit = default_condition_limit)
{
	inverse_result result;
	if (!finite(matrix) || !std::isfinite(condition_limit) || condition_limit <= 0.0)
	{
		result.failure = error::non_finite_input;
		return result;
	}

	const double a = matrix[0], b = matrix[1], c = matrix[2];
	const double d = matrix[3], e = matrix[4], f = matrix[5];
	const double g = matrix[6], h = matrix[7], i = matrix[8];
	const double determinant = a * (e * i - f * h) - b * (d * i - f * g) +
		c * (d * h - e * g);
	if (determinant == 0.0 || !std::isfinite(determinant))
	{
		result.failure = error::singular_basis;
		return result;
	}

	const double scale = 1.0 / determinant;
	result.value = {
		(e * i - f * h) * scale, (c * h - b * i) * scale, (b * f - c * e) * scale,
		(f * g - d * i) * scale, (a * i - c * g) * scale, (c * d - a * f) * scale,
		(d * h - e * g) * scale, (b * g - a * h) * scale, (a * e - b * d) * scale
	};
	if (!finite(result.value))
	{
		result.failure = error::singular_basis;
		return result;
	}

	result.condition = infinity_norm(matrix) * infinity_norm(result.value);
	if (!std::isfinite(result.condition) || result.condition > condition_limit)
		result.failure = error::ill_conditioned_basis;
	return result;
}

inline vector3 multiply(const vector3 &row, const linear3 &matrix)
{
	return {
		row[0] * matrix[0] + row[1] * matrix[3] + row[2] * matrix[6],
		row[0] * matrix[1] + row[1] * matrix[4] + row[2] * matrix[7],
		row[0] * matrix[2] + row[1] * matrix[5] + row[2] * matrix[8]
	};
}

inline affine_matrix multiply(const affine_matrix &left, const affine_matrix &right)
{
	affine_matrix result {};
	for (size_t row = 0; row < 4; ++row)
		for (size_t column = 0; column < 4; ++column)
			for (size_t inner = 0; inner < 4; ++inner)
				result[row * 4 + column] +=
					left[row * 4 + inner] * right[inner * 4 + column];
	return result;
}

inline affine_matrix translation(const vector3 &offset)
{
	return {
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		static_cast<float>(offset[0]), static_cast<float>(offset[1]),
		static_cast<float>(offset[2]), 1.0f
	};
}

inline bool is_zero(const vector3 &value)
{
	return value[0] == 0.0 && value[1] == 0.0 && value[2] == 0.0;
}

inline entry_result make_entry(const void *pristine_t48,
	const linear3 &observed_skin_linear, const vector3 &output_displacement,
	double condition_limit = default_condition_limit)
{
	entry_result result;
	std::memcpy(result.bytes.data(), pristine_t48, result.bytes.size());
	if (!finite(output_displacement))
	{
		result.failure = error::non_finite_input;
		return result;
	}
	if (is_zero(output_displacement))
		return result;

	const auto inverse = invert(observed_skin_linear, condition_limit);
	result.failure = inverse.failure;
	result.condition = inverse.condition;
	if (result.failure != error::none)
		return result;

	result.pre_offset = multiply(output_displacement, inverse.value);
	if (!finite(result.pre_offset))
	{
		result.failure = error::non_finite_input;
		return result;
	}

	std::array<uint8_t, t48_entry_size> translated {};
	translate_world_t48(pristine_t48, static_cast<float>(result.pre_offset[0]),
		static_cast<float>(result.pre_offset[1]), static_cast<float>(result.pre_offset[2]),
		translated.data());
	for (const size_t float_index : {size_t {3}, size_t {7}, size_t {11}})
		std::memcpy(result.bytes.data() + float_index * sizeof(float),
			translated.data() + float_index * sizeof(float), sizeof(float));
	return result;
}

inline table_plan make_table_plan(const std::vector<uint8_t> &pristine_table,
	size_t entry_count, const std::vector<slot_request> &requests,
	double condition_limit = default_condition_limit)
{
	table_plan result;
	result.bytes = pristine_table;
	if (entry_count > std::numeric_limits<size_t>::max() / t48_entry_size ||
		pristine_table.size() != entry_count * t48_entry_size)
	{
		result.failure = error::invalid_table_size;
		return result;
	}

	std::vector<uint8_t> candidate = pristine_table;
	std::vector<size_t> changed_slots;
	std::vector<bool> seen(entry_count, false);
	for (size_t request_index = 0; request_index < requests.size(); ++request_index)
	{
		const auto &request = requests[request_index];
		if (request.slot >= entry_count)
		{
			result.failure = error::slot_out_of_range;
			result.failed_request = request_index;
			return result;
		}
		if (seen[request.slot])
		{
			result.failure = error::duplicate_slot;
			result.failed_request = request_index;
			return result;
		}
		seen[request.slot] = true;

		const size_t offset = request.slot * t48_entry_size;
		const auto replacement = make_entry(pristine_table.data() + offset,
			request.observed_skin_linear, request.output_displacement, condition_limit);
		if (replacement.failure != error::none)
		{
			result.failure = replacement.failure;
			result.failed_request = request_index;
			return result;
		}
		std::memcpy(candidate.data() + offset, replacement.bytes.data(), t48_entry_size);
		if (std::memcmp(pristine_table.data() + offset, replacement.bytes.data(),
			t48_entry_size) != 0)
			changed_slots.push_back(request.slot);
	}

	result.bytes = std::move(candidate);
	result.changed_slots = std::move(changed_slots);
	return result;
}

inline error invert_affine(const affine_matrix &matrix, affine_matrix &result,
	double condition_limit = default_condition_limit)
{
	const auto inverse = invert(linear_part(matrix), condition_limit);
	if (inverse.failure != error::none)
		return inverse.failure;
	result = {
		static_cast<float>(inverse.value[0]), static_cast<float>(inverse.value[1]),
		static_cast<float>(inverse.value[2]), 0.0f,
		static_cast<float>(inverse.value[3]), static_cast<float>(inverse.value[4]),
		static_cast<float>(inverse.value[5]), 0.0f,
		static_cast<float>(inverse.value[6]), static_cast<float>(inverse.value[7]),
		static_cast<float>(inverse.value[8]), 0.0f,
		0.0f, 0.0f, 0.0f, 1.0f
	};
	for (size_t column = 0; column < 3; ++column)
		result[12 + column] = -static_cast<float>(
			matrix[12] * inverse.value[column] +
			matrix[13] * inverse.value[3 + column] +
			matrix[14] * inverse.value[6 + column]);
	return error::none;
}

inline error encode_desired_pose(const affine_matrix &baseline_inverse_bind,
	const affine_matrix &native_pose, const affine_matrix &desired_pose,
	affine_matrix &replacement, double condition_limit = default_condition_limit)
{
	affine_matrix native_inverse {};
	const auto failure = invert_affine(native_pose, native_inverse, condition_limit);
	if (failure != error::none)
		return failure;
	replacement = multiply(multiply(baseline_inverse_bind, desired_pose), native_inverse);
	return error::none;
}
}
