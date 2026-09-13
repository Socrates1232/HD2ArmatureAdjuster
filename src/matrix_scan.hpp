#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace armature_probe
{
struct format_score
{
	float confidence = 0.0f;
	uint32_t layout_a = 0;
	uint32_t layout_b = 0;
};

struct matrix_run
{
	uint32_t stride = 0;
	uint32_t elements = 0;
	uint32_t non_identity = 0;
	size_t offset = 0;
};

inline float load_float(const uint8_t *p, size_t index)
{
	float value = 0.0f;
	std::memcpy(&value, p + index * sizeof(float), sizeof(value));
	return value;
}

inline bool finite_value(float value)
{
	return std::isfinite(value) && std::fabs(value) < 1.0e6f;
}

inline bool near_value(float value, float wanted, float tolerance = 1.0e-4f)
{
	return std::fabs(value - wanted) < tolerance;
}

inline bool finite_block(const uint8_t *p, size_t float_count)
{
	for (size_t i = 0; i < float_count; ++i)
		if (!finite_value(load_float(p, i)))
			return false;
	return true;
}

inline bool basis_like(const uint8_t *p,
	size_t a0, size_t a1, size_t a2,
	size_t b0, size_t b1, size_t b2,
	size_t c0, size_t c1, size_t c2)
{
	const float ax = load_float(p, a0), ay = load_float(p, a1), az = load_float(p, a2);
	const float bx = load_float(p, b0), by = load_float(p, b1), bz = load_float(p, b2);
	const float cx = load_float(p, c0), cy = load_float(p, c1), cz = load_float(p, c2);
	const float aa = ax * ax + ay * ay + az * az;
	const float bb = bx * bx + by * by + bz * bz;
	const float cc = cx * cx + cy * cy + cz * cz;
	if (!(aa > 1.0e-6f && bb > 1.0e-6f && cc > 1.0e-6f &&
		aa < 1.0e6f && bb < 1.0e6f && cc < 1.0e6f))
		return false;
	const float smallest = std::min({ aa, bb, cc });
	const float largest = std::max({ aa, bb, cc });
	if (largest > smallest * 4.0f)
		return false;
	constexpr float max_cosine_squared = 0.0025f;
	const float ab = ax * bx + ay * by + az * bz;
	const float ac = ax * cx + ay * cy + az * cz;
	const float bc = bx * cx + by * cy + bz * cz;
	return ab * ab <= max_cosine_squared * aa * bb &&
		ac * ac <= max_cosine_squared * aa * cc &&
		bc * bc <= max_cosine_squared * bb * cc;
}

inline bool transform_like(const uint8_t *p, uint32_t stride)
{
	if (stride == 64)
	{
		if (!finite_block(p, 16) || !basis_like(p, 0, 1, 2, 4, 5, 6, 8, 9, 10))
			return false;
		const bool row_affine = near_value(load_float(p, 3), 0) && near_value(load_float(p, 7), 0) &&
			near_value(load_float(p, 11), 0) && near_value(load_float(p, 15), 1);
		const bool column_affine = near_value(load_float(p, 12), 0) && near_value(load_float(p, 13), 0) &&
			near_value(load_float(p, 14), 0) && near_value(load_float(p, 15), 1);
		return row_affine || column_affine;
	}
	if (stride == 48)
	{
		return finite_block(p, 12) &&
			(basis_like(p, 0, 1, 2, 4, 5, 6, 8, 9, 10) ||
			 basis_like(p, 0, 1, 2, 3, 4, 5, 6, 7, 8));
	}
	if (stride == 32)
	{
		if (!finite_block(p, 8))
			return false;
		const float x = load_float(p, 0), y = load_float(p, 1);
		const float z = load_float(p, 2), w = load_float(p, 3);
		const float dx = load_float(p, 4), dy = load_float(p, 5);
		const float dz = load_float(p, 6), dw = load_float(p, 7);
		const float length = x * x + y * y + z * z + w * w;
		const float orthogonal = x * dx + y * dy + z * dz + w * dw;
		return std::fabs(length - 1.0f) < 0.02f && std::fabs(orthogonal) < 0.002f;
	}
	return false;
}

inline bool identity_like(const uint8_t *p, uint32_t stride)
{
	static constexpr float matrix4[16] =
		{ 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
	static constexpr float packed_a[12] =
		{ 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
	static constexpr float packed_b[12] =
		{ 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 };
	static constexpr float dual_quaternion[8] = { 0, 0, 0, 1, 0, 0, 0, 0 };
	const float *first = stride == 64 ? matrix4 : stride == 48 ? packed_a : dual_quaternion;
	const float *second = stride == 48 ? packed_b : nullptr;
	const size_t count = stride / sizeof(float);
	bool match_first = true, match_second = second != nullptr;
	for (size_t i = 0; i < count; ++i)
	{
		match_first = match_first && near_value(load_float(p, i), first[i]);
		if (second != nullptr)
			match_second = match_second && near_value(load_float(p, i), second[i]);
	}
	return match_first || match_second;
}

inline matrix_run find_longest_run(const uint8_t *data, size_t size)
{
	matrix_run best;
	size_t best_bytes = 0;
	for (uint32_t stride : { 64u, 48u, 32u })
	{
		for (uint32_t phase = 0; phase < stride; phase += 16)
		{
			size_t start = phase;
			uint32_t length = 0, non_identity = 0;
			for (size_t offset = phase; offset + stride <= size; offset += stride)
			{
				if (!transform_like(data + offset, stride))
				{
					length = 0;
					non_identity = 0;
					continue;
				}
				if (length == 0)
					start = offset;
				++length;
				if (!identity_like(data + offset, stride))
					++non_identity;
				const size_t bytes = static_cast<size_t>(length) * stride;
				if (bytes > best_bytes)
				{
					best_bytes = bytes;
					best = { stride, length, non_identity, start };
				}
			}
		}
	}
	return best;
}

inline std::vector<matrix_run> find_runs(const uint8_t *data, size_t size,
	uint32_t stride, uint32_t min_elements)
{
	std::vector<matrix_run> runs;
	for (uint32_t phase = 0; phase < stride; phase += 16)
	{
		size_t start = phase;
		uint32_t length = 0, non_identity = 0;
		auto finish = [&]() {
			if (length >= min_elements && non_identity != 0)
				runs.push_back({ stride, length, non_identity, start });
			length = 0;
			non_identity = 0;
		};
		for (size_t offset = phase; offset + stride <= size; offset += stride)
		{
			if (!transform_like(data + offset, stride))
			{
				finish();
				continue;
			}
			if (length == 0)
				start = offset;
			++length;
			if (!identity_like(data + offset, stride))
				++non_identity;
		}
		finish();
	}
	return runs;
}

inline format_score assess(const uint8_t *data, size_t size, uint32_t stride)
{
	format_score out;
	const uint32_t count = static_cast<uint32_t>(size / stride);
	if (count == 0)
		return out;
	uint32_t valid = 0;
	for (uint32_t i = 0; i < count; ++i)
	{
		const uint8_t *p = data + static_cast<size_t>(i) * stride;
		valid += transform_like(p, stride);
		if (stride == 64)
		{
			out.layout_a += near_value(load_float(p, 3), 0) && near_value(load_float(p, 7), 0) && near_value(load_float(p, 11), 0);
			out.layout_b += near_value(load_float(p, 12), 0) && near_value(load_float(p, 13), 0) && near_value(load_float(p, 14), 0);
		}
		else if (stride == 48)
		{
			out.layout_a += basis_like(p, 0, 1, 2, 4, 5, 6, 8, 9, 10);
			out.layout_b += basis_like(p, 0, 1, 2, 3, 4, 5, 6, 7, 8);
		}
	}
	out.confidence = static_cast<float>(valid) / static_cast<float>(count);
	return out;
}
}
