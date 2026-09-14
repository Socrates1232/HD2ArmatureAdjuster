#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace hd2aa::retarget
{
enum class publish_status
{
	applied,
	no_change,
	stale_expected,
	write_failed_rolled_back,
	readback_failed_rolled_back,
	dirty_unknown,
	invalid_plan,
};

struct publication
{
	std::vector<uint8_t> expected;
	uint64_t revision = 0;
	uint64_t source_sample_id = 0;
};

struct publish_result
{
	publish_status status = publish_status::invalid_plan;
	size_t attempted_slots = 0;
	size_t completed_slots = 0;
};

template <typename Read, typename Write>
publish_result publish_table(uintptr_t address, publication &state,
	const std::vector<uint8_t> &next, const std::vector<size_t> &slots,
	uint64_t source_sample_id, Read read, Write write)
{
	publish_result result;
	if (address == 0 || state.expected.empty() || state.expected.size() != next.size() ||
		state.expected.size() % 48 != 0)
		return result;
	std::vector<size_t> ordered = slots;
	std::sort(ordered.begin(), ordered.end());
	if (std::adjacent_find(ordered.begin(), ordered.end()) != ordered.end() ||
		(!ordered.empty() && ordered.back() >= state.expected.size() / 48))
		return result;
	std::vector<uint8_t> current(state.expected.size());
	if (!read(address, current.data(), current.size()) || current != state.expected)
	{
		result.status = publish_status::stale_expected;
		return result;
	}
	ordered.erase(std::remove_if(ordered.begin(), ordered.end(), [&](size_t slot) {
		return std::memcmp(state.expected.data() + slot * 48, next.data() + slot * 48, 48) == 0;
	}), ordered.end());
	if (ordered.empty())
	{
		result.status = publish_status::no_change;
		state.source_sample_id = source_sample_id;
		return result;
	}

	std::vector<size_t> attempted;
	std::array<uint8_t, 48> readback {};
	publish_status failure = publish_status::applied;
	for (size_t slot : ordered)
	{
		attempted.push_back(slot);
		++result.attempted_slots;
		if (!write(address + slot * 48, next.data() + slot * 48, 48))
		{
			failure = publish_status::write_failed_rolled_back;
			break;
		}
		if (!read(address + slot * 48, readback.data(), readback.size()) ||
			std::memcmp(readback.data(), next.data() + slot * 48, 48) != 0)
		{
			failure = publish_status::readback_failed_rolled_back;
			break;
		}
		++result.completed_slots;
	}
	if (failure != publish_status::applied)
	{
		bool rolled_back = true;
		for (auto item = attempted.rbegin(); item != attempted.rend(); ++item)
			rolled_back = write(address + *item * 48,
				state.expected.data() + *item * 48, 48) && rolled_back;
		rolled_back = read(address, current.data(), current.size()) &&
			current == state.expected && rolled_back;
		result.status = rolled_back ? failure : publish_status::dirty_unknown;
		return result;
	}
	state.expected = next;
	state.source_sample_id = source_sample_id;
	++state.revision;
	result.status = publish_status::applied;
	return result;
}
}
