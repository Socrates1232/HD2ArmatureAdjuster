#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace armature_probe
{
enum class profile_variant : uint32_t
{
	a,
	b
};

struct address_range
{
	uintptr_t begin = 0;
	uintptr_t end = 0;
};

struct profile_hit
{
	uintptr_t address = 0;
	profile_variant variant = profile_variant::a;
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
	uintptr_t base_address, const uint8_t *a, const uint8_t *b, uint32_t entries,
	uint32_t anchor_entry, const std::vector<address_range> &excluded, size_t max_hits)
{
	profile_scan_result result;
	constexpr size_t stride = 48;
	const size_t table_bytes = static_cast<size_t>(entries) * stride;
	if (data == nullptr || a == nullptr || b == nullptr || entries == 0 ||
		anchor_entry >= entries || size < table_bytes)
		return result;

	for (size_t offset = 0; offset + table_bytes <= size && result.hits.size() < max_hits;
		offset += sizeof(uint32_t))
	{
		const uintptr_t address = base_address + offset;
		if (std::any_of(excluded.begin(), excluded.end(), [address, table_bytes](const address_range &range) {
			return overlaps(address, table_bytes, range);
		}))
			continue;

		const uint8_t *candidate = data + offset;
		const size_t anchor = static_cast<size_t>(anchor_entry) * stride;
		const bool anchor_a = std::memcmp(candidate + anchor, a + anchor, stride) == 0;
		const bool anchor_b = std::memcmp(candidate + anchor, b + anchor, stride) == 0;
		if (!anchor_a && !anchor_b)
			continue;

		const bool full_a = anchor_a && std::memcmp(candidate, a, table_bytes) == 0;
		const bool full_b = anchor_b && std::memcmp(candidate, b, table_bytes) == 0;
		if (full_a || full_b)
		{
			result.hits.push_back({ address, full_b ? profile_variant::b : profile_variant::a });
			continue;
		}

		++result.partial_candidates;
		uint32_t matched = 0;
		const uint8_t *reference = anchor_b ? b : a;
		for (uint32_t entry = 0; entry < entries; ++entry)
			matched += std::memcmp(candidate + static_cast<size_t>(entry) * stride,
				reference + static_cast<size_t>(entry) * stride, stride) == 0;
		result.best_partial_entries = std::max(result.best_partial_entries, matched);
	}
	return result;
}
}
