#include "runtime_profile.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

namespace
{
void append_u32(std::vector<uint8_t> &data, uint32_t value)
{
	const auto *bytes = reinterpret_cast<const uint8_t *>(&value);
	data.insert(data.end(), bytes, bytes + sizeof(value));
}

void append_u64(std::vector<uint8_t> &data, uint64_t value)
{
	const auto *bytes = reinterpret_cast<const uint8_t *>(&value);
	data.insert(data.end(), bytes, bytes + sizeof(value));
}
}

int main()
{
	const std::string patch_name = "example.patch_3";
	std::vector<uint8_t> table(3 * armature_profile::transform_stride);
	for (size_t index = 0; index < table.size(); ++index)
		table[index] = static_cast<uint8_t>((index * 13 + 7) & 0xFF);
	std::vector<uint8_t> payload(patch_name.begin(), patch_name.end());
	append_u64(payload, 0x0123456789ABCDEFull);
	append_u32(payload, 0x0F);
	append_u32(payload, 3);
	append_u32(payload, 0);
	append_u32(payload, static_cast<uint32_t>(table.size()));
	append_u64(payload, armature_profile::fnv1a(table.data(), table.size()));
	append_u32(payload, 0);
	append_u32(payload, 0);
	payload.insert(payload.end(), table.begin(), table.end());

	std::vector<uint8_t> file(armature_profile::header_size, 0);
	std::memcpy(file.data(), armature_profile::magic.data(), armature_profile::magic.size());
	const auto set_u32 = [&file](size_t offset, uint32_t value) {
		std::memcpy(file.data() + offset, &value, sizeof(value));
	};
	const auto set_u64 = [&file](size_t offset, uint64_t value) {
		std::memcpy(file.data() + offset, &value, sizeof(value));
	};
	set_u32(8, armature_profile::header_size);
	set_u32(12, 1);
	set_u32(16, static_cast<uint32_t>(patch_name.size()));
	std::fill(file.begin() + 24, file.begin() + 56, 0xA5);
	set_u64(56, armature_profile::fnv1a(payload.data(), payload.size()));
	file.insert(file.end(), payload.begin(), payload.end());

	armature_profile::package parsed;
	std::string error;
	if (!armature_profile::parse(file.data(), file.size(), parsed, error) ||
		parsed.patch_name != patch_name || parsed.tables.size() != 1 ||
		parsed.tables[0].unit_id != 0x0123456789ABCDEFull ||
		parsed.tables[0].lod_mask != 0x0F || parsed.tables[0].entries != 3 ||
		parsed.tables[0].t48 != table)
	{
		std::cerr << "valid runtime profile failed: " << error << '\n';
		return 1;
	}
	file.back() ^= 1;
	if (armature_profile::parse(file.data(), file.size(), parsed, error))
	{
		std::cerr << "corrupt runtime profile was accepted\n";
		return 1;
	}
	std::cout << "runtime profile parsing and checksum validation passed\n";
	return 0;
}
