#pragma once

#include "evaluator.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace hd2aa::retarget
{
enum class plan_error
{
	none,
	invalid_table_size,
	slot_out_of_range,
	duplicate_slot,
	math_failure,
	float32_overflow
};

struct translation_slot_request
{
	size_t slot = 0;
	linear3 observed_skin_linear {};
	vector3 displacement {};
};

struct table_plan
{
	plan_error failure = plan_error::none;
	size_t failed_request = std::numeric_limits<size_t>::max();
	std::vector<uint8_t> bytes;
	std::vector<size_t> changed_slots;
};

inline table_plan build_translation_plan(const std::vector<uint8_t> &pristine,
	size_t entries, const std::vector<translation_slot_request> &requests)
{
	table_plan result;
	result.bytes = pristine;
	if (entries > std::numeric_limits<size_t>::max() / 48 || pristine.size() != entries * 48)
	{
		result.failure = plan_error::invalid_table_size;
		return result;
	}
	std::vector<bool> seen(entries, false);
	std::vector<uint8_t> candidate = pristine;
	for (size_t request_index = 0; request_index < requests.size(); ++request_index)
	{
		const auto &request = requests[request_index];
		if (request.slot >= entries)
		{
			result.failure = plan_error::slot_out_of_range;
			result.failed_request = request_index;
			return result;
		}
		if (seen[request.slot])
		{
			result.failure = plan_error::duplicate_slot;
			result.failed_request = request_index;
			return result;
		}
		seen[request.slot] = true;
		const size_t offset = request.slot * 48;
		const auto baseline = decode_t48_column(pristine.data() + offset);
		if (!baseline)
		{
			result.failure = plan_error::math_failure;
			result.failed_request = request_index;
			return result;
		}
		const auto native = recover_native_linear(request.observed_skin_linear,
			linear_part(baseline.value));
		const auto encoded = native ? encode_translation_only(native.value,
			request.displacement, baseline.value) : math_result<matrix4> { native.failure };
		if (!encoded)
		{
			result.failure = plan_error::math_failure;
			result.failed_request = request_index;
			return result;
		}
		std::array<uint8_t, 48> bytes {};
		if (encode_t48_column(encoded.value, bytes.data()) != math_error::none)
		{
			result.failure = plan_error::float32_overflow;
			result.failed_request = request_index;
			return result;
		}
		for (size_t float_index : { size_t {3}, size_t {7}, size_t {11} })
			std::memcpy(candidate.data() + offset + float_index * sizeof(float),
				bytes.data() + float_index * sizeof(float), sizeof(float));
		if (std::memcmp(candidate.data() + offset, pristine.data() + offset, 48) != 0)
			result.changed_slots.push_back(request.slot);
	}
	result.bytes = std::move(candidate);
	return result;
}
}
