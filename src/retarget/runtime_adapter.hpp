#pragma once

#include "rig_package.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hd2aa::retarget
{
struct runtime_profile_identity
{
	uint64_t unit_id = 0;
	uint32_t entries = 0;
	std::string table_sha256;
	std::string filename;
	std::string package_sha256;
};

struct table_association
{
	size_t rig_table_index = 0;
	size_t profile_index = 0;
};

struct association_result
{
	bool valid = false;
	std::vector<table_association> tables;
	std::vector<std::string> errors;
};

inline association_result associate_tables(const rig_package &rig,
	const std::vector<runtime_profile_identity> &profiles)
{
	association_result result;
	for (size_t rig_index = 0; rig_index < rig.tables.size(); ++rig_index)
	{
		const rig_table &table = rig.tables[rig_index];
		const bool affects_output = std::any_of(table.slots.begin(), table.slots.end(),
			[&rig](const rig_slot &slot) { return rig.bones[slot.target_bone].affected; });
		if (!affects_output)
			continue;
		std::vector<size_t> matches;
		for (size_t profile_index = 0; profile_index < profiles.size(); ++profile_index)
		{
			const runtime_profile_identity &profile = profiles[profile_index];
			if (profile.unit_id != table.unit_id || profile.entries != table.entries ||
				profile.table_sha256 != table.table_sha256)
				continue;
			const bool dependency = std::any_of(table.profile_dependencies.begin(),
				table.profile_dependencies.end(), [&profile](const profile_dependency &item) {
					return item.filename == profile.filename && item.sha256 == profile.package_sha256;
				});
			if (dependency)
				matches.push_back(profile_index);
		}
		if (matches.size() != 1)
		{
			result.errors.push_back("rig table " + std::to_string(rig_index) +
				(matches.empty() ? " has no exact active profile" : " has ambiguous active profiles"));
			continue;
		}
		result.tables.push_back({ rig_index, matches.front() });
	}
	if (result.tables.empty())
		result.errors.push_back("rig has no affected table with a verified active profile");
	result.valid = result.errors.empty();
	return result;
}
}
