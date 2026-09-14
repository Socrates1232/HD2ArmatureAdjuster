#include "ib_replacement_generator.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <iterator>
#include <limits>
#include <random>
#include <string>
#include <vector>

namespace
{
using armature_probe::affine_matrix;
using armature_probe::ib_replacement::error;
using armature_probe::ib_replacement::linear3;
using armature_probe::ib_replacement::vector3;

int failures = 0;

void check(bool condition, const std::string &message)
{
	if (!condition)
	{
		std::cerr << "FAILED: " << message << '\n';
		++failures;
	}
}

bool close(double left, double right, double tolerance = 2.0e-5)
{
	return std::fabs(left - right) <= tolerance;
}

bool close(const affine_matrix &left, const affine_matrix &right,
	double tolerance = 2.0e-5)
{
	for (size_t index = 0; index < left.size(); ++index)
		if (!close(left[index], right[index], tolerance))
			return false;
	return true;
}

bool close(const std::array<double, 4> &left, const std::array<double, 4> &right,
	double tolerance = 2.0e-5)
{
	for (size_t index = 0; index < left.size(); ++index)
		if (!close(left[index], right[index], tolerance))
			return false;
	return true;
}

linear3 rotation_z(double angle)
{
	const double cosine = std::cos(angle);
	const double sine = std::sin(angle);
	return {cosine, sine, 0.0, -sine, cosine, 0.0, 0.0, 0.0, 1.0};
}

linear3 multiply(const linear3 &left, const linear3 &right)
{
	linear3 result {};
	for (size_t row = 0; row < 3; ++row)
		for (size_t column = 0; column < 3; ++column)
			for (size_t inner = 0; inner < 3; ++inner)
				result[row * 3 + column] +=
					left[row * 3 + inner] * right[inner * 3 + column];
	return result;
}

affine_matrix transform(const linear3 &linear = {1.0, 0.0, 0.0, 0.0, 1.0,
	0.0, 0.0, 0.0, 1.0}, const vector3 &position = {})
{
	return {
		static_cast<float>(linear[0]), static_cast<float>(linear[1]),
		static_cast<float>(linear[2]), 0.0f,
		static_cast<float>(linear[3]), static_cast<float>(linear[4]),
		static_cast<float>(linear[5]), 0.0f,
		static_cast<float>(linear[6]), static_cast<float>(linear[7]),
		static_cast<float>(linear[8]), 0.0f,
		static_cast<float>(position[0]), static_cast<float>(position[1]),
		static_cast<float>(position[2]), 1.0f
	};
}

std::array<uint8_t, 48> pack(const affine_matrix &matrix)
{
	std::array<uint8_t, 48> result {};
	armature_probe::encode_t48(matrix, result.data());
	return result;
}

std::array<double, 4> point_times_matrix(const std::array<double, 4> &point,
	const affine_matrix &matrix)
{
	std::array<double, 4> result {};
	for (size_t column = 0; column < 4; ++column)
		for (size_t row = 0; row < 4; ++row)
			result[column] += point[row] * matrix[row * 4 + column];
	return result;
}

double distance3(const vector3 &left, const vector3 &right)
{
	const double x = left[0] - right[0];
	const double y = left[1] - right[1];
	const double z = left[2] - right[2];
	return std::sqrt(x * x + y * y + z * z);
}

void test_packing_and_exact_zero()
{
	const affine_matrix baseline = transform(
		{1.0, 0.2, 0.1, 0.3, 2.0, 0.2, 0.4, 0.1, 3.0}, {0.5, 0.6, 0.7});
	const auto bytes = pack(baseline);
	std::array<float, 12> packed {};
	std::memcpy(packed.data(), bytes.data(), bytes.size());
	check(packed[3] == 0.5f && packed[7] == 0.6f && packed[11] == 0.7f,
		"t48 translation is stored at float indices 3, 7, and 11");

	linear3 deliberately_invalid {};
	const auto zero = armature_probe::ib_replacement::make_entry(
		bytes.data(), deliberately_invalid, {0.0, 0.0, 0.0});
	check(zero.failure == error::none, "zero displacement does not require a pose inverse");
	check(zero.bytes == bytes, "zero displacement returns exact pristine bytes");
}

void test_rotated_branch_and_linear_byte_identity()
{
	const affine_matrix baseline = transform();
	const auto baseline_bytes = pack(baseline);
	const vector3 displacement {0.03, 0.0, 0.0};
	const affine_matrix poses[] = {transform(), transform(rotation_z(1.5707963267948966))};
	vector3 legacy_translations[2] {};

	for (size_t index = 0; index < 2; ++index)
	{
		const auto native_skin = armature_probe::ib_replacement::multiply(baseline, poses[index]);
		const auto generated = armature_probe::ib_replacement::make_entry(
			baseline_bytes.data(), armature_probe::ib_replacement::linear_part(native_skin),
			displacement);
		check(generated.failure == error::none, "pose-correct entry generation succeeds");
		const affine_matrix replacement = armature_probe::decode_t48(generated.bytes.data());
		const affine_matrix actual = armature_probe::ib_replacement::multiply(replacement, poses[index]);
		const affine_matrix wanted = armature_probe::ib_replacement::multiply(
			native_skin, armature_probe::ib_replacement::translation(displacement));
		check(close(actual, wanted), "rotated branch entry receives the common output displacement");

		for (const size_t float_index : {size_t {0}, size_t {1}, size_t {2}, size_t {4},
			size_t {5}, size_t {6}, size_t {8}, size_t {9}, size_t {10}})
			check(std::memcmp(generated.bytes.data() + float_index * sizeof(float),
				baseline_bytes.data() + float_index * sizeof(float), sizeof(float)) == 0,
				"replacement preserves every packed linear float byte-for-byte");

		std::array<uint8_t, 48> legacy_bytes {};
		armature_probe::translate_world_t48(baseline_bytes.data(), 0.03f, 0.0f, 0.0f,
			legacy_bytes.data());
		const auto legacy_skin = armature_probe::ib_replacement::multiply(
			armature_probe::decode_t48(legacy_bytes.data()), poses[index]);
		legacy_translations[index] = {legacy_skin[12], legacy_skin[13], legacy_skin[14]};
	}
	check(distance3(legacy_translations[0], legacy_translations[1]) > 0.04,
		"legacy equal pre-offsets diverge when branch entries rotate differently");
}

void test_random_affines_blends_and_translation_feedback()
{
	std::mt19937_64 random(361928);
	std::uniform_real_distribution<double> angle(-3.0, 3.0);
	std::uniform_real_distribution<double> scale(0.6, 1.5);
	std::uniform_real_distribution<double> offset(-0.04, 0.04);
	std::uniform_real_distribution<double> position(-1.0, 1.0);
	std::uniform_real_distribution<double> weight(0.05, 1.0);

	for (size_t sample = 0; sample < 200; ++sample)
	{
		const vector3 displacement {offset(random), offset(random), offset(random)};
		std::array<affine_matrix, 4> native_skins {};
		std::array<affine_matrix, 4> corrected_skins {};
		std::array<double, 4> weights {};
		double weight_sum = 0.0;

		for (size_t bone = 0; bone < 4; ++bone)
		{
			linear3 baseline_linear = multiply(
				{scale(random), 0.0, 0.05, 0.0, scale(random), 0.0, 0.0, 0.0, scale(random)},
				rotation_z(angle(random)));
			linear3 pose_linear = multiply(
				{scale(random), 0.0, 0.0, 0.04, scale(random), 0.0, 0.0, 0.0, scale(random)},
				rotation_z(angle(random)));
			const affine_matrix baseline = transform(baseline_linear,
				{position(random), position(random), position(random)});
			const affine_matrix pose = transform(pose_linear,
				{position(random), position(random), position(random)});
			const auto pristine = pack(baseline);

			std::array<uint8_t, 48> prior_bytes {};
			armature_probe::translate_world_t48(pristine.data(), static_cast<float>(offset(random)),
				static_cast<float>(offset(random)), static_cast<float>(offset(random)),
				prior_bytes.data());
			const affine_matrix prior = armature_probe::decode_t48(prior_bytes.data());
			const affine_matrix observed = armature_probe::ib_replacement::multiply(prior, pose);
			const auto generated = armature_probe::ib_replacement::make_entry(pristine.data(),
				armature_probe::ib_replacement::linear_part(observed), displacement);
			check(generated.failure == error::none,
				"random nonuniform-scale/shear input remains invertible");

			native_skins[bone] = armature_probe::ib_replacement::multiply(baseline, pose);
			corrected_skins[bone] = armature_probe::ib_replacement::multiply(
				armature_probe::decode_t48(generated.bytes.data()), pose);
			const auto wanted = armature_probe::ib_replacement::multiply(native_skins[bone],
				armature_probe::ib_replacement::translation(displacement));
			check(close(corrected_skins[bone], wanted, 8.0e-5),
				"previous translation-only overrides do not contaminate the observed linear block");
			weights[bone] = weight(random);
			weight_sum += weights[bone];
		}

		const std::array<double, 4> point {position(random), position(random), position(random), 1.0};
		std::array<double, 4> native_blend {};
		std::array<double, 4> corrected_blend {};
		for (size_t bone = 0; bone < 4; ++bone)
		{
			weights[bone] /= weight_sum;
			const auto native = point_times_matrix(point, native_skins[bone]);
			const auto corrected = point_times_matrix(point, corrected_skins[bone]);
			for (size_t component = 0; component < 4; ++component)
			{
				native_blend[component] += weights[bone] * native[component];
				corrected_blend[component] += weights[bone] * corrected[component];
			}
		}
		const std::array<double, 4> wanted_blend {
			native_blend[0] + displacement[0], native_blend[1] + displacement[1],
			native_blend[2] + displacement[2], native_blend[3]
		};
		check(close(corrected_blend, wanted_blend, 1.2e-4),
			"normalized branch blends translate uniformly");
	}
}

void test_mixed_boundary_weights()
{
	const affine_matrix baseline = transform();
	const affine_matrix pose = transform(rotation_z(0.8), {0.2, -0.1, 0.4});
	const affine_matrix native = armature_probe::ib_replacement::multiply(baseline, pose);
	const auto pristine = pack(baseline);
	const vector3 displacement {0.03, -0.01, 0.02};
	const auto generated = armature_probe::ib_replacement::make_entry(pristine.data(),
		armature_probe::ib_replacement::linear_part(native), displacement);
	const affine_matrix arm = armature_probe::ib_replacement::multiply(
		armature_probe::decode_t48(generated.bytes.data()), pose);
	const std::array<double, 4> point {0.3, 0.4, -0.2, 1.0};
	const auto native_point = point_times_matrix(point, native);
	const auto arm_point = point_times_matrix(point, arm);
	const double arm_weight = 0.6;
	std::array<double, 4> mixed {};
	for (size_t component = 0; component < 4; ++component)
		mixed[component] = arm_weight * arm_point[component] +
			(1.0 - arm_weight) * native_point[component];
	const std::array<double, 4> wanted {
		native_point[0] + arm_weight * displacement[0],
		native_point[1] + arm_weight * displacement[1],
		native_point[2] + arm_weight * displacement[2], native_point[3]
	};
	check(close(mixed, wanted), "torso/arm boundary follows the original mixed weight");
}

void test_no_drift_and_rotation_contamination()
{
	const affine_matrix baseline = transform(rotation_z(0.37), {0.2, -0.6, 1.1});
	const auto pristine = pack(baseline);
	affine_matrix installed = baseline;
	for (size_t frame = 0; frame < 500; ++frame)
	{
		const affine_matrix pose = transform(rotation_z(frame * 0.01),
			{frame * 0.001, 0.2, 0.3});
		const affine_matrix observed = armature_probe::ib_replacement::multiply(installed, pose);
		const vector3 displacement = armature_probe::ib_replacement::multiply(
			{0.03, 0.0, 0.0}, rotation_z(frame * 0.002));
		const auto generated = armature_probe::ib_replacement::make_entry(pristine.data(),
			armature_probe::ib_replacement::linear_part(observed), displacement);
		installed = armature_probe::decode_t48(generated.bytes.data());
		const auto actual = armature_probe::ib_replacement::multiply(installed, pose);
		const auto wanted = armature_probe::ib_replacement::multiply(
			armature_probe::ib_replacement::multiply(baseline, pose),
			armature_probe::ib_replacement::translation(displacement));
		check(close(actual, wanted, 8.0e-5), "replacement generation does not accumulate drift");
	}

	const affine_matrix pose = transform(rotation_z(0.2));
	const affine_matrix contaminated = armature_probe::ib_replacement::multiply(
		transform(rotation_z(0.8)), baseline);
	const auto observed = armature_probe::ib_replacement::multiply(contaminated, pose);
	const vector3 displacement {0.03, 0.0, 0.0};
	const auto generated = armature_probe::ib_replacement::make_entry(pristine.data(),
		armature_probe::ib_replacement::linear_part(observed), displacement);
	const auto actual = armature_probe::ib_replacement::multiply(
		armature_probe::decode_t48(generated.bytes.data()), pose);
	const auto wanted = armature_probe::ib_replacement::multiply(
		armature_probe::ib_replacement::multiply(baseline, pose),
		armature_probe::ib_replacement::translation(displacement));
	check(!close(actual, wanted, 0.01),
		"a prior linear-block edit invalidates the translation-only observation shortcut");
}

void test_relative_transforms_and_general_encoder()
{
	const affine_matrix chest = transform(rotation_z(0.4), {0.2, -0.1, 0.5});
	const affine_matrix shoulder = armature_probe::ib_replacement::multiply(
		transform(rotation_z(0.2), {0.18, 0.0, 0.0}), chest);
	const affine_matrix elbow = armature_probe::ib_replacement::multiply(
		transform(rotation_z(1.0), {0.0, 0.3, 0.0}), shoulder);
	const affine_matrix hand = armature_probe::ib_replacement::multiply(
		transform(rotation_z(-0.3), {0.0, 0.25, 0.0}), elbow);
	const affine_matrix finger = armature_probe::ib_replacement::multiply(
		transform(rotation_z(0.7), {0.02, 0.07, 0.0}), hand);
	const affine_matrix native[] = {shoulder, elbow, hand, finger};
	const vector3 displacement {-0.03, 0.01, 0.0};
	for (size_t index = 1; index < std::size(native); ++index)
	{
		affine_matrix native_parent_inverse {}, desired_parent_inverse {};
		const auto desired = armature_probe::ib_replacement::multiply(
			native[index], armature_probe::ib_replacement::translation(displacement));
		const auto desired_parent = armature_probe::ib_replacement::multiply(
			native[index - 1], armature_probe::ib_replacement::translation(displacement));
		check(armature_probe::ib_replacement::invert_affine(native[index - 1],
			native_parent_inverse) == error::none, "native parent is invertible");
		check(armature_probe::ib_replacement::invert_affine(desired_parent,
			desired_parent_inverse) == error::none, "desired parent is invertible");
		const auto native_relative = armature_probe::ib_replacement::multiply(
			native[index], native_parent_inverse);
		const auto desired_relative = armature_probe::ib_replacement::multiply(
			desired, desired_parent_inverse);
		check(close(native_relative, desired_relative, 8.0e-5),
			"common branch displacement preserves relative transforms and segment lengths");
	}

	const affine_matrix baseline = transform(
		multiply({0.9, 0.0, 0.0, 0.0, 1.1, 0.0, 0.0, 0.0, 1.0}, rotation_z(0.7)),
		{0.2, 0.3, 0.4});
	const affine_matrix native_pose = transform(rotation_z(-0.9), {0.3, 0.4, 0.5});
	const affine_matrix desired_pose = transform(rotation_z(0.5), {0.2, 0.7, 0.6});
	affine_matrix replacement {};
	check(armature_probe::ib_replacement::encode_desired_pose(baseline, native_pose,
		desired_pose, replacement) == error::none, "general desired-pose encoder succeeds");
	check(close(armature_probe::ib_replacement::multiply(replacement, native_pose),
		armature_probe::ib_replacement::multiply(baseline, desired_pose), 8.0e-5),
		"general encoder satisfies B' * W = B * W desired");
}

void test_stale_pose_shared_table_and_failures()
{
	const affine_matrix baseline = transform();
	const auto pristine = pack(baseline);
	const vector3 displacement {0.03, 0.0, 0.0};
	const auto generated = armature_probe::ib_replacement::make_entry(pristine.data(),
		armature_probe::ib_replacement::linear_part(baseline), displacement);
	const affine_matrix consumed_pose = transform(rotation_z(0.17453292519943295));
	const affine_matrix stale_result = armature_probe::ib_replacement::multiply(
		armature_probe::decode_t48(generated.bytes.data()), consumed_pose);
	const double stale_error = std::sqrt(
		std::pow(stale_result[12] - displacement[0], 2) +
		std::pow(stale_result[13] - displacement[1], 2) +
		std::pow(stale_result[14] - displacement[2], 2));
	const double expected_error = 2.0 * 0.03 * std::sin(0.17453292519943295 / 2.0);
	check(close(stale_error, expected_error, 2.0e-6) && stale_error > 0.005,
		"a ten-degree stale pose exposes the expected nonzero timing error");

	const affine_matrix other_pose = transform(rotation_z(1.5707963267948966));
	const auto shared_actual = armature_probe::ib_replacement::multiply(
		armature_probe::decode_t48(generated.bytes.data()), other_pose);
	const auto shared_wanted = armature_probe::ib_replacement::multiply(other_pose,
		armature_probe::ib_replacement::translation(displacement));
	check(!close(shared_actual, shared_wanted, 0.02),
		"one pose-dependent IB payload cannot satisfy a differently posed shared instance");

	linear3 singular {};
	check(armature_probe::ib_replacement::make_entry(pristine.data(), singular,
		displacement).failure == error::singular_basis, "singular basis is rejected");
	linear3 near_singular {1.0, 0.0, 0.0, 0.0, 1.0e-12, 0.0, 0.0, 0.0, 1.0};
	check(armature_probe::ib_replacement::make_entry(pristine.data(), near_singular,
		displacement).failure == error::ill_conditioned_basis,
		"near-singular basis is rejected by its condition estimate");
	linear3 non_finite {1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0,
		std::numeric_limits<double>::quiet_NaN()};
	check(armature_probe::ib_replacement::make_entry(pristine.data(), non_finite,
		displacement).failure == error::non_finite_input, "non-finite basis is rejected");
	check(armature_probe::ib_replacement::make_entry(pristine.data(),
		armature_probe::ib_replacement::linear_part(baseline),
		{std::numeric_limits<double>::infinity(), 0.0, 0.0}).failure ==
		error::non_finite_input, "non-finite displacement is rejected");

	std::vector<uint8_t> table(pristine.begin(), pristine.end());
	table.insert(table.end(), pristine.begin(), pristine.end());
	const auto successful_plan = armature_probe::ib_replacement::make_table_plan(table, 2,
		{{1, armature_probe::ib_replacement::linear_part(baseline), displacement}});
	check(successful_plan.failure == error::none &&
		successful_plan.changed_slots == std::vector<size_t> {1},
		"successful table plan reports only its changed slot");
	check(std::memcmp(successful_plan.bytes.data(), table.data(), 48) == 0,
		"successful table plan preserves untouched entries exactly");
	const auto zero_plan = armature_probe::ib_replacement::make_table_plan(table, 2,
		{{1, singular, {0.0, 0.0, 0.0}}});
	check(zero_plan.failure == error::none && zero_plan.bytes == table &&
		zero_plan.changed_slots.empty(), "zero table plan is an exact declared no-op");
	const std::vector<armature_probe::ib_replacement::slot_request> requests {
		{0, armature_probe::ib_replacement::linear_part(baseline), displacement},
		{1, singular, displacement}
	};
	const auto failed_plan = armature_probe::ib_replacement::make_table_plan(table, 2, requests);
	check(failed_plan.failure == error::singular_basis && failed_plan.bytes == table &&
		failed_plan.changed_slots.empty(), "table planning is failure-atomic");
	const auto duplicate_plan = armature_probe::ib_replacement::make_table_plan(table, 2,
		{{0, armature_probe::ib_replacement::linear_part(baseline), displacement},
		 {0, armature_probe::ib_replacement::linear_part(other_pose), displacement}});
	check(duplicate_plan.failure == error::duplicate_slot && duplicate_plan.bytes == table,
		"conflicting duplicate slot requests are rejected before publication");
	check(armature_probe::ib_replacement::make_table_plan(table, 2,
		{{2, armature_probe::ib_replacement::linear_part(baseline), displacement}}).failure ==
		error::slot_out_of_range, "out-of-range slots are rejected");
	std::vector<uint8_t> truncated = table;
	truncated.pop_back();
	check(armature_probe::ib_replacement::make_table_plan(truncated, 2, {}).failure ==
		error::invalid_table_size, "table byte count must exactly match entry count");
}
}

int main()
{
	test_packing_and_exact_zero();
	test_rotated_branch_and_linear_byte_identity();
	test_random_affines_blends_and_translation_feedback();
	test_mixed_boundary_weights();
	test_no_drift_and_rotation_contamination();
	test_relative_transforms_and_general_encoder();
	test_stale_pose_shared_table_and_failures();
	if (failures)
	{
		std::cerr << failures << " inverse-bind replacement test(s) failed\n";
		return 1;
	}
	std::cout << "pose-correct inverse-bind replacement generator passed\n";
	return 0;
}
