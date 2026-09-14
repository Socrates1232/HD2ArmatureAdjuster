#include "instance_lifecycle.hpp"

#include <array>
#include <iostream>
#include <vector>

int main()
{
	const std::array<uint8_t, 4> baseline { 1, 2, 3, 4 };
	const std::array<uint8_t, 4> expected { 1, 9, 3, 4 };
	const std::array<uint8_t, 4> unknown { 1, 8, 3, 4 };
	using instance_lifecycle::table_state;
	if (instance_lifecycle::classify(baseline.data(), baseline.data(), expected.data(),
		baseline.size()) != table_state::pristine ||
		instance_lifecycle::classify(expected.data(), baseline.data(), expected.data(),
		baseline.size()) != table_state::our_override ||
		instance_lifecycle::classify(unknown.data(), baseline.data(), expected.data(),
		baseline.size()) != table_state::unknown)
	{
		std::cerr << "table-state classification failed\n";
		return 1;
	}

	std::vector<instance_lifecycle::instance> registry {
		{ 0x1000, 3, 0x1000, 0x1000, 4, 1 },
	};
	std::vector<instance_lifecycle::instance> next {
		{ 0x1000, 3, 0x1000, 0x2000, 4, 0 },
		{ 0x3000, 3, 0x3000, 0x1000, 4, 0 },
	};
	if (instance_lifecycle::merge(registry, next, 2, 8) != 1 || registry.size() != 2 ||
		registry[0].last_seen_generation != 2 || registry[0].region_size != 0x2000 ||
		registry[1].address != 0x3000 || registry[1].last_seen_generation != 2)
	{
		std::cerr << "instance-registry merge failed\n";
		return 1;
	}

	std::cout << "instance classification and relocation registry passed\n";
	return 0;
}
