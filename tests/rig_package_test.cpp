#include "retarget/rig_package.hpp"
#include "retarget/runtime_adapter.hpp"
#include "retarget/sha256.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

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

std::string package(const char *root_target = "[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]",
	const char *declared = "true")
{
	const char *identity = "[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]";
	const char *child_source = "[1,0,0,1,0,1,0,0,0,0,1,0,0,0,0,1]";
	const char *child_target = "[1,0,0,1.06,0,1,0,0,0,0,1,0,0,0,0,1]";
	return std::string("{") +
		"\"schema\":\"HD2RIG1\",\"rig_id\":\"test\"," +
		"\"source_reference_sha256\":\"" + std::string(64, '0') + "\"," +
		"\"matrix_convention\":\"column_vectors_row_major_storage\"," +
		"\"unit\":\"metre\",\"retarget_policy\":\"copy_effective_local_delta\"," +
		"\"binding_mode\":\"reshape_existing\",\"requested_capability\":\"linear_rest_translation\"," +
		"\"root_policy\":\"preserve_native\",\"bones\":[" +
		"{\"stable_id\":\"root\",\"display_name\":\"Root\",\"parent_id\":null," +
		"\"source_rest_local\":" + identity + ",\"target_rest_local\":" + root_target +
		",\"delta_basis\":" + identity + "}," +
		"{\"stable_id\":\"child\",\"display_name\":\"Child\",\"parent_id\":\"root\"," +
		"\"source_rest_local\":" + child_source + ",\"target_rest_local\":" + child_target +
		",\"delta_basis\":" + identity + "}]," +
		"\"tables\":[{\"unit_id\":\"0123456789abcdef\",\"table_sha256\":\"" +
		std::string(64, '1') + "\",\"entries\":2,\"lod_mask\":1," +
		"\"profile_dependencies\":[{\"filename\":\"test.hd2profile\",\"sha256\":\"" +
		std::string(64, '2') + "\"}],\"pose_space_adapter_id\":\"common_identity\"," +
		"\"slots\":[{\"slot\":0,\"source_bone_id\":\"root\",\"target_bone_id\":\"root\"}," +
		"{\"slot\":1,\"source_bone_id\":\"child\",\"target_bone_id\":\"child\"}]}]," +
		"\"space_adapters\":[{\"id\":\"common_identity\",\"mode\":\"identity\"}]," +
		"\"capability_report\":{\"linear_path_eligible\":" + declared + ",\"reasons\":[]}," +
		"\"authoring\":{\"basis_mode\":\"preserved\",\"notes\":\"unicode \\u80a9\"}}";
}
}

int main(int argc, char **argv)
{
	check(hd2aa::sha256_hex("abc", 3) ==
		"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
		"portable SHA-256 matches the standard vector");
	rig_package rig;
	std::string error;
	const std::string valid = package();
	check(parse_rig_json(valid.data(), valid.size(), rig, error), error.c_str());
	check(rig.bones.size() == 2 && rig.tables.size() == 1, "loader retains graph and tables");
	check(!rig.bones[0].affected && rig.bones[1].affected, "loader derives affected closure");
	check(rig.bones[1].rest_delta[0] > 0.059 && rig.bones[1].rest_delta[0] < 0.061,
		"loader recomputes rest translation delta");
	check(rig.tables[0].slots[1].source_bone == 1, "loader resolves stable slot identity");
	const runtime_profile_identity profile { 0x0123456789abcdefull, 2,
		std::string(64, '1'), "test.hd2profile", std::string(64, '2') };
	const association_result associated = associate_tables(rig, { profile });
	check(associated.valid && associated.tables.size() == 1 &&
		associated.tables[0].profile_index == 0,
		"rig table requires exact table and profile-package hashes");
	auto stale_profile = profile;
	stale_profile.package_sha256[0] = '3';
	check(!associate_tables(rig, { stale_profile }).valid,
		"stale profile dependency blocks runtime association");

	std::string duplicate = valid;
	duplicate.insert(1, "\"schema\":\"HD2RIG1\",");
	check(!parse_rig_json(duplicate.data(), duplicate.size(), rig, error) &&
		error.find("duplicate") != std::string::npos, "duplicate JSON keys are rejected");
	const std::string bad_report = package(
		"[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]", "false");
	check(!parse_rig_json(bad_report.data(), bad_report.size(), rig, error) &&
		error.find("capability report") != std::string::npos, "declared capability is not trusted");
	const std::string moved_root = package(
		"[1,0,0,0.1,0,1,0,0,0,0,1,0,0,0,0,1]");
	check(!parse_rig_json(moved_root.data(), moved_root.size(), rig, error) &&
		error.find("root edit") != std::string::npos, "preserve-native root is enforced");
	if (argc == 2)
	{
		std::ifstream stream(argv[1], std::ios::binary);
		const std::string real_package((std::istreambuf_iterator<char>(stream)), {});
		check(stream.good() || stream.eof(), "real package can be read");
		check(parse_rig_json(real_package.data(), real_package.size(), rig, error), error.c_str());
		check(rig.bones.size() > 2 && !rig.tables.empty(),
			"real exported package loads through the runtime parser");
	}

	if (failures == 0)
		std::cout << "strict HD2RIG1 package loading passed\n";
	return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
