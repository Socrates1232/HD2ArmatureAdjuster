#pragma once

#include <cstdint>

namespace shoulder_controller
{
enum class action
{
	none,
	start_scan,
	discard_and_request_scan,
	begin_edit,
	release_for_retry,
};

enum class refresh_action
{
	none,
	full_scan,
	priority_scan,
};

inline bool background_scan_allowed(bool pending)
{
	return !pending;
}

inline action next_action(bool desired, bool active, bool pending,
	bool needs_fresh_scan, uint32_t hunt_phase)
{
	if (!desired || active || !pending)
		return action::none;
	if (hunt_phase == 0)
		return action::start_scan;
	if (hunt_phase == 1)
		return action::none;
	if (needs_fresh_scan)
		return action::discard_and_request_scan;
	if (hunt_phase == 2)
		return action::begin_edit;
	if (hunt_phase == 3)
		return action::release_for_retry;
	return action::none;
}

inline refresh_action next_refresh(bool full_due, bool priority_due)
{
	if (full_due)
		return refresh_action::full_scan;
	if (priority_due)
		return refresh_action::priority_scan;
	return refresh_action::none;
}
}
