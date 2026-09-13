#include "ib_layout.hpp"
#include "ib_profile_data.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>

namespace
{
using armature_probe::affine_matrix;

affine_matrix multiply(const affine_matrix &a, const affine_matrix &b)
{
	affine_matrix out {};
	for (size_t row = 0; row < 4; ++row)
		for (size_t column = 0; column < 4; ++column)
			for (size_t k = 0; k < 4; ++k)
				out[row * 4 + column] += a[row * 4 + k] * b[k * 4 + column];
	return out;
}

affine_matrix inverse_affine(const affine_matrix &m)
{
	const float a = m[0], b = m[1], c = m[2];
	const float d = m[4], e = m[5], f = m[6];
	const float g = m[8], h = m[9], i = m[10];
	const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
	if (std::fabs(det) < 1.0e-8f)
		return {};
	const float s = 1.0f / det;
	affine_matrix out {
		(e * i - f * h) * s, (c * h - b * i) * s, (b * f - c * e) * s, 0.0f,
		(f * g - d * i) * s, (a * i - c * g) * s, (c * d - a * f) * s, 0.0f,
		(d * h - e * g) * s, (b * g - a * h) * s, (a * e - b * d) * s, 0.0f,
		0.0f, 0.0f, 0.0f, 1.0f
	};
	for (size_t column = 0; column < 3; ++column)
		out[12 + column] = -(m[12] * out[column] + m[13] * out[4 + column] +
			m[14] * out[8 + column]);
	return out;
}

bool close(const affine_matrix &a, const affine_matrix &b, float tolerance = 1.0e-5f)
{
	for (size_t i = 0; i < a.size(); ++i)
		if (std::fabs(a[i] - b[i]) > tolerance)
			return false;
	return true;
}
}

int main()
{
	const affine_matrix source {
		2.0f, 0.25f, -0.5f, 0.0f,
		0.75f, 3.0f, 0.125f, 0.0f,
		-0.25f, 0.5f, 4.0f, 0.0f,
		1.25f, -2.5f, 3.75f, 1.0f
	};
	const armature_probe::t48_matrix expected {
		2.0f, 0.75f, -0.25f, 1.25f,
		0.25f, 3.0f, 0.5f, -2.5f,
		-0.5f, 0.125f, 4.0f, 3.75f
	};

	std::array<uint8_t, 64> file_bytes {};
	armature_probe::encode_file64(source, file_bytes.data());
	if (armature_probe::decode_file64(file_bytes.data()) != source)
	{
		std::cerr << "file64 round trip failed\n";
		return 1;
	}

	std::array<uint8_t, 48> packed_bytes {};
	armature_probe::file64_to_t48(file_bytes.data(), packed_bytes.data());
	if (std::memcmp(packed_bytes.data(), expected.data(), packed_bytes.size()) != 0)
	{
		std::cerr << "file64 to t48 layout failed\n";
		return 1;
	}
	if (armature_probe::decode_t48(packed_bytes.data()) != source)
	{
		std::cerr << "t48 round trip failed\n";
		return 1;
	}

	std::array<uint8_t, 48> profile_a {};
	std::array<uint8_t, 48> profile_b {};
	armature_probe::file64_to_t48(armature_ib_profile::a_file64_slot.data(), profile_a.data());
	armature_probe::file64_to_t48(armature_ib_profile::b_file64_slot.data(), profile_b.data());
	const size_t profile_offset = static_cast<size_t>(armature_ib_profile::slot) * 48;
	if (std::memcmp(profile_a.data(), armature_ib_profile::a_t48.data() + profile_offset,
		profile_a.size()) != 0 ||
		std::memcmp(profile_b.data(), armature_ib_profile::b_t48.data() + profile_offset,
		profile_b.size()) != 0)
	{
		std::cerr << "generated A/B profile conversion failed\n";
		return 1;
	}
	const auto a_profile_matrix = armature_probe::decode_t48(profile_a.data());
	const auto b_profile_matrix = armature_probe::decode_t48(profile_b.data());
	for (size_t index = 0; index < 16; ++index)
		if (index != 12 && index != 13 && index != 14 &&
			a_profile_matrix[index] != b_profile_matrix[index])
		{
			std::cerr << "A/B profile changed a non-translation component\n";
			return 1;
		}
	if (a_profile_matrix[12] == b_profile_matrix[12] &&
		a_profile_matrix[13] == b_profile_matrix[13] &&
		a_profile_matrix[14] == b_profile_matrix[14])
	{
		std::cerr << "A/B profile has no translation change\n";
		return 1;
	}

	const affine_matrix world {
		0.0f, -2.0f, 0.0f, 0.0f,
		3.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 0.5f, 0.0f,
		1.0f, -2.0f, 3.0f, 1.0f
	};
	const affine_matrix translation {
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.17f, -0.23f, 0.31f, 1.0f
	};
	const affine_matrix inverse_bind = inverse_affine(world);
	const affine_matrix skin = multiply(multiply(translation, inverse_bind), world);
	if (!close(skin, translation))
	{
		std::cerr << "(T * IB) * W identity failed with rotation and nonuniform scale\n";
		return 1;
	}

	std::cout << "file64/t48 conversion and scaled affine identity passed\n";
	return 0;
}
