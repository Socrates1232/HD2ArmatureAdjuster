#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <immintrin.h>
#include <intrin.h>

namespace armature_probe
{
inline bool have_sse41()
{
	static const bool available = [] {
		int registers[4] = {};
		__cpuid(registers, 0);
		if (registers[0] < 1)
			return false;
		__cpuid(registers, 1);
		return (registers[2] & (1 << 19)) != 0;
	}();
	return available;
}

inline void copy_from_write_combined(void *destination, const void *source, size_t size)
{
	if (!have_sse41())
	{
		std::memcpy(destination, source, size);
		return;
	}

	auto *out = static_cast<uint8_t *>(destination);
	const auto *in = static_cast<const uint8_t *>(source);
	_mm_mfence();

	size_t head = (16u - (reinterpret_cast<uintptr_t>(in) & 15u)) & 15u;
	head = head > size ? size : head;
	if (head != 0)
	{
		std::memcpy(out, in, head);
		out += head;
		in += head;
		size -= head;
	}

	while (size >= 16)
	{
		const __m128i value = _mm_stream_load_si128(
			reinterpret_cast<__m128i *>(const_cast<uint8_t *>(in)));
		_mm_storeu_si128(reinterpret_cast<__m128i *>(out), value);
		out += 16;
		in += 16;
		size -= 16;
	}
	_mm_mfence();
	if (size != 0)
		std::memcpy(out, in, size);
}
}
