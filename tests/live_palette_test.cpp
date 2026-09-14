#include "retarget/live_palette.hpp"
#include "retarget/custom_runtime.hpp"

#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

using namespace hd2aa::retarget;

namespace
{
int failures = 0;

void check(bool value, const char *message)
{
	if (!value)
	{
		++failures;
		std::cerr << "FAIL: " << message << '\n';
	}
}

matrix4 local(double angle, double x)
{
	const double c = std::cos(angle), s = std::sin(angle);
	return { c, -s, 0, x, s, c, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
}

std::vector<uint8_t> pack(const std::vector<matrix4> &matrices, size_t first)
{
	std::vector<uint8_t> bytes((matrices.size() - first) * 48);
	for (size_t index = first; index < matrices.size(); ++index)
		check(encode_t48_column(matrices[index], bytes.data() + (index - first) * 48) ==
			math_error::none, "matrix packs as t48");
	return bytes;
}

bool close(const matrix4 &left, const matrix4 &right, double tolerance = 2.0e-5)
{
	return matrix_difference(left, right) <= tolerance;
}
}

int main()
{
	constexpr size_t count = 6;
	rig_package rig;
	rig.linear_capability = true;
	rig.bones.resize(count);
	std::vector<matrix4> rest_local, animated_local;
	std::vector<int32_t> parents;
	for (size_t index = 0; index < count; ++index)
	{
		rig.bones[index].stable_id = "bone" + std::to_string(index);
		rig.bones[index].parent = index == 0 ? -1 : static_cast<int32_t>(index - 1);
		rig.bones[index].source_rest_local = local(0.0, index == 0 ? 0.0 : 0.25);
		rig.bones[index].target_rest_local = rig.bones[index].source_rest_local;
		rest_local.push_back(rig.bones[index].source_rest_local);
		animated_local.push_back(local(0.1 * static_cast<double>(index), index == 0 ? 0.0 : 0.25));
		parents.push_back(rig.bones[index].parent);
	}
	rig.bones[2].target_rest_local[3] += 0.06;
	rig.bones[2].rest_delta = { 0.06, 0.0, 0.0 };
	for (size_t index = 2; index < count; ++index)
		rig.bones[index].affected = true;
	const auto rest_world = world_from_local(rest_local, parents);
	const auto animated_world = world_from_local(animated_local, parents);
	check(rest_world && animated_world, "fixture worlds build");

	rig_table table;
	table.entries = static_cast<uint32_t>(count);
	for (size_t index = 0; index < count; ++index)
		table.slots.push_back({ static_cast<uint32_t>(index), index, index });
	std::vector<matrix4> bind(count), skin(count);
	for (size_t index = 0; index < count; ++index)
	{
		bind[index] = inverse(rest_world.value[index]).value;
		skin[index] = multiply(animated_world.value[index], bind[index]);
	}
	const std::vector<uint8_t> pristine = pack(bind, 0);
	const std::vector<uint8_t> palette = pack(skin, 1);
	const pose_sample pose = recover_live_pose(palette.data(), palette.size(), pristine,
		rig, table, true, 4, 1.0e-5);
	check(pose && pose.hierarchy_edges == 4 && pose.hierarchy_edges_matched == 4,
		"live palette recovers and validates native hierarchy motion");
	for (size_t index = 1; index < count; ++index)
		check(close(pose.native_world[index], animated_world.value[index]),
			"native world transform is recovered from skin and current bind");
	rig.tables.push_back(table);
	std::vector<uint8_t> window(2048, 0xa5);
	std::memcpy(window.data() + 128, palette.data(), palette.size());
	std::memcpy(window.data() + 1024, palette.data(), palette.size());
	const std::vector<table_binding_view> bindings { { 0, &pristine } };
	const palette_candidate located = find_latest_live_palette(window.data(), window.size(),
		rig, bindings, 4, 1.0e-5);
	check(located.found && located.offset == 1024,
		"bounded mapped-window scan chooses the newest/highest valid palette copy");

	const live_plan plan = build_live_translation_plan(rig, table, pose, pristine);
	check(plan && plan.table.changed_slots.size() == 4,
		"one rest edit produces a uniform descendant write plan");
	const vector3 expected_displacement = multiply(
		linear_part(animated_world.value[1]), vector3 { 0.06, 0.0, 0.0 });
	for (size_t index = 2; index < count; ++index)
	{
		const auto edited_bind = decode_t48_column(plan.table.bytes.data() + index * 48);
		const matrix4 edited_skin = multiply(animated_world.value[index], edited_bind.value);
		const matrix4 expected_skin = multiply(translation(expected_displacement), skin[index]);
		check(close(edited_skin, expected_skin), "published bind produces the target posed displacement");
	}
	check(std::memcmp(plan.table.bytes.data(), pristine.data(), 2 * 48) == 0,
		"unaffected ancestors remain byte-identical");

	std::vector<matrix4> feedback_skin(count);
	for (size_t index = 0; index < count; ++index)
	{
		const auto current = decode_t48_column(plan.table.bytes.data() + index * 48);
		feedback_skin[index] = multiply(animated_world.value[index], current.value);
	}
	const std::vector<uint8_t> feedback_palette = pack(feedback_skin, 1);
	const pose_sample feedback = recover_live_pose(feedback_palette.data(), feedback_palette.size(),
		plan.table.bytes, rig, table, true, 4, 1.0e-5);
	const live_plan second = build_live_translation_plan(rig, table, feedback, pristine);
	check(feedback && second && second.table.bytes == plan.table.bytes,
		"current-bind recovery prevents publisher feedback drift");

	rig.tables[0].unit_id = 0x1234;
	rig.tables[0].table_sha256 = std::string(64, '1');
	rig.tables[0].profile_dependencies.push_back({ "test.hd2profile", std::string(64, '2') });
	runtime_profile_data runtime_profile;
	runtime_profile.identity = { 0x1234, static_cast<uint32_t>(count), std::string(64, '1'),
		"test.hd2profile", std::string(64, '2') };
	runtime_profile.pristine = pristine;
	custom_runtime runtime;
	std::string runtime_error;
	check(runtime.configure(rig, { runtime_profile }, runtime_error), runtime_error.c_str());
	check(runtime.toggle(), "F8-equivalent action persists requested ON");
	runtime.observe_window(window.data(), window.size(), 55, 0, 10);
	const uintptr_t table_address = 0x10000;
	std::vector<uint8_t> memory = pristine;
	auto read = [&](uintptr_t address, void *out, size_t size) {
		if (address < table_address || address - table_address > memory.size() ||
			size > memory.size() - (address - table_address)) return false;
		std::memcpy(out, memory.data() + address - table_address, size);
		return true;
	};
	auto write = [&](uintptr_t address, const void *data, size_t size) {
		if (address < table_address || address - table_address > memory.size() ||
			size > memory.size() - (address - table_address)) return false;
		std::memcpy(memory.data() + address - table_address, data, size);
		return true;
	};
	runtime.service({ { table_address, 0, 1 } }, 1, 10, read, write);
	check(runtime.status() == custom_status::applied && memory == plan.table.bytes,
		"runtime connects qualified live pose to the guarded IB publisher");
	memory = pristine;
	runtime.service({ { table_address, 0, 2 } }, 2, 11, read, write);
	check(runtime.status() == custom_status::applied && memory == plan.table.bytes,
		"scene refill at a retained address rebases and reapplies the requested rig");
	check(!runtime.toggle(), "second F8-equivalent action persists requested OFF");
	runtime.service({}, 2, 12, read, write);
	check(runtime.status() == custom_status::disabled && memory == pristine,
		"runtime restores pristine bytes when requested OFF");

	auto broken_skin = skin;
	broken_skin[3][3] += 1.0;
	const std::vector<uint8_t> broken_palette = pack(broken_skin, 1);
	const pose_sample broken = recover_live_pose(broken_palette.data(), broken_palette.size(),
		pristine, rig, table, true, 4, 0.02);
	check(!broken && broken.failure == pose_error::hierarchy_residual,
		"a matrix-shaped stranger fails hierarchy ownership validation");

	rig.bones[1].rest_delta = { 0.01, 0.0, 0.0 };
	rig.bones[1].affected = true;
	const live_plan missing_root = build_live_translation_plan(rig, table, pose, pristine);
	check(!missing_root && missing_root.failure == pose_error::missing_required_pose,
		"missing slot-zero parent motion blocks the plan instead of guessing identity");

	if (failures == 0)
		std::cout << "live palette recovery and dynamic translation planning passed\n";
	return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
