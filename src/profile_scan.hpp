#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace armature_probe
{
struct address_range
{
	uintptr_t begin = 0;
	uintptr_t end = 0;
};

struct profile_hit
{
	uintptr_t address = 0;
	size_t profile_index = 0;
};

struct profile_view
{
	const uint8_t *t48 = nullptr;
	uint32_t entries = 0;
};

struct profile_scan_result
{
	std::vector<profile_hit> hits;
	uint32_t partial_candidates = 0;
	uint32_t best_partial_entries = 0;
};

inline bool overlaps(uintptr_t begin, size_t size, const address_range &range)
{
	if (size > UINTPTR_MAX - begin)
		return true;
	return begin < range.end && begin + size > range.begin;
}

inline profile_scan_result find_exact_profiles(const uint8_t *data, size_t size,
	uintptr_t base_address, const std::vector<profile_view> &profiles,
	const std::vector<address_range> &excluded, size_t max_hits)
{
	profile_scan_result result;
	constexpr size_t stride = 48;
	if (data == nullptr || profiles.empty() || size < stride)
		return result;

	std::unordered_multimap<uint64_t, size_t> prefixes;
	for (size_t index = 0; index < profiles.size(); ++index)
		if (profiles[index].t48 != nullptr && profiles[index].entries != 0)
		{
			uint64_t prefix = 0;
			std::memcpy(&prefix, profiles[index].t48, sizeof(prefix));
			prefixes.emplace(prefix, index);
		}

	for (size_t offset = 0; offset + stride <= size && result.hits.size() < max_hits;
		offset += sizeof(uint32_t))
	{
		uint64_t prefix = 0;
		std::memcpy(&prefix, data + offset, sizeof(prefix));
		const auto matching = prefixes.equal_range(prefix);
		if (matching.first == matching.second)
			continue;
		const uintptr_t address = base_address + offset;
		for (auto it = matching.first; it != matching.second && result.hits.size() < max_hits; ++it)
		{
			const profile_view &profile = profiles[it->second];
			const size_t table_bytes = static_cast<size_t>(profile.entries) * stride;
			if (offset + table_bytes > size ||
				std::any_of(excluded.begin(), excluded.end(), [address, table_bytes](const address_range &range) {
					return overlaps(address, table_bytes, range);
				}))
				continue;
			const uint8_t *candidate = data + offset;
			if (std::memcmp(candidate, profile.t48, stride) != 0)
				continue;
			if (std::memcmp(candidate, profile.t48, table_bytes) == 0)
			{
				result.hits.push_back({ address, it->second });
				continue;
			}
			++result.partial_candidates;
			uint32_t matched = 0;
			for (uint32_t entry = 0; entry < profile.entries; ++entry)
				matched += std::memcmp(candidate + static_cast<size_t>(entry) * stride,
					profile.t48 + static_cast<size_t>(entry) * stride, stride) == 0;
			result.best_partial_entries = std::max(result.best_partial_entries, matched);
		}
	}
	return result;
}
}
