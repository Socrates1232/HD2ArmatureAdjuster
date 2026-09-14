#include "retarget/publisher.hpp"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

using namespace hd2aa::retarget;

namespace
{
int failures = 0;

void check(bool value, const char *message)
{
	if (!value)
	{
		++failures;
		std::cerr << "FAIL: " << message << '\n';
	}
}

struct memory
{
	uintptr_t base = 0x1000;
	std::vector<uint8_t> bytes;
	int fail_write = -1;
	int corrupt_readback = -1;
	int writes = 0;

	bool read(uintptr_t address, void *out, size_t size)
	{
		if (address < base || address - base > bytes.size() || size > bytes.size() - (address - base))
			return false;
		std::memcpy(out, bytes.data() + address - base, size);
		if (corrupt_readback == writes && size == 48)
			static_cast<uint8_t *>(out)[0] ^= 1;
		return true;
	}

	bool write(uintptr_t address, const void *source, size_t size)
	{
		++writes;
		if (writes == fail_write)
			return false;
		if (address < base || address - base > bytes.size() || size > bytes.size() - (address - base))
			return false;
		std::memcpy(bytes.data() + address - base, source, size);
		return true;
	}
};

publish_result run(memory &mem, publication &state, const std::vector<uint8_t> &next)
{
	return publish_table(mem.base, state, next, { 1, 2 }, 7,
		[&](uintptr_t at, void *out, size_t size) { return mem.read(at, out, size); },
		[&](uintptr_t at, const void *data, size_t size) { return mem.write(at, data, size); });
}
}

int main()
{
	std::vector<uint8_t> baseline(4 * 48);
	for (size_t index = 0; index < baseline.size(); ++index)
		baseline[index] = static_cast<uint8_t>(index);
	std::vector<uint8_t> next = baseline;
	next[48 + 3] ^= 0x55;
	next[96 + 7] ^= 0x33;

	publication state { baseline };
	memory good { 0x1000, baseline };
	const publish_result applied = run(good, state, next);
	check(applied.status == publish_status::applied && state.expected == next &&
		good.bytes == next && state.revision == 1 && state.source_sample_id == 7,
		"one owner publishes and commits one verified revision");
	const publish_result unchanged = run(good, state, next);
	check(unchanged.status == publish_status::no_change && state.revision == 1,
		"identical plans do not create fake revisions");

	publication write_state { baseline };
	memory write_failure { 0x1000, baseline, 2 };
	const publish_result write_result = run(write_failure, write_state, next);
	check(write_result.status == publish_status::write_failed_rolled_back &&
		write_failure.bytes == baseline && write_state.expected == baseline,
		"mid-transaction write failure restores the previous verified image");

	publication read_state { baseline };
	memory read_failure { 0x1000, baseline, -1, 1 };
	const publish_result read_result = run(read_failure, read_state, next);
	check(read_result.status == publish_status::readback_failed_rolled_back &&
		read_failure.bytes == baseline && read_state.expected == baseline,
		"readback mismatch restores the previous verified image");

	publication stale_state { baseline };
	memory stale { 0x1000, baseline };
	stale.bytes[0] ^= 1;
	const publish_result stale_result = run(stale, stale_state, next);
	check(stale_result.status == publish_status::stale_expected && stale.writes == 0,
		"unknown table ownership prevents every write");

	if (failures == 0)
		std::cout << "failure-atomic revisioned publisher passed\n";
	return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
