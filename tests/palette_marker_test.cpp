#include "palette_marker.hpp"
#include "pose_driver.hpp"

#include <cstring>
#include <iostream>

int main()
{
	using namespace armature_probe;
	constexpr uint32_t entries = 21, first_control = 8, probe_slot = 12, repeats = 8;
	std::vector<uint8_t> bytes(64 + static_cast<size_t>(entries - 1) * 48 + 64, 0xA5);
	const size_t palette = 64;
	for (uint32_t element = 0; element < entries - 1; ++element)
	{
		affine_matrix value = translation(static_cast<float>(element) * 0.01f, 0, 0);
		if (element >= probe_slot)
			value = translation(0, 0.5f, 0);
		encode_t48(value, bytes.data() + palette + static_cast<size_t>(element) * 48);
	}
	const std::vector<palette_marker> markers {
		{ 1, 2, entries, first_control, probe_slot, repeats },
		{ 3, 4, entries, first_control, probe_slot, repeats + 3 }
	};
	const auto hits = find_palette_markers(bytes.data(), bytes.size(), markers);
	if (hits.size() != 1 || hits[0].marker_index != 0 || hits[0].palette_offset != palette ||
		hits[0].palette_end != palette + static_cast<size_t>(entries - 1) * 48)
	{
		std::cerr << "palette marker location failed: " << hits.size() << " hits";
		for (const auto &hit : hits)
			std::cerr << " [" << hit.marker_index << ',' << hit.palette_offset << ','
				<< hit.palette_end << ']';
		std::cerr << '\n';
		return 1;
	}
	const std::vector<palette_marker_hit> mixed_hits {
		{ 1, 64, 1024 },
		{ 0, 128, 768 },
	};
	const auto requested = requested_palette_hits(mixed_hits, std::vector<uint8_t> { 1, 0 });
	if (requested.size() != 1 || requested[0].marker_index != 0 || requested[0].palette_end != 768)
	{
		std::cerr << "an unrelated higher palette displaced the requested marker\n";
		return 2;
	}
	bytes[palette + static_cast<size_t>(probe_slot + 2) * 48] ^= 1;
	if (!find_palette_markers(bytes.data(), bytes.size(), markers).empty())
	{
		std::cerr << "broken marker tail was accepted\n";
		return 3;
	}
	std::cout << "palette marker location and rejection passed\n";
	return 0;
}
