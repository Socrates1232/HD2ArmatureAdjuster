#include "profile_scan.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

namespace
{
using armature_probe::address_range;
bool finds(const std::vector<armature_probe::profile_hit> &hits, uintptr_t address,
	size_t profile_index)
{
	return std::any_of(hits.begin(), hits.end(), [address, profile_index](const auto &hit) {
		return hit.address == address && hit.profile_index == profile_index;
	});
}
}

int main()
{
	constexpr uintptr_t base = 0x10000000;
	constexpr size_t planted = 32 * 1024;
	constexpr uint32_t entries = 88;
	constexpr size_t table_bytes = entries * 48;
	std::vector<uint8_t> memory(64 * 1024, 0xCD);
	std::vector<uint8_t> profile_a(table_bytes), profile_b(table_bytes);
	for (size_t index = 0; index < table_bytes; ++index)
	{
		profile_a[index] = static_cast<uint8_t>((index * 17 + 3) & 0xFF);
		profile_b[index] = static_cast<uint8_t>((index * 29 + 11) & 0xFF);
	}
	const std::vector<armature_probe::profile_view> profiles {
		{ profile_a.data(), entries }, { profile_b.data(), entries }
	};

	// Many first-slot decoys must remain diagnostics and must not consume the hit cap.
	for (size_t offset = 0; offset < 16 * 1024; offset += 128)
		std::memcpy(memory.data() + offset, profile_a.data(), 48);
	std::memcpy(memory.data() + planted, profile_a.data(), table_bytes);
	auto result = armature_probe::find_exact_profiles(memory.data(), memory.size(), base,
		profiles, {}, 1);
	if (!finds(result.hits, base + planted, 0) || result.hits.size() != 1 ||
		result.partial_candidates == 0 || result.best_partial_entries == 0)
	{
		std::cerr << "full A match or partial-match separation failed\n";
		return 1;
	}

	std::memcpy(memory.data() + planted, profile_b.data(), table_bytes);
	result = armature_probe::find_exact_profiles(memory.data(), memory.size(), base,
		profiles, {}, 4);
	if (!finds(result.hits, base + planted, 1))
	{
		std::cerr << "full B match failed\n";
		return 1;
	}

	const std::vector<address_range> mapped_exclusion {
		{ base + planted - 64, base + planted + table_bytes + 64 }
	};
	result = armature_probe::find_exact_profiles(memory.data(), memory.size(), base,
		profiles, mapped_exclusion, 4);
	if (!result.hits.empty())
	{
		std::cerr << "mapped/upload exclusion failed\n";
		return 1;
	}

	const uintptr_t self = reinterpret_cast<uintptr_t>(profile_a.data());
	const std::vector<address_range> self_exclusion { { self, self + table_bytes } };
	result = armature_probe::find_exact_profiles(profile_a.data(), table_bytes,
		self, profiles, self_exclusion, 4);
	if (!result.hits.empty())
	{
		std::cerr << "self-reference exclusion failed\n";
		return 1;
	}

	std::cout << "dynamic profile matching and exclusions passed\n";
	return 0;
}
