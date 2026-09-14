#include "guarded_slot_writer.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace
{
constexpr size_t stride = 4;
const std::array<uint8_t, 8> source { 1, 2, 3, 4, 5, 6, 7, 8 };
const std::array<uint8_t, 8> destination { 9, 9, 9, 9, 8, 8, 8, 8 };

void require(bool condition, const char *message)
{
	if (!condition)
	{
		std::cerr << message << '\n';
		std::exit(1);
	}
}

auto reader(std::array<uint8_t, 8> &memory)
{
	return [&memory](uint32_t slot, uint8_t *output, size_t size) {
		std::copy_n(memory.data() + static_cast<size_t>(slot) * stride, size, output);
		return true;
	};
}
}

int main()
{
	std::array<uint8_t, 8> restored_memory = source;
	uint32_t writes = 0;
	auto fail_second = [&restored_memory, &writes](uint32_t slot, const uint8_t *data, size_t size) {
		++writes;
		if (writes == 2)
			return false;
		std::copy_n(data, size, restored_memory.data() + static_cast<size_t>(slot) * stride);
		return true;
	};
	auto restored = guarded_slot_writer::apply(std::vector<uint32_t> { 0, 1 }, stride,
		source.data(), destination.data(), reader(restored_memory), fail_second,
		[&restored_memory]() { return restored_memory == destination; });
	require(restored.state == guarded_slot_writer::status::write_failed_restored,
		"a cleanly recoverable write failure must report restored");
	require(restored_memory == source,
		"earlier successful slots must be restored after a later write failure");
	require(restored.dirty_slots.empty(), "a verified rollback must not report dirty slots");

	std::array<uint8_t, 8> dirty_memory = source;
	writes = 0;
	auto partial_second = [&dirty_memory, &writes](uint32_t slot, const uint8_t *data, size_t size) {
		++writes;
		if (writes == 2)
		{
			dirty_memory[slot * stride] = data[0];
			return false;
		}
		std::copy_n(data, size, dirty_memory.data() + static_cast<size_t>(slot) * stride);
		return true;
	};
	auto dirty = guarded_slot_writer::apply(std::vector<uint32_t> { 0, 1 }, stride,
		source.data(), destination.data(), reader(dirty_memory), partial_second,
		[&dirty_memory]() { return dirty_memory == destination; });
	require(dirty.state == guarded_slot_writer::status::dirty,
		"an unrecognized partial slot write must remain explicitly dirty");
	require(dirty_memory[0] == source[0] && dirty_memory[4] == destination[4],
		"known earlier writes must roll back without overwriting an unknown partial slot");
	require(dirty.dirty_slots == std::vector<uint32_t> { 1 },
		"the unresolved partial slot must remain tracked");

	std::cout << "guarded multi-slot write rollback passed\n";
	return 0;
}
