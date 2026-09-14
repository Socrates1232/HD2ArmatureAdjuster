#pragma once

#include "evaluator.hpp"
#include "json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hd2aa::retarget
{
struct profile_dependency
{
	std::string filename;
	std::string sha256;
};

struct rig_bone
{
	std::string stable_id;
	std::string display_name;
	int32_t parent = -1;
	matrix4 source_rest_local {};
	matrix4 target_rest_local {};
	vector3 rest_delta {};
	bool affected = false;
};

struct rig_slot
{
	uint32_t slot = 0;
	size_t source_bone = 0;
	size_t target_bone = 0;
};

struct rig_table
{
	uint64_t unit_id = 0;
	std::string table_sha256;
	uint32_t entries = 0;
	uint32_t lod_mask = 0;
	std::string pose_space_adapter_id;
	std::vector<profile_dependency> profile_dependencies;
	std::vector<rig_slot> slots;
};

struct rig_package
{
	std::string rig_id;
	std::string source_reference_sha256;
	bool linear_capability = false;
	std::vector<rig_bone> bones;
	std::vector<rig_table> tables;
};

inline bool json_fields(const json::value &value,
	std::initializer_list<const char *> allowed, std::string &error)
{
	if (value.type != json::kind::object)
	{
		error = "expected a JSON object";
		return false;
	}
	for (const auto &item : value.object)
		if (std::none_of(allowed.begin(), allowed.end(), [&item](const char *name) {
			return item.first == name;
		}))
		{
			error = "unknown JSON field: " + item.first;
			return false;
		}
	return true;
}

inline const json::value *json_required(const json::value &value, const char *name,
	json::kind type, std::string &error)
{
	const json::value *result = value.find(name);
	if (result == nullptr || result->type != type)
		error = std::string("missing or invalid field: ") + name;
	return result != nullptr && result->type == type ? result : nullptr;
}

inline bool valid_hex(const std::string &value, size_t length)
{
	return value.size() == length && std::all_of(value.begin(), value.end(), [](char item) {
		return (item >= '0' && item <= '9') || (item >= 'a' && item <= 'f');
	});
}

inline bool parse_hex64(const std::string &value, uint64_t &out)
{
	if (!valid_hex(value, 16))
		return false;
	out = 0;
	for (char item : value)
		out = out * 16 + static_cast<uint64_t>(item <= '9' ? item - '0' : item - 'a' + 10);
	return true;
}

inline bool json_u32(const json::value &value, uint32_t &out)
{
	if (value.type != json::kind::number || value.number < 0.0 ||
		value.number > static_cast<double>(std::numeric_limits<uint32_t>::max()) ||
		std::floor(value.number) != value.number)
		return false;
	out = static_cast<uint32_t>(value.number);
	return true;
}

inline bool json_matrix(const json::value &value, matrix4 &out)
{
	if (value.type != json::kind::array || value.array.size() != 16)
		return false;
	for (size_t index = 0; index < 16; ++index)
	{
		if (value.array[index].type != json::kind::number)
			return false;
		out[index] = value.array[index].number;
	}
	return affine(out, 1.0e-5);
}

inline double matrix_difference(const matrix4 &left, const matrix4 &right)
{
	double result = 0.0;
	for (size_t index = 0; index < left.size(); ++index)
		result = std::max(result, std::fabs(left[index] - right[index]));
	return result;
}

inline bool parse_rig_json(const char *data, size_t size, rig_package &out,
	std::string &error)
{
	out = {};
	if (data == nullptr || size == 0 || size > 16u * 1024u * 1024u)
	{
		error = "rig JSON is empty or exceeds 16 MiB";
		return false;
	}
	json::value root;
	json::parser parser(data, data + size);
	if (!parser.parse(root, error) || !json_fields(root,
		{ "schema", "rig_id", "source_reference_sha256", "matrix_convention", "unit",
		  "retarget_policy", "binding_mode", "requested_capability", "root_policy",
		  "bones", "tables", "space_adapters", "capability_report", "authoring",
		  "fixture_only" }, error))
		return false;

	auto exact = [&root, &error](const char *field, const char *expected) {
		const auto *value = json_required(root, field, json::kind::string, error);
		if (value == nullptr)
			return false;
		if (value->string != expected)
		{
			error = std::string("unsupported ") + field;
			return false;
		}
		return true;
	};
	if (!exact("schema", "HD2RIG1") ||
		!exact("matrix_convention", "column_vectors_row_major_storage") ||
		!exact("unit", "metre") || !exact("retarget_policy", "copy_effective_local_delta") ||
		!exact("binding_mode", "reshape_existing") || !exact("root_policy", "preserve_native"))
		return false;
	if (const auto *fixture = root.find("fixture_only"); fixture != nullptr &&
		(fixture->type != json::kind::boolean || fixture->boolean))
	{
		error = "fixture-only or invalid rig packages cannot run live";
		return false;
	}
	const auto *rig_id = json_required(root, "rig_id", json::kind::string, error);
	const auto *source_sha = json_required(root, "source_reference_sha256", json::kind::string, error);
	const auto *capability = json_required(root, "requested_capability", json::kind::string, error);
	const auto *bone_values = json_required(root, "bones", json::kind::array, error);
	const auto *table_values = json_required(root, "tables", json::kind::array, error);
	const auto *adapters = json_required(root, "space_adapters", json::kind::array, error);
	const auto *report = json_required(root, "capability_report", json::kind::object, error);
	if (rig_id == nullptr || source_sha == nullptr || capability == nullptr || bone_values == nullptr ||
		table_values == nullptr || adapters == nullptr || report == nullptr)
		return false;
	if (rig_id->string.empty() || rig_id->string.size() > 256 || !valid_hex(source_sha->string, 64))
	{
		error = "invalid rig or source-reference identity";
		return false;
	}
	out.rig_id = rig_id->string;
	out.source_reference_sha256 = source_sha->string;
	out.linear_capability = capability->string == "linear_rest_translation";
	if (!out.linear_capability && capability->string != "full_native_pose")
	{
		error = "unsupported requested capability";
		return false;
	}
	if (bone_values->array.empty() || bone_values->array.size() > 4096 ||
		table_values->array.empty() || table_values->array.size() > 4096)
	{
		error = "rig exceeds bone/table budgets";
		return false;
	}

	std::unordered_map<std::string, size_t> bone_by_id;
	std::vector<matrix4> source_rest, target_rest;
	std::vector<int32_t> parents;
	for (const json::value &value : bone_values->array)
	{
		if (!json_fields(value, { "stable_id", "display_name", "parent_id", "source_rest_local",
			"target_rest_local", "delta_basis", "role" }, error))
			return false;
		const auto *id = json_required(value, "stable_id", json::kind::string, error);
		const auto *display = json_required(value, "display_name", json::kind::string, error);
		const auto *source = value.find("source_rest_local");
		const auto *target = value.find("target_rest_local");
		const auto *basis = value.find("delta_basis");
		const auto *parent = value.find("parent_id");
		if (id == nullptr || display == nullptr || source == nullptr || target == nullptr || basis == nullptr ||
			parent == nullptr || id->string.empty() || id->string.size() > 256 ||
			bone_by_id.count(id->string) != 0)
		{
			error = "invalid or duplicate stable bone ID";
			return false;
		}
		rig_bone bone;
		bone.stable_id = id->string;
		bone.display_name = display->string;
		if (!json_matrix(*source, bone.source_rest_local) ||
			!json_matrix(*target, bone.target_rest_local))
		{
			error = bone.stable_id + ": invalid rest matrix";
			return false;
		}
		matrix4 delta_basis {};
		if (!json_matrix(*basis, delta_basis) ||
			matrix_difference(delta_basis, identity()) > 1.0e-6)
		{
			error = bone.stable_id + ": Stage 1 requires identity delta_basis";
			return false;
		}
		if (parent->type == json::kind::null_value)
			bone.parent = -1;
		else if (parent->type == json::kind::string)
		{
			const auto found = bone_by_id.find(parent->string);
			if (found == bone_by_id.end())
			{
				error = bone.stable_id + ": parent is missing or ordered after child";
				return false;
			}
			bone.parent = static_cast<int32_t>(found->second);
		}
		else
		{
			error = bone.stable_id + ": invalid parent";
			return false;
		}
		bone_by_id.emplace(bone.stable_id, out.bones.size());
		parents.push_back(bone.parent);
		source_rest.push_back(bone.source_rest_local);
		target_rest.push_back(bone.target_rest_local);
		out.bones.push_back(std::move(bone));
	}
	const auto deltas = rest_translation_deltas(source_rest, target_rest, 1.0e-6);
	const bool computed_linear = static_cast<bool>(deltas);
	if (out.linear_capability && !computed_linear)
	{
		error = "REST_LINEAR_CHANGE_REQUIRES_FULL_POSE";
		return false;
	}
	if (computed_linear)
		for (size_t index = 0; index < out.bones.size(); ++index)
		{
			out.bones[index].rest_delta = deltas.value[index];
			const bool direct = deltas.value[index] != vector3 {};
			if (out.bones[index].parent == -1 && direct)
			{
				error = out.bones[index].stable_id + ": root edit violates preserve_native";
				return false;
			}
			out.bones[index].affected = direct || (out.bones[index].parent >= 0 &&
				out.bones[static_cast<size_t>(out.bones[index].parent)].affected);
		}

	std::unordered_set<std::string> adapter_ids;
	for (const auto &adapter : adapters->array)
	{
		if (!json_fields(adapter, { "id", "mode", "matrix", "provider_contract" }, error)) return false;
		const auto *id = json_required(adapter, "id", json::kind::string, error);
		const auto *mode = json_required(adapter, "mode", json::kind::string, error);
		if (id == nullptr || mode == nullptr || id->string.empty() || mode->string != "identity")
		{
			error = "Stage 1 runtime requires identity pose-space adapters";
			return false;
		}
		adapter_ids.insert(id->string);
	}

	std::unordered_set<std::string> table_ids;
	for (const json::value &value : table_values->array)
	{
		if (!json_fields(value, { "unit_id", "table_sha256", "entries", "lod_mask",
			"profile_dependencies", "pose_space_adapter_id", "slots" }, error)) return false;
		const auto *unit = json_required(value, "unit_id", json::kind::string, error);
		const auto *table_sha = json_required(value, "table_sha256", json::kind::string, error);
		const auto *entries = value.find("entries");
		const auto *lod_mask = value.find("lod_mask");
		const auto *dependencies = json_required(value, "profile_dependencies", json::kind::array, error);
		const auto *adapter = json_required(value, "pose_space_adapter_id", json::kind::string, error);
		const auto *slots = json_required(value, "slots", json::kind::array, error);
		rig_table table;
		if (unit == nullptr || table_sha == nullptr || entries == nullptr || lod_mask == nullptr ||
			dependencies == nullptr || adapter == nullptr || slots == nullptr ||
			!parse_hex64(unit->string, table.unit_id) || !valid_hex(table_sha->string, 64) ||
			!json_u32(*entries, table.entries) || table.entries == 0 ||
			!json_u32(*lod_mask, table.lod_mask) || table.lod_mask == 0 ||
			adapter_ids.count(adapter->string) == 0)
		{
			error = "invalid rig table identity or adapter";
			return false;
		}
		table.table_sha256 = table_sha->string;
		table.pose_space_adapter_id = adapter->string;
		const std::string table_id = unit->string + ":" + table.table_sha256 + ":" + std::to_string(table.entries);
		if (!table_ids.insert(table_id).second)
		{
			error = "duplicate rig table identity";
			return false;
		}
		for (const auto &dependency : dependencies->array)
		{
			if (!json_fields(dependency, { "filename", "sha256" }, error)) return false;
			const auto *filename = json_required(dependency, "filename", json::kind::string, error);
			const auto *digest = json_required(dependency, "sha256", json::kind::string, error);
			if (filename == nullptr || digest == nullptr || filename->string.find_first_of("/\\") != std::string::npos ||
				!valid_hex(digest->string, 64))
			{
				error = "invalid profile dependency";
				return false;
			}
			table.profile_dependencies.push_back({ filename->string, digest->string });
		}
		if (table.profile_dependencies.empty() || slots->array.empty())
		{
			error = "rig table has no dependencies or slots";
			return false;
		}
		std::unordered_set<uint32_t> seen_slots;
		for (const auto &slot_value : slots->array)
		{
			if (!json_fields(slot_value, { "slot", "source_bone_id", "target_bone_id", "desired_bind" }, error)) return false;
			const auto *slot_number = slot_value.find("slot");
			const auto *source_id = json_required(slot_value, "source_bone_id", json::kind::string, error);
			const auto *target_id = json_required(slot_value, "target_bone_id", json::kind::string, error);
			rig_slot slot;
			if (slot_number == nullptr || source_id == nullptr || target_id == nullptr ||
				!json_u32(*slot_number, slot.slot) || slot.slot >= table.entries ||
				!seen_slots.insert(slot.slot).second || bone_by_id.count(source_id->string) == 0 ||
				bone_by_id.count(target_id->string) == 0)
			{
				error = "invalid or duplicate rig table slot";
				return false;
			}
			slot.source_bone = bone_by_id[source_id->string];
			slot.target_bone = bone_by_id[target_id->string];
			table.slots.push_back(slot);
		}
		out.tables.push_back(std::move(table));
	}

	if (!json_fields(*report, { "linear_path_eligible", "reasons" }, error)) return false;
	const auto *declared = json_required(*report, "linear_path_eligible", json::kind::boolean, error);
	const auto *reasons = json_required(*report, "reasons", json::kind::array, error);
	if (declared == nullptr || reasons == nullptr || declared->boolean != computed_linear)
	{
		error = "capability report disagrees with semantic validation";
		return false;
	}
	return true;
}
}
