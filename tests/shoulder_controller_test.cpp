#include "shoulder_controller.hpp"

#include <cstdlib>
#include <iostream>

namespace
{
struct controller_model
{
	uint32_t phase = 2;
	bool desired = false;
	bool active = false;
	bool pending = false;
	bool needs_fresh_scan = false;
	uint32_t scan_ticks = 0;
	uint32_t scan_starts = 0;
	uint32_t edit_calls = 0;
	int32_t retry_ticks = -1;
	bool target_available = true;

	void enable()
	{
		desired = true;
		pending = true;
		needs_fresh_scan = true;
		phase = 0;
	}

	void complete_scan()
	{
		if (phase == 1 && ++scan_ticks >= 2)
			phase = target_available ? 2u : 3u;
	}

	void present()
	{
		complete_scan();
		if (desired && !active && !pending && phase == 3 && retry_ticks < 0)
			retry_ticks = 2;
		if (retry_ticks > 0)
			--retry_ticks;
		if (retry_ticks == 0 && shoulder_controller::background_scan_allowed(pending))
		{
			phase = 0;
			retry_ticks = -1;
		}
		if (pending && phase > 1 && shoulder_controller::background_scan_allowed(pending))
			phase = 0;
		if (shoulder_controller::background_scan_allowed(pending) && phase == 0)
			start_scan();
		switch (shoulder_controller::next_action(
			desired, active, pending, needs_fresh_scan, phase))
		{
		case shoulder_controller::action::start_scan:
			if (start_scan())
				needs_fresh_scan = false;
			break;
		case shoulder_controller::action::discard_and_request_scan:
			phase = 0;
			break;
		case shoulder_controller::action::begin_edit:
			++edit_calls;
			active = true;
			pending = false;
			break;
		case shoulder_controller::action::release_for_retry:
			pending = false;
			needs_fresh_scan = false;
			retry_ticks = 2;
			break;
		case shoulder_controller::action::none:
			break;
		}
		if (desired && !active && !pending && phase == 2)
		{
			++edit_calls;
			active = true;
		}
	}

	bool start_scan()
	{
		phase = 1;
		scan_ticks = 0;
		++scan_starts;
		return true;
	}
};

void require(bool condition, const char *message)
{
	if (!condition)
	{
		std::cerr << message << '\n';
		std::exit(1);
	}
}
}

int main()
{
	controller_model model;
	model.enable();
	for (uint32_t frame = 0; frame < 20; ++frame)
		model.present();
	require(model.scan_starts == 1, "one enable must start exactly one fresh scan");
	require(model.edit_calls == 1, "successful fresh discovery must reach the editor");
	require(model.active, "successful edit must become active");
	require(!model.pending, "successful edit must clear the pending request");
	require(!model.needs_fresh_scan, "the shoulder-owned scan must acknowledge freshness");
	require(!shoulder_controller::background_scan_allowed(true),
		"background scheduling must not discard a pending shoulder result");

	controller_model in_flight;
	in_flight.phase = 1;
	in_flight.desired = true;
	in_flight.pending = true;
	in_flight.needs_fresh_scan = true;
	for (uint32_t frame = 0; frame < 20; ++frame)
		in_flight.present();
	require(in_flight.scan_starts == 1,
		"an existing scan must be discarded before one shoulder-owned scan starts");
	require(in_flight.edit_calls == 1,
		"enable during an existing scan must eventually reach the editor");

	controller_model delayed_target;
	delayed_target.target_available = false;
	delayed_target.enable();
	for (uint32_t frame = 0; frame < 10; ++frame)
		delayed_target.present();
	require(delayed_target.scan_starts >= 2,
		"an empty fresh scan must schedule a bounded retry");
	delayed_target.target_available = true;
	for (uint32_t frame = 0; frame < 20; ++frame)
		delayed_target.present();
	require(delayed_target.edit_calls == 1 && delayed_target.active,
		"a target appearing after an empty scan must still reach the editor");

	require(shoulder_controller::next_refresh(true, true) ==
		shoulder_controller::refresh_action::full_scan,
		"an overdue full scan must take priority over a priority-region scan");

	std::cout << "shoulder controller scan ownership passed\n";
	return 0;
}
