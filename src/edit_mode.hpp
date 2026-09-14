#pragma once

#include <cstdint>

namespace shoulder_edit
{
enum class edit_mode : uint32_t
{
	static_ib_offset,
	pose_control_from_source,
};

inline bool uses_pose_driver(edit_mode mode)
{
	return mode == edit_mode::pose_control_from_source;
}
}
