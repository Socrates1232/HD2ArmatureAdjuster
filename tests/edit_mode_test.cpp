#include "edit_mode.hpp"

#include <iostream>

int main()
{
	using shoulder_edit::edit_mode;
	if (shoulder_edit::uses_pose_driver(edit_mode::static_ib_offset) ||
		!shoulder_edit::uses_pose_driver(edit_mode::pose_control_from_source))
	{
		std::cerr << "request mode did not exclusively select its writer\n";
		return 1;
	}
	std::cout << "explicit edit-mode routing passed\n";
	return 0;
}
