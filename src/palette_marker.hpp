#pragma once

#include "matrix_scan.hpp"
#include "runtime_profile.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace armature_probe
{
struct palette_marker
{
	uint64_t unit_id = 0;
	uint64_t table_key = 0;
	uint32_t entries = 0;
	uint32_t first_control_slot = 0;
	uint32_t probe_slot = 0;
	uint32_t tail_repeats = 0;
};

struct palette_marker_hit
{
	size_t marker_index = 0;
	size_t palette_offset = 0;
	size_t palette_end = 0;
};

inline std::vector<palette_marker_hit> find_palette_markers(const uint8_t *data, size_t size,
	const std::vector<palette_marker> &markers)
{
	std::vector<palette_marker_hit> hits;
	for (uint32_t phase = 0; phase < armature_profile::transform_stride; phase += 16)
	{
		size_t run_start = phase;
		uint32_t run = 1;
		auto admit = [&](size_t end) {
			for (size_t index = 0; index < markers.size(); ++index)
			{
				const palette_marker &marker = markers[index];
				if (run != marker.tail_repeats || marker.entries < 2)
					continue;
				const size_t palette_bytes = static_cast<size_t>(marker.entries - 1) *
					armature_profile::transform_stride;
				if (end < palette_bytes)
					continue;
				const size_t begin = end - palette_bytes;
				if (run_start != begin + static_cast<size_t>(marker.probe_slot) *
					armature_profile::transform_stride)
					continue;
				const format_score score = assess(data + begin, palette_bytes,
					armature_profile::transform_stride);
				const uint32_t palette_entries = marker.entries - 1;
				if (score.confidence >= 0.90f &&
					score.layout_a * 10 >= palette_entries * 9)
				{
					const float quality = score.confidence * 100.0f +
						static_cast<float>(score.layout_b) / palette_entries;
					auto overlap = std::find_if(hits.begin(), hits.end(),
						[index, begin, end](const palette_marker_hit &hit) {
							return hit.marker_index == index && begin < hit.palette_end &&
								hit.palette_offset < end;
						});
					if (overlap == hits.end())
						hits.push_back({ index, begin, end });
					else
					{
						const format_score old_score = assess(data + overlap->palette_offset,
							overlap->palette_end - overlap->palette_offset,
							armature_profile::transform_stride);
						const float old_quality = old_score.confidence * 100.0f +
							static_cast<float>(old_score.layout_b) / palette_entries;
						if (quality > old_quality)
							*overlap = { index, begin, end };
					}
				}
			}
		};
		for (size_t offset = phase + armature_profile::transform_stride;
			offset + armature_profile::transform_stride <= size;
			offset += armature_profile::transform_stride)
		{
			if (std::memcmp(data + offset, data + offset - armature_profile::transform_stride,
				armature_profile::transform_stride) == 0)
			{
				++run;
				continue;
			}
			if (run >= 2)
				admit(offset);
			run_start = offset;
			run = 1;
		}
		if (run >= 2)
			admit(phase + ((size - phase) / armature_profile::transform_stride) *
				armature_profile::transform_stride);
	}
	return hits;
}
}
