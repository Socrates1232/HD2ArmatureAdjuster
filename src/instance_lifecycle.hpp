#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace instance_lifecycle
{
enum class table_state
{
	pristine,
	our_override,
	unknown,
};

struct instance
{
	uintptr_t address = 0;
	size_t profile_index = 0;
	uintptr_t region_base = 0;
	size_t region_size = 0;
	uint32_t protection = 0;
	uint64_t last_seen_generation = 0;
};

inline table_state classify(const uint8_t *current, const uint8_t *baseline,
	const uint8_t *expected, size_t size)
{
	if (std::memcmp(current, baseline, size) == 0)
		return table_state::pristine;
	if (std::memcmp(current, expected, size) == 0)
		return table_state::our_override;
	return table_state::unknown;
}

inline size_t merge(std::vector<instance> &registry, const std::vector<instance> &found,
	uint64_t generation, size_t maximum)
{
	size_t added = 0;
	for (instance item : found)
	{
		auto existing = registry.begin();
		for (; existing != registry.end(); ++existing)
			if (existing->address == item.address &&
				existing->profile_index == item.profile_index)
				break;
		item.last_seen_generation = generation;
		if (existing != registry.end())
		{
			*existing = item;
			continue;
		}
		if (registry.size() == maximum)
			continue;
		registry.push_back(item);
		++added;
	}
	return added;
}
}
