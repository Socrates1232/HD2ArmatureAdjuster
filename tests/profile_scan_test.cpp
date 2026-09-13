#include "ib_profile_data.hpp"
#include "profile_scan.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

namespace
{
using armature_probe::address_range;
using armature_probe::profile_variant;

bool finds(const std::vector<armature_probe::profile_hit> &hits, uintptr_t address,
	profile_variant variant)
{
	return std::any_of(hits.begin(), hits.end(), [address, variant](const auto &hit) {
		return hit.address == address && hit.variant == variant;
	});
}
}

int main()
{
	constexpr uintptr_t base = 0x10000000;
	constexpr size_t planted = 32 * 1024;
	constexpr size_t table_bytes = armature_ib_profile::t48_bytes;
	constexpr size_t anchor = static_cast<size_t>(armature_ib_profile::slot) * 48;
	std::vector<uint8_t> memory(64 * 1024, 0xCD);

	// Many anchor-only decoys must remain diagnostics and must not consume the hit cap.
	for (size_t offset = 0; offset < 16 * 1024; offset += 128)
		std::memcpy(memory.data() + offset + anchor,
			armature_ib_profile::a_t48.data() + anchor, 48);
	std::memcpy(memory.data() + planted, armature_ib_profile::a_t48.data(), table_bytes);
	auto result = armature_probe::find_exact_profiles(memory.data(), memory.size(), base,
		armature_ib_profile::a_t48.data(), armature_ib_profile::b_t48.data(),
		armature_ib_profile::count, armature_ib_profile::slot, {}, 1);
	if (!finds(result.hits, base + planted, profile_variant::a) || result.hits.size() != 1 ||
		result.partial_candidates == 0 || result.best_partial_entries == 0)
	{
		std::cerr << "full A match or partial-match separation failed\n";
		return 1;
	}

	std::memcpy(memory.data() + planted, armature_ib_profile::b_t48.data(), table_bytes);
	result = armature_probe::find_exact_profiles(memory.data(), memory.size(), base,
		armature_ib_profile::a_t48.data(), armature_ib_profile::b_t48.data(),
		armature_ib_profile::count, armature_ib_profile::slot, {}, 4);
	if (!finds(result.hits, base + planted, profile_variant::b))
	{
		std::cerr << "full B match failed\n";
		return 1;
	}

	const std::vector<address_range> mapped_exclusion {
		{ base + planted - 64, base + planted + table_bytes + 64 }
	};
	result = armature_probe::find_exact_profiles(memory.data(), memory.size(), base,
		armature_ib_profile::a_t48.data(), armature_ib_profile::b_t48.data(),
		armature_ib_profile::count, armature_ib_profile::slot, mapped_exclusion, 4);
	if (!result.hits.empty())
	{
		std::cerr << "mapped/upload exclusion failed\n";
		return 1;
	}

	const uintptr_t self = reinterpret_cast<uintptr_t>(armature_ib_profile::a_t48.data());
	const std::vector<address_range> self_exclusion { { self, self + table_bytes } };
	result = armature_probe::find_exact_profiles(armature_ib_profile::a_t48.data(), table_bytes,
		self, armature_ib_profile::a_t48.data(), armature_ib_profile::b_t48.data(),
		armature_ib_profile::count, armature_ib_profile::slot, self_exclusion, 4);
	if (!result.hits.empty())
	{
		std::cerr << "self-reference exclusion failed\n";
		return 1;
	}

	std::cout << "exact A/B profile matching and exclusions passed\n";
	return 0;
}
