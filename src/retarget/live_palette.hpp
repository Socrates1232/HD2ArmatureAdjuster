#pragma once

#include "plan_encoder.hpp"
#include "rig_package.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace hd2aa::retarget
{
enum class pose_error
{
	none,
	invalid_size,
	invalid_slot,
	invalid_matrix,
	conflicting_duplicate,
	insufficient_hierarchy_evidence,
	hierarchy_residual,
	missing_required_pose,
	plan_failure,
};

struct pose_sample
{
	pose_error failure = pose_error::none;
	std::vector<matrix4> native_world;
	std::vector<bool> bone_available;
	std::vector<linear3> observed_skin_linear;
	std::vector<bool> slot_available;
	uint32_t hierarchy_edges = 0;
	uint32_t hierarchy_edges_matched = 0;
	double maximum_translation_residual = 0.0;

	explicit operator bool() const { return failure == pose_error::none; }
};

inline double vector_distance(const vector3 &left, const vector3 &right)
{
	const double x = left[0] - right[0];
	const double y = left[1] - right[1];
	const double z = left[2] - right[2];
	return std::sqrt(x * x + y * y + z * z);
}

inline pose_sample recover_live_pose(const uint8_t *palette, size_t palette_bytes,
	const std::vector<uint8_t> &current_bind, const rig_package &rig,
	const rig_table &table, bool slot_zero_omitted = true,
	uint32_t minimum_hierarchy_edges = 4, double translation_tolerance = 0.04)
{
	pose_sample result;
	const uint32_t first_slot = slot_zero_omitted ? 1u : 0u;
	const size_t expected = table.entries >= first_slot ?
		static_cast<size_t>(table.entries - first_slot) * 48 : 0;
	if (palette == nullptr || palette_bytes != expected ||
		current_bind.size() != static_cast<size_t>(table.entries) * 48)
	{
		result.failure = pose_error::invalid_size;
		return result;
	}
	result.native_world.resize(rig.bones.size());
	result.bone_available.assign(rig.bones.size(), false);
	result.observed_skin_linear.resize(table.entries);
	result.slot_available.assign(table.entries, false);
	for (const rig_slot &slot : table.slots)
	{
		if (slot.slot >= table.entries || slot.source_bone >= rig.bones.size())
		{
			result.failure = pose_error::invalid_slot;
			return result;
		}
		if (slot.slot < first_slot)
			continue;
		const size_t palette_offset = static_cast<size_t>(slot.slot - first_slot) * 48;
		const size_t bind_offset = static_cast<size_t>(slot.slot) * 48;
		const auto observed = decode_t48_column(palette + palette_offset);
		const auto bind = decode_t48_column(current_bind.data() + bind_offset);
		const auto bind_inverse = bind ? inverse(bind.value) : math_result<matrix4> {};
		if (!observed || !bind || !bind_inverse)
		{
			result.failure = pose_error::invalid_matrix;
			return result;
		}
		const matrix4 native = multiply(observed.value, bind_inverse.value);
		if (!affine(native) || !inverse(linear_part(native)))
		{
			result.failure = pose_error::invalid_matrix;
			return result;
		}
		result.observed_skin_linear[slot.slot] = linear_part(observed.value);
		result.slot_available[slot.slot] = true;
		if (result.bone_available[slot.source_bone])
		{
			if (matrix_difference(result.native_world[slot.source_bone], native) > 2.0e-3)
			{
				result.failure = pose_error::conflicting_duplicate;
				return result;
			}
		}
		else
		{
			result.native_world[slot.source_bone] = native;
			result.bone_available[slot.source_bone] = true;
		}
	}

	for (size_t bone_index = 0; bone_index < rig.bones.size(); ++bone_index)
	{
		const int32_t parent = rig.bones[bone_index].parent;
		if (parent < 0 || !result.bone_available[bone_index] ||
			!result.bone_available[static_cast<size_t>(parent)])
			continue;
		const auto parent_inverse = inverse(result.native_world[static_cast<size_t>(parent)]);
		if (!parent_inverse)
		{
			result.failure = pose_error::invalid_matrix;
			return result;
		}
		const matrix4 local = multiply(parent_inverse.value, result.native_world[bone_index]);
		const double residual = vector_distance(translation_part(local),
			translation_part(rig.bones[bone_index].source_rest_local));
		++result.hierarchy_edges;
		if (residual <= translation_tolerance)
			++result.hierarchy_edges_matched;
		result.maximum_translation_residual = std::max(result.maximum_translation_residual, residual);
	}
	if (result.hierarchy_edges < minimum_hierarchy_edges)
		result.failure = pose_error::insufficient_hierarchy_evidence;
	else if (result.hierarchy_edges_matched * 4 < result.hierarchy_edges * 3)
		result.failure = pose_error::hierarchy_residual;
	return result;
}

struct live_plan
{
	pose_error failure = pose_error::none;
	table_plan table;
	size_t missing_bone = std::numeric_limits<size_t>::max();

	explicit operator bool() const { return failure == pose_error::none; }
};

inline bool table_has_affected_slots(const rig_package &rig, const rig_table &table)
{
	return std::any_of(table.slots.begin(), table.slots.end(), [&rig](const rig_slot &slot) {
		return slot.target_bone < rig.bones.size() && rig.bones[slot.target_bone].affected;
	});
}

inline live_plan build_live_translation_plan(const rig_package &rig, const rig_table &table,
	const pose_sample &pose, const std::vector<uint8_t> &pristine)
{
	live_plan result;
	if (!pose)
	{
		result.failure = pose.failure;
		return result;
	}
	std::vector<bool> required(rig.bones.size(), false);
	for (const rig_slot &slot : table.slots)
	{
		if (slot.target_bone >= rig.bones.size() || !rig.bones[slot.target_bone].affected)
			continue;
		for (int32_t bone = static_cast<int32_t>(slot.target_bone); bone >= 0;)
		{
			if (required[static_cast<size_t>(bone)])
				break;
			required[static_cast<size_t>(bone)] = true;
			bone = rig.bones[static_cast<size_t>(bone)].parent;
		}
	}
	std::vector<vector3> displacement(rig.bones.size());
	for (size_t index = 0; index < rig.bones.size(); ++index)
	{
		if (!required[index])
			continue;
		const rig_bone &bone = rig.bones[index];
		if (bone.parent < 0)
			continue;
		displacement[index] = displacement[static_cast<size_t>(bone.parent)];
		if (bone.rest_delta != vector3 {})
		{
			const size_t parent = static_cast<size_t>(bone.parent);
			if (parent >= pose.bone_available.size() || !pose.bone_available[parent])
			{
				result.failure = pose_error::missing_required_pose;
				result.missing_bone = parent;
				return result;
			}
			const vector3 local = multiply(linear_part(pose.native_world[parent]), bone.rest_delta);
			for (size_t axis = 0; axis < 3; ++axis)
				displacement[index][axis] += local[axis];
		}
	}

	std::vector<translation_slot_request> requests;
	for (const rig_slot &slot : table.slots)
	{
		if (!rig.bones[slot.target_bone].affected)
			continue;
		if (slot.slot >= pose.slot_available.size() || !pose.slot_available[slot.slot])
		{
			result.failure = pose_error::missing_required_pose;
			result.missing_bone = slot.source_bone;
			return result;
		}
		requests.push_back({ slot.slot, pose.observed_skin_linear[slot.slot],
			displacement[slot.target_bone] });
	}
	result.table = build_translation_plan(pristine, table.entries, requests);
	if (result.table.failure != plan_error::none)
		result.failure = pose_error::plan_failure;
	return result;
}

struct table_binding_view
{
	size_t rig_table_index = 0;
	const std::vector<uint8_t> *current_bind = nullptr;
};

struct palette_candidate
{
	bool found = false;
	size_t rig_table_index = 0;
	size_t offset = 0;
	pose_sample pose;
};

inline bool plausible_t48(const uint8_t *bytes)
{
	const auto matrix = decode_t48_column(bytes);
	if (!matrix)
		return false;
	const auto inverse_linear = inverse(linear_part(matrix.value));
	if (!inverse_linear)
		return false;
	for (double value : linear_part(matrix.value))
		if (std::fabs(value) > 4.0)
			return false;
	const vector3 point = translation_part(matrix.value);
	return std::fabs(point[0]) < 10000.0 && std::fabs(point[1]) < 10000.0 &&
		std::fabs(point[2]) < 10000.0;
}

inline std::vector<palette_candidate> find_latest_live_palettes(const uint8_t *window, size_t size,
	const rig_package &rig, const std::vector<table_binding_view> &bindings,
	uint32_t minimum_hierarchy_edges = 4, double translation_tolerance = 0.04)
{
	std::vector<palette_candidate> result(bindings.size());
	if (window == nullptr)
		return result;
	for (size_t offset = 0; offset + 48 <= size; offset += 16)
	{
		if (!plausible_t48(window + offset))
			continue;
		for (size_t binding_index = 0; binding_index < bindings.size(); ++binding_index)
		{
			const table_binding_view &binding = bindings[binding_index];
			if (binding.rig_table_index >= rig.tables.size() || binding.current_bind == nullptr)
				continue;
			const rig_table &table = rig.tables[binding.rig_table_index];
			if (table.entries < 6)
				continue;
			const size_t candidate_bytes = static_cast<size_t>(table.entries - 1) * 48;
			if (candidate_bytes > size - offset)
				continue;
			if (!plausible_t48(window + offset + (candidate_bytes / 96) * 48) ||
				!plausible_t48(window + offset + candidate_bytes - 48))
				continue;
			pose_sample pose = recover_live_pose(window + offset, candidate_bytes,
				*binding.current_bind, rig, table, true, minimum_hierarchy_edges,
				translation_tolerance);
			if (pose)
			{
				result[binding_index].found = true;
				result[binding_index].rig_table_index = binding.rig_table_index;
				result[binding_index].offset = offset;
				result[binding_index].pose = std::move(pose);
			}
		}
	}
	return result;
}

inline palette_candidate find_latest_live_palette(const uint8_t *window, size_t size,
	const rig_package &rig, const std::vector<table_binding_view> &bindings,
	uint32_t minimum_hierarchy_edges = 4, double translation_tolerance = 0.04)
{
	palette_candidate result;
	for (palette_candidate &candidate : find_latest_live_palettes(window, size, rig, bindings,
		minimum_hierarchy_edges, translation_tolerance))
		if (candidate.found && (!result.found || candidate.offset >= result.offset))
			result = std::move(candidate);
	return result;
}
}
