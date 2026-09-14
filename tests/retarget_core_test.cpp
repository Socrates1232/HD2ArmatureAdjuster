#include "retarget/evaluator.hpp"
#include "retarget/plan_encoder.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <random>
#include <vector>

namespace
{
using namespace hd2aa::retarget;

int failures = 0;

void check(bool condition, const char *message)
{
	if (!condition)
	{
		std::cerr << "FAIL: " << message << '\n';
		++failures;
	}
}

bool close(double left, double right, double tolerance = 1.0e-8)
{
	return std::fabs(left - right) <= tolerance;
}

bool close(const vector3 &left, const vector3 &right, double tolerance = 1.0e-8)
{
	for (size_t index = 0; index < 3; ++index)
		if (!close(left[index], right[index], tolerance))
			return false;
	return true;
}

bool close(const linear3 &left, const linear3 &right, double tolerance = 1.0e-8)
{
	for (size_t index = 0; index < 9; ++index)
		if (!close(left[index], right[index], tolerance))
			return false;
	return true;
}

bool close(const matrix4 &left, const matrix4 &right, double tolerance = 1.0e-8)
{
	for (size_t index = 0; index < 16; ++index)
		if (!close(left[index], right[index], tolerance))
			return false;
	return true;
}

matrix4 rz(double angle)
{
	matrix4 result = identity();
	const double c = std::cos(angle), s = std::sin(angle);
	result[0] = c;
	result[1] = -s;
	result[4] = s;
	result[5] = c;
	return result;
}

matrix4 random_affine(std::mt19937_64 &generator)
{
	std::uniform_real_distribution<double> angle(-2.0, 2.0);
	std::uniform_real_distribution<double> scale(0.6, 1.4);
	std::uniform_real_distribution<double> position(-1.0, 1.0);
	matrix4 result = rz(angle(generator));
	result[0] *= scale(generator);
	result[1] *= scale(generator);
	result[4] *= scale(generator);
	result[5] *= scale(generator);
	result[2] = position(generator) * 0.08;
	result[6] = position(generator) * 0.08;
	result[10] = scale(generator);
	result[3] = position(generator);
	result[7] = position(generator);
	result[11] = position(generator);
	return result;
}

struct fixture_data
{
	std::vector<int32_t> parents;
	std::vector<matrix4> rest;
	std::vector<matrix4> target;
	std::vector<matrix4> native;
	std::vector<matrix4> binds;
};

fixture_data fixture()
{
	fixture_data out;
	out.parents = { -1, 0, 1, 2, 3, 3 };
	out.rest = {
		translation({ 0, 0, 1 }),
		multiply(translation({ .20, 0, .30 }), rz(.15)),
		multiply(translation({ .30, 0, 0 }), rz(-.1)),
		translation({ .25, 0, 0 }),
		translation({ .06, .015, 0 }),
		translation({ .07, -.015, 0 })
	};
	out.target = out.rest;
	out.target[1][3] -= .03;
	std::vector<matrix4> motion;
	for (double angle : { .3, .5, 1.2, -.4, .25, -.2 })
		motion.push_back(rz(angle));
	motion[0][3] = .7;
	motion[0][7] = -.2;
	motion[0][11] = .1;
	motion[2][3] = .002;
	motion[2][7] = -.001;
	std::vector<matrix4> animated_local;
	for (size_t index = 0; index < out.rest.size(); ++index)
		animated_local.push_back(multiply(out.rest[index], motion[index]));
	out.native = world_from_local(animated_local, out.parents).value;
	const auto rest_world = world_from_local(out.rest, out.parents).value;
	for (const matrix4 &world : rest_world)
		out.binds.push_back(inverse(world).value);
	return out;
}

matrix4 with_linear_and_translation(const linear3 &linear, const vector3 &position)
{
	matrix4 result = identity();
	for (size_t row = 0; row < 3; ++row)
		for (size_t column = 0; column < 3; ++column)
			result[row * 4 + column] = linear[row * 3 + column];
	result[3] = position[0];
	result[7] = position[1];
	result[11] = position[2];
	return result;
}

vector3 transform_point(const matrix4 &matrix, const vector3 &point)
{
	return {
		matrix[0] * point[0] + matrix[1] * point[1] + matrix[2] * point[2] + matrix[3],
		matrix[4] * point[0] + matrix[5] * point[1] + matrix[6] * point[2] + matrix[7],
		matrix[8] * point[0] + matrix[9] * point[1] + matrix[10] * point[2] + matrix[11]
	};
}

void test_identity_and_round_trip()
{
	const auto data = fixture();
	std::vector<matrix4> bases(data.parents.size(), identity());
	const auto target = evaluate_full(data.native, data.rest, data.rest, bases, data.parents);
	check(target && target.value.size() == data.native.size(), "M01 identity target evaluates");
	for (size_t index = 0; index < data.native.size(); ++index)
	{
		check(close(target.value[index], data.native[index]), "M01 identity reproduces native pose");
		const auto encoded = encode_desired(data.native[index], target.value[index], data.binds[index]);
		check(encoded && close(encoded.value, data.binds[index]), "M01 identity reproduces bind");
	}
	const auto local = local_from_world(data.native, data.parents);
	const auto world = local ? world_from_local(local.value, data.parents) :
		retarget_result<std::vector<matrix4>> {};
	check(local && world, "M03 local/world conversion succeeds");
	for (size_t index = 0; index < data.native.size(); ++index)
		check(close(world.value[index], data.native[index]), "M03 local/world round trip");
}

void test_full_and_linear_retarget()
{
	const auto data = fixture();
	std::vector<matrix4> bases(data.parents.size(), identity());
	const auto target = evaluate_full(data.native, data.rest, data.target, bases, data.parents);
	const auto deltas = rest_translation_deltas(data.rest, data.target);
	std::vector<linear3> native_linear;
	for (const matrix4 &matrix : data.native)
		native_linear.push_back(linear_part(matrix));
	const auto displacement = propagate_displacements(native_linear, deltas.value, data.parents);
	check(target && deltas && displacement, "M04 compatible rest-position rig evaluates");
	for (size_t index = 0; index < data.parents.size(); ++index)
	{
		const matrix4 shifted = multiply(translation(displacement.value[index]), data.native[index]);
		check(close(shifted, target.value[index]), "M04 linear evaluator equals full evaluator");
		const auto linear = encode_translation_only(native_linear[index],
			displacement.value[index], data.binds[index]);
		const auto general = encode_desired(data.native[index], target.value[index], data.binds[index]);
		check(linear && general && close(linear.value, general.value),
			"M04 translation encoder equals general encoder");
		if (index >= 1)
			check(close(displacement.value[index], displacement.value[1]),
				"M04 complete arm including fingers inherits one displacement");
		if (index >= 2)
		{
			const auto old_parent = inverse(data.native[data.parents[index]]);
			const auto new_parent = inverse(target.value[data.parents[index]]);
			check(old_parent && new_parent && close(
				multiply(old_parent.value, data.native[index]),
				multiply(new_parent.value, target.value[index])),
				"M04 internal articulation is unchanged");
		}
	}

	auto rotated = data.target;
	rotated[2] = multiply(rotated[2], rz(.2));
	check(rest_translation_deltas(data.rest, rotated).failure ==
		retarget_error::linear_rest_changed,
		"rest rotation requires full native pose");
}

void test_random_affines_and_basis()
{
	std::mt19937_64 generator(607);
	const std::vector<int32_t> parents { -1, 0, 1, 1, 2, 4, 3, 6 };
	for (size_t iteration = 0; iteration < 100; ++iteration)
	{
		std::vector<matrix4> rest, target, local, bind, bases(parents.size(), identity());
		for (size_t index = 0; index < parents.size(); ++index)
		{
			rest.push_back(random_affine(generator));
			target.push_back(rest.back());
			if (index != 0)
			{
				target.back()[3] += std::sin(static_cast<double>(iteration + index)) * .025;
				target.back()[7] += std::cos(static_cast<double>(iteration + index)) * .02;
			}
			local.push_back(multiply(rest.back(), random_affine(generator)));
		}
		const auto world = world_from_local(local, parents);
		const auto target_world = evaluate_full(world.value, rest, target, bases, parents);
		const auto deltas = rest_translation_deltas(rest, target);
		std::vector<linear3> native_linear;
		for (const matrix4 &matrix : world.value)
			native_linear.push_back(linear_part(matrix));
		const auto displacement = propagate_displacements(native_linear, deltas.value, parents);
		check(target_world && displacement, "M05 randomized scaled/sheared rig evaluates");
		for (size_t index = 0; index < parents.size(); ++index)
		{
			const matrix4 mesh_bind = random_affine(generator);
			const auto direct = encode_desired(world.value[index], target_world.value[index], mesh_bind);
			const auto optimized = encode_translation_only(native_linear[index],
				displacement.value[index], mesh_bind);
			check(direct && optimized && close(direct.value, optimized.value, 2.0e-8),
				"M05 randomized general and optimized paths agree");
		}
	}

	auto data = fixture();
	std::vector<matrix4> basis(data.parents.size(), identity());
	basis[2] = identity();
	basis[2][5] = 0;
	basis[2][6] = -1;
	basis[2][9] = 1;
	basis[2][10] = 0;
	const auto target_world = evaluate_full(data.native, data.rest, data.target, basis, data.parents);
	const auto source_local = local_from_world(data.native, data.parents);
	const auto target_local = local_from_world(target_world.value, data.parents);
	const auto rest_inverse = inverse(data.rest[2]);
	const auto basis_inverse = inverse(basis[2]);
	const matrix4 expected = multiply(multiply(multiply(data.target[2], basis[2]),
		multiply(rest_inverse.value, source_local.value[2])), basis_inverse.value);
	check(close(target_local.value[2], expected), "M06 explicit delta basis is applied by conjugation");
}

void test_feedback_codec_and_legacy_bridge()
{
	const auto data = fixture();
	const auto deltas = rest_translation_deltas(data.rest, data.target);
	std::vector<linear3> native_linear;
	for (const matrix4 &matrix : data.native)
		native_linear.push_back(linear_part(matrix));
	const auto displacement = propagate_displacements(native_linear, deltas.value, data.parents);
	std::vector<matrix4> installed = data.binds;
	for (size_t step = 0; step < 500; ++step)
	{
		const double amount = .5 + .5 * std::sin(step * .1);
		for (size_t index = 0; index < installed.size(); ++index)
		{
			const matrix4 observed = multiply(data.native[index], installed[index]);
			const auto recovered = recover_native_linear(linear_part(observed),
				linear_part(data.binds[index]));
			check(recovered && close(recovered.value, native_linear[index], 1.0e-7),
				"M07 translation feedback does not contaminate native linear input");
			const vector3 scaled { displacement.value[index][0] * amount,
				displacement.value[index][1] * amount, displacement.value[index][2] * amount };
			installed[index] = encode_translation_only(recovered.value, scaled,
				data.binds[index]).value;
		}
	}

	const matrix4 nontrivial = multiply(translation({ .31, -.42, .53 }), rz(.7));
	std::array<uint8_t, 48> packed {};
	check(encode_t48_column(nontrivial, packed.data()) == math_error::none,
		"M11 column t48 encode succeeds");
	const float *floats = reinterpret_cast<const float *>(packed.data());
	check(close(floats[3], .31, 1.0e-6) && close(floats[7], -.42, 1.0e-6) &&
		close(floats[11], .53, 1.0e-6), "M11 t48 translation lanes are 3/7/11");
	check(close(decode_t48_column(packed.data()).value, nontrivial, 1.0e-6),
		"M11 t48 round trip");
	const auto row = transpose_to_existing_row(nontrivial);
	std::array<uint8_t, 64> file64 {};
	std::array<uint8_t, 48> converted {};
	armature_probe::encode_file64(row, file64.data());
	armature_probe::file64_to_t48(file64.data(), converted.data());
	check(converted == packed, "M11 file64 row to t48 column adapter agrees");

	const vector3 wanted { -.03, .01, .004 };
	for (size_t index = 0; index < data.native.size(); ++index)
	{
		const matrix4 skin = multiply(data.native[index], data.binds[index]);
		const auto skin_inverse = inverse(linear_part(skin));
		const vector3 legacy_argument = multiply(skin_inverse.value, wanted);
		const matrix4 legacy_column = multiply(data.binds[index], translation(legacy_argument));
		const auto direct = encode_translation_only(native_linear[index], wanted, data.binds[index]);
		check(direct && close(legacy_column, direct.value), "M12 legacy helper bridge agrees");
	}

	std::array<uint8_t, 48> baseline_bytes {};
	encode_t48_column(data.binds[1], baseline_bytes.data());
	std::vector<uint8_t> table(baseline_bytes.begin(), baseline_bytes.end());
	const auto no_op = build_translation_plan(table, 1,
		{{ 0, linear_part(multiply(data.native[1], data.binds[1])), {} }});
	check(no_op.failure == plan_error::none && no_op.bytes == table && no_op.changed_slots.empty(),
		"zero target is an exact byte no-op");
}

void test_bind_skin_timing_and_failures()
{
	const auto data = fixture();
	const matrix4 changed_bind = multiply(translation({ .03, 0, 0 }), data.binds[2]);
	const matrix4 observed = multiply(data.native[2], changed_bind);
	const matrix4 wrong_native = multiply(observed, inverse(data.binds[2]).value);
	const matrix4 right_native = multiply(observed, inverse(changed_bind).value);
	check(!close(wrong_native, data.native[2], .005), "M08 wrong consumed bind contaminates full pose");
	check(close(right_native, data.native[2]), "M08 correct consumed bind recovers full pose");

	const auto source_rest_world = world_from_local(data.rest, data.parents).value;
	const auto target_rest_world = world_from_local(data.target, data.parents).value;
	check(!close(multiply(target_rest_world[1], inverse(source_rest_world[1]).value), identity(), .01),
		"M09 reshape-existing changes rest output");
	check(close(multiply(target_rest_world[1], inverse(target_rest_world[1]).value), identity()),
		"M09 target-bound bind cancels at its own rest pose");

	const vector3 raw { -.03, 0, 0 };
	const matrix4 first = multiply(data.native[1], multiply(data.binds[1], translation(raw)));
	const matrix4 second = multiply(data.native[2], multiply(data.binds[2], translation(raw)));
	const vector3 first_shift {
		first[3] - multiply(data.native[1], data.binds[1])[3],
		first[7] - multiply(data.native[1], data.binds[1])[7],
		first[11] - multiply(data.native[1], data.binds[1])[11] };
	const vector3 second_shift {
		second[3] - multiply(data.native[2], data.binds[2])[3],
		second[7] - multiply(data.native[2], data.binds[2])[7],
		second[11] - multiply(data.native[2], data.binds[2])[11] };
	check(!close(first_shift, second_shift, .01), "M13 uniform legacy offsets distort a bent branch");

	const vector3 wanted { .03, 0, 0 };
	const auto written = encode_translation_only(linear_part(identity()), wanted, identity());
	const matrix4 later = rz(10.0 * 3.14159265358979323846 / 180.0);
	const vector3 actual = translation_part(multiply(later, written.value));
	const double timing_error = std::sqrt((actual[0] - wanted[0]) * (actual[0] - wanted[0]) +
		(actual[1] - wanted[1]) * (actual[1] - wanted[1]));
	check(timing_error > .005 && close(timing_error,
		2 * .03 * std::sin(5.0 * 3.14159265358979323846 / 180.0), 1.0e-12),
		"M14 stale orientation exposes timing error");

	const auto actor_a = encode_translation_only(linear_part(identity()), wanted, identity());
	const auto actor_b = encode_translation_only(linear_part(rz(1.0)), wanted, identity());
	check(!close(actor_a.value, actor_b.value, .02),
		"M15 one shared table cannot satisfy two differently posed actors");

	linear3 singular { 1, 0, 0, 0, 0, 0, 0, 0, 1 };
	check(inverse(singular).failure == math_error::singular, "M16 singular input is rejected");
	matrix4 nonfinite = identity();
	nonfinite[0] = std::numeric_limits<double>::quiet_NaN();
	check(inverse(nonfinite).failure == math_error::non_finite, "M16 nonfinite input is rejected");
	check(validate_parents({ 0 }) == retarget_error::invalid_parent,
		"M16 malformed parent graph is rejected");
}

void test_blending_and_space_adapter()
{
	const auto data = fixture();
	std::vector<matrix4> bases(data.parents.size(), identity());
	const auto target = evaluate_full(data.native, data.rest, data.target, bases, data.parents).value;
	const vector3 point { .5, 0, 1.3 };
	const std::array<size_t, 4> arm_slots { 1, 2, 3, 4 };
	const std::array<double, 4> arm_weights { .2, .3, .3, .2 };
	vector3 source {}, changed {};
	for (size_t lane = 0; lane < arm_slots.size(); ++lane)
	{
		const size_t slot = arm_slots[lane];
		const auto before = transform_point(multiply(data.native[slot], data.binds[slot]), point);
		const auto after = transform_point(multiply(target[slot], data.binds[slot]), point);
		for (size_t axis = 0; axis < 3; ++axis)
		{
			source[axis] += arm_weights[lane] * before[axis];
			changed[axis] += arm_weights[lane] * after[axis];
		}
	}
	const vector3 output { changed[0] - source[0], changed[1] - source[1], changed[2] - source[2] };
	const vector3 expected {
		target[1][3] - data.native[1][3], target[1][7] - data.native[1][7],
		target[1][11] - data.native[1][11] };
	check(close(output, expected), "M10 normalized branch blend receives one displacement");

	const vector3 sparse_point { 1, 2, 3 };
	const vector3 weighted = transform_point(identity(), sparse_point);
	check(close(vector3 { weighted[0] * .7, weighted[1] * .7, weighted[2] * .7 },
		vector3 { .7, 1.4, 2.1 }), "M18 sparse weights are not silently normalized");

	std::mt19937_64 generator(24);
	const matrix4 adapter = random_affine(generator);
	const matrix4 native = random_affine(generator);
	const matrix4 target_world = random_affine(generator);
	const matrix4 bind = random_affine(generator);
	const auto common = encode_desired(multiply(adapter, native),
		multiply(adapter, target_world), bind);
	const auto unit = encode_desired(native, target_world, bind);
	check(common && unit && close(common.value, unit.value, 1.0e-8),
		"M17 common-space adapter cancels in the encoder");
}
}

int main()
{
	test_identity_and_round_trip();
	test_full_and_linear_retarget();
	test_random_affines_and_basis();
	test_feedback_codec_and_legacy_bridge();
	test_bind_skin_timing_and_failures();
	test_blending_and_space_adapter();
	if (failures != 0)
	{
		std::cerr << failures << " retarget-core checks failed\n";
		return 1;
	}
	std::cout << "stage-1 retarget core assertions passed\n";
	return 0;
}
