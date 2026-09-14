#pragma once

#include <atomic>
#include <cstdint>

namespace hd2aa::retarget
{
struct custom_intent_snapshot
{
	bool desired = false;
	uint64_t revision = 0;
};

class custom_intent_mailbox
{
public:
	custom_intent_snapshot load() const
	{
		const uint64_t state = _state.load(std::memory_order_acquire);
		return { (state & 1) != 0, state >> 1 };
	}

	bool toggle()
	{
		uint64_t current = _state.load(std::memory_order_relaxed);
		for (;;)
		{
			const bool desired = (current & 1) == 0;
			const uint64_t next = (((current >> 1) + 1) << 1) | (desired ? 1u : 0u);
			if (_state.compare_exchange_weak(current, next,
				std::memory_order_release, std::memory_order_relaxed))
				return desired;
		}
	}

	void request(bool desired)
	{
		uint64_t current = _state.load(std::memory_order_relaxed);
		while ((current & 1) != (desired ? 1u : 0u))
		{
			const uint64_t next = (((current >> 1) + 1) << 1) | (desired ? 1u : 0u);
			if (_state.compare_exchange_weak(current, next,
				std::memory_order_release, std::memory_order_relaxed))
				return;
		}
	}

	void reset() { _state.store(0, std::memory_order_release); }

private:
	std::atomic<uint64_t> _state { 0 };
};
}
