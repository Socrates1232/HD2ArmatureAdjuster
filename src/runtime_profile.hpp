#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace armature_profile
{
constexpr std::array<uint8_t, 8> magic { 'H', 'D', '2', 'I', 'B', 'P', '1', 0 };
constexpr uint32_t header_size = 64;
constexpr uint32_t record_header_size = 40;
constexpr uint32_t transform_stride = 48;

struct table
{
	uint64_t unit_id = 0;
	uint32_t lod_mask = 0;
	uint32_t entries = 0;
	uint32_t anchor_slot = 0;
	uint32_t first_lod = 0;
	uint64_t table_fnv1a = 0;
	std::vector<uint8_t> t48;
};

struct package
{
	std::string patch_name;
	std::array<uint8_t, 32> patch_sha256 {};
	std::vector<table> tables;
};

inline uint32_t read_u32(const uint8_t *data)
{
	uint32_t value = 0;
	std::memcpy(&value, data, sizeof(value));
	return value;
}

inline uint64_t read_u64(const uint8_t *data)
{
	uint64_t value = 0;
	std::memcpy(&value, data, sizeof(value));
	return value;
}

inline uint64_t fnv1a(const uint8_t *data, size_t size)
{
	uint64_t hash = 14695981039346656037ull;
	for (size_t index = 0; index < size; ++index)
		hash = (hash ^ data[index]) * 1099511628211ull;
	return hash;
}

inline bool parse(const uint8_t *data, size_t size, package &out, std::string &error)
{
	out = {};
	if (data == nullptr || size < header_size)
	{
		error = "profile header is truncated";
		return false;
	}
	if (std::memcmp(data, magic.data(), magic.size()) != 0)
	{
		error = "profile magic/version is unsupported";
		return false;
	}
	if (read_u32(data + 8) != header_size)
	{
		error = "profile header size is unsupported";
		return false;
	}
	const uint32_t record_count = read_u32(data + 12);
	const uint32_t name_size = read_u32(data + 16);
	if (record_count == 0 || record_count > 4096 || name_size == 0 || name_size > 1024)
	{
		error = "profile counts are outside supported bounds";
		return false;
	}
	if (header_size + static_cast<size_t>(name_size) > size)
	{
		error = "profile patch name is truncated";
		return false;
	}
	const uint64_t expected_payload_hash = read_u64(data + 56);
	if (fnv1a(data + header_size, size - header_size) != expected_payload_hash)
	{
		error = "profile payload checksum failed";
		return false;
	}
	std::memcpy(out.patch_sha256.data(), data + 24, out.patch_sha256.size());
	out.patch_name.assign(reinterpret_cast<const char *>(data + header_size), name_size);
	if (out.patch_name.find('\0') != std::string::npos)
	{
		error = "profile patch name contains a null byte";
		return false;
	}

	size_t cursor = header_size + name_size;
	for (uint32_t record = 0; record < record_count; ++record)
	{
		if (cursor > size || record_header_size > size - cursor)
		{
			error = "profile record header is truncated";
			return false;
		}
		table item;
		item.unit_id = read_u64(data + cursor);
		item.lod_mask = read_u32(data + cursor + 8);
		item.entries = read_u32(data + cursor + 12);
		item.anchor_slot = read_u32(data + cursor + 16);
		const uint32_t table_size = read_u32(data + cursor + 20);
		item.table_fnv1a = read_u64(data + cursor + 24);
		item.first_lod = read_u32(data + cursor + 32);
		cursor += record_header_size;
		if (item.unit_id == 0 || item.lod_mask == 0 || item.entries == 0 ||
			item.entries > 4096 || item.anchor_slot >= item.entries ||
			table_size != static_cast<uint64_t>(item.entries) * transform_stride)
		{
			error = "profile record fields are invalid";
			return false;
		}
		if (cursor > size || table_size > size - cursor)
		{
			error = "profile transform table is truncated";
			return false;
		}
		if (fnv1a(data + cursor, table_size) != item.table_fnv1a)
		{
			error = "profile transform checksum failed";
			return false;
		}
		item.t48.assign(data + cursor, data + cursor + table_size);
		cursor += table_size;
		out.tables.push_back(std::move(item));
	}
	if (cursor != size)
	{
		error = "profile has trailing data";
		return false;
	}
	return true;
}
}
