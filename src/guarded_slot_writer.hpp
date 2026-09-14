#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace guarded_slot_writer
{
enum class status
{
	applied,
	write_failed_restored,
	readback_failed_restored,
	dirty,
};

struct result
{
	status state = status::dirty;
	size_t forward_attempts = 0;
	size_t forward_successes = 0;
	std::vector<uint32_t> dirty_slots;
};

template <typename ReadSlot, typename WriteSlot, typename VerifyTable>
result apply(const std::vector<uint32_t> &slots, size_t stride,
	const uint8_t *source, const uint8_t *destination,
	ReadSlot read_slot, WriteSlot write_slot, VerifyTable verify_table)
{
	result output;
	std::vector<uint32_t> touched;
	auto rollback = [&](status restored_status) {
		std::vector<uint8_t> current(stride);
		for (auto item = touched.rbegin(); item != touched.rend(); ++item)
		{
			const uint32_t slot = *item;
			const uint8_t *original = source + static_cast<size_t>(slot) * stride;
			const uint8_t *attempted = destination + static_cast<size_t>(slot) * stride;
			if (!read_slot(slot, current.data(), stride))
			{
				output.dirty_slots.push_back(slot);
				continue;
			}
			if (std::memcmp(current.data(), original, stride) == 0)
				continue;
			if (std::memcmp(current.data(), attempted, stride) != 0 ||
				!write_slot(slot, original, stride) ||
				!read_slot(slot, current.data(), stride) ||
				std::memcmp(current.data(), original, stride) != 0)
				output.dirty_slots.push_back(slot);
		}
		output.state = output.dirty_slots.empty() ? restored_status : status::dirty;
		return output;
	};

	for (uint32_t slot : slots)
	{
		touched.push_back(slot);
		++output.forward_attempts;
		const uint8_t *wanted = destination + static_cast<size_t>(slot) * stride;
		if (!write_slot(slot, wanted, stride))
			return rollback(status::write_failed_restored);
		++output.forward_successes;
	}
	if (!verify_table())
		return rollback(status::readback_failed_restored);
	output.state = status::applied;
	return output;
}
}
