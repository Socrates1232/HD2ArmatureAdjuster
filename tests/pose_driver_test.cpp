#include "pose_driver.hpp"

#include <cmath>
#include <iostream>

namespace
{
bool close(const armature_probe::affine_matrix &a,
	const armature_probe::affine_matrix &b, float tolerance = 2.0e-5f)
{
	for (size_t index = 0; index < a.size(); ++index)
		if (std::fabs(a[index] - b[index]) > tolerance)
			return false;
	return true;
}
}

int main()
{
	using namespace armature_probe;
	const float c = std::cos(0.7f), s = std::sin(0.7f);
	const affine_matrix world { c, s, 0, 0, -s, c, 0, 0, 0, 0, 1, 0,
		0.6f, -0.2f, 1.4f, 1 };
	affine_matrix baseline {};
	if (!affine_inverse(world, baseline))
		return 1;
	const affine_matrix pose { 0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0,
		0.9f, 0.1f, 1.2f, 1 };
	const affine_matrix native_skin = multiply(baseline, pose);
	const affine_matrix old_pre_offset = multiply(translation(0.04f, 0, 0), baseline);
	const affine_matrix observed = multiply(old_pre_offset, pose);
	const affine_matrix correction = translation(-0.03f, 0, 0);
	affine_matrix driven {};
	if (!drive_inverse_bind(baseline, old_pre_offset, observed, correction, driven))
		return 2;
	if (!close(multiply(driven, pose), multiply(native_skin, correction)))
	{
		std::cerr << "pose-aware output correction failed\n";
		return 3;
	}
	if (close(multiply(old_pre_offset, pose), multiply(native_skin, translation(0.04f, 0, 0))))
	{
		std::cerr << "static pre-offset negative control unexpectedly passed\n";
		return 4;
	}
	std::cout << "pose-aware inverse-bind driver identity passed\n";
	return 0;
}
