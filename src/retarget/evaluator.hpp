#pragma once

#include "math_affine.hpp"

#include <cstddef>
#include <vector>

namespace hd2aa::retarget
{
enum class retarget_error
{
	none,
	empty_rig,
	length_mismatch,
	invalid_parent,
	invalid_matrix,
	linear_rest_changed,
	root_rest_changed,
	math_failure
};

template <typename T>
struct retarget_result
{
	retarget_error failure = retarget_error::none;
	size_t failed_node = static_cast<size_t>(-1);
	T value {};

	explicit operator bool() const { return failure == retarget_error::none; }
};

inline retarget_error validate_parents(const std::vector<int32_t> &parents)
{
	if (parents.empty())
		return retarget_error::empty_rig;
	for (size_t index = 0; index < parents.size(); ++index)
		if (parents[index] < -1 || parents[index] >= static_cast<int32_t>(index))
			return retarget_error::invalid_parent;
	return retarget_error::none;
}

inline retarget_result<std::vector<matrix4>> world_from_local(
	const std::vector<matrix4> &local, const std::vector<int32_t> &parents)
{
	retarget_result<std::vector<matrix4>> result;
	result.failure = validate_parents(parents);
	if (result.failure != retarget_error::none)
		return result;
	if (local.size() != parents.size())
	{
		result.failure = retarget_error::length_mismatch;
		return result;
	}
	result.value.reserve(local.size());
	for (size_t index = 0; index < local.size(); ++index)
	{
		if (!affine(local[index]))
		{
			result.failure = retarget_error::invalid_matrix;
			result.failed_node = index;
			result.value.clear();
			return result;
		}
		result.value.push_back(parents[index] == -1 ? local[index] :
			multiply(result.value[parents[index]], local[index]));
	}
	return result;
}

inline retarget_result<std::vector<matrix4>> local_from_world(
	const std::vector<matrix4> &world, const std::vector<int32_t> &parents)
{
	retarget_result<std::vector<matrix4>> result;
	result.failure = validate_parents(parents);
	if (result.failure != retarget_error::none)
		return result;
	if (world.size() != parents.size())
	{
		result.failure = retarget_error::length_mismatch;
		return result;
	}
	result.value.reserve(world.size());
	for (size_t index = 0; index < world.size(); ++index)
	{
		if (!affine(world[index]))
		{
			result.failure = retarget_error::invalid_matrix;
			result.failed_node = index;
			result.value.clear();
			return result;
		}
		if (parents[index] == -1)
			result.value.push_back(world[index]);
		else
		{
			const auto parent_inverse = inverse(world[parents[index]]);
			if (!parent_inverse)
			{
				result.failure = retarget_error::math_failure;
				result.failed_node = index;
				result.value.clear();
				return result;
			}
			result.value.push_back(multiply(parent_inverse.value, world[index]));
		}
	}
	return result;
}

inline retarget_result<std::vector<matrix4>> evaluate_full(
	const std::vector<matrix4> &source_world,
	const std::vector<matrix4> &source_rest_local,
	const std::vector<matrix4> &target_rest_local,
	const std::vector<matrix4> &delta_basis,
	const std::vector<int32_t> &parents)
{
	retarget_result<std::vector<matrix4>> result;
	if (source_world.size() != parents.size() || source_rest_local.size() != parents.size() ||
		target_rest_local.size() != parents.size() || delta_basis.size() != parents.size())
	{
		result.failure = retarget_error::length_mismatch;
		return result;
	}
	const auto source_local = local_from_world(source_world, parents);
	if (!source_local)
	{
		result.failure = source_local.failure;
		result.failed_node = source_local.failed_node;
		return result;
	}
	std::vector<matrix4> target_local;
	target_local.reserve(parents.size());
	for (size_t index = 0; index < parents.size(); ++index)
	{
		const auto rest_inverse = inverse(source_rest_local[index]);
		const auto basis_inverse = inverse(delta_basis[index]);
		if (!rest_inverse || !basis_inverse || !affine(target_rest_local[index]))
		{
			result.failure = retarget_error::math_failure;
			result.failed_node = index;
			return result;
		}
		const matrix4 delta = multiply(rest_inverse.value, source_local.value[index]);
		target_local.push_back(multiply(multiply(multiply(target_rest_local[index],
			delta_basis[index]), delta), basis_inverse.value));
	}
	return world_from_local(target_local, parents);
}

inline retarget_result<std::vector<vector3>> rest_translation_deltas(
	const std::vector<matrix4> &source, const std::vector<matrix4> &target,
	double tolerance = 1.0e-8)
{
	retarget_result<std::vector<vector3>> result;
	if (source.size() != target.size())
	{
		result.failure = retarget_error::length_mismatch;
		return result;
	}
	for (size_t index = 0; index < source.size(); ++index)
	{
		if (!affine(source[index]) || !affine(target[index]))
		{
			result.failure = retarget_error::invalid_matrix;
			result.failed_node = index;
			return result;
		}
		const auto source_linear = linear_part(source[index]);
		const auto target_linear = linear_part(target[index]);
		for (size_t value = 0; value < source_linear.size(); ++value)
			if (std::fabs(source_linear[value] - target_linear[value]) > tolerance)
			{
				result.failure = retarget_error::linear_rest_changed;
				result.failed_node = index;
				return result;
			}
		const auto a = translation_part(source[index]);
		const auto b = translation_part(target[index]);
		result.value.push_back({ b[0] - a[0], b[1] - a[1], b[2] - a[2] });
	}
	return result;
}

inline retarget_result<std::vector<vector3>> propagate_displacements(
	const std::vector<linear3> &native_linear,
	const std::vector<vector3> &rest_deltas,
	const std::vector<int32_t> &parents)
{
	retarget_result<std::vector<vector3>> result;
	result.failure = validate_parents(parents);
	if (result.failure != retarget_error::none)
		return result;
	if (native_linear.size() != parents.size() || rest_deltas.size() != parents.size())
	{
		result.failure = retarget_error::length_mismatch;
		return result;
	}
	result.value.resize(parents.size());
	for (size_t index = 0; index < parents.size(); ++index)
	{
		if (!inverse(native_linear[index]) || !finite(rest_deltas[index]))
		{
			result.failure = retarget_error::math_failure;
			result.failed_node = index;
			result.value.clear();
			return result;
		}
		if (parents[index] == -1)
		{
			if (rest_deltas[index] != vector3 {})
			{
				result.failure = retarget_error::root_rest_changed;
				result.failed_node = index;
				result.value.clear();
				return result;
			}
			result.value[index] = {};
		}
		else
		{
			const auto local = multiply(native_linear[parents[index]], rest_deltas[index]);
			for (size_t axis = 0; axis < 3; ++axis)
				result.value[index][axis] = result.value[parents[index]][axis] + local[axis];
		}
	}
	return result;
}

inline math_result<matrix4> encode_desired(const matrix4 &source_world,
	const matrix4 &target_world, const matrix4 &mesh_bind)
{
	math_result<matrix4> result;
	const auto source_inverse = inverse(source_world);
	if (!source_inverse)
	{
		result.failure = source_inverse.failure;
		result.condition = source_inverse.condition;
		return result;
	}
	if (!affine(target_world) || !affine(mesh_bind))
	{
		result.failure = math_error::not_affine;
		return result;
	}
	result.value = multiply(multiply(source_inverse.value, target_world), mesh_bind);
	return result;
}

inline math_result<matrix4> encode_translation_only(const linear3 &native_linear,
	const vector3 &displacement, const matrix4 &mesh_bind)
{
	math_result<matrix4> result;
	const auto native_inverse = inverse(native_linear);
	if (!native_inverse)
	{
		result.failure = native_inverse.failure;
		result.condition = native_inverse.condition;
		return result;
	}
	if (!finite(displacement) || !affine(mesh_bind))
	{
		result.failure = math_error::non_finite;
		return result;
	}
	result.value = multiply(translation(multiply(native_inverse.value, displacement)), mesh_bind);
	return result;
}

inline math_result<linear3> recover_native_linear(const linear3 &observed_skin,
	const linear3 &baseline_bind)
{
	math_result<linear3> result;
	const auto bind_inverse = inverse(baseline_bind);
	if (!bind_inverse)
	{
		result.failure = bind_inverse.failure;
		result.condition = bind_inverse.condition;
		return result;
	}
	if (!finite(observed_skin))
	{
		result.failure = math_error::non_finite;
		return result;
	}
	result.value = multiply(observed_skin, bind_inverse.value);
	return result;
}
}
