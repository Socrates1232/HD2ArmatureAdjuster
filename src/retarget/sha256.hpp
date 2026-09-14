#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace hd2aa
{
class sha256
{
public:
	void update(const void *data, size_t size)
	{
		const auto *bytes = static_cast<const uint8_t *>(data);
		_total += size;
		while (size != 0)
		{
			const size_t take = size < 64 - _used ? size : 64 - _used;
			std::memcpy(_block.data() + _used, bytes, take);
			_used += take;
			bytes += take;
			size -= take;
			if (_used == 64)
			{
				transform(_block.data());
				_used = 0;
			}
		}
	}

	std::array<uint8_t, 32> finish()
	{
		const uint64_t bits = _total * 8;
		_block[_used++] = 0x80;
		if (_used > 56)
		{
			while (_used < 64) _block[_used++] = 0;
			transform(_block.data());
			_used = 0;
		}
		while (_used < 56) _block[_used++] = 0;
		for (int index = 7; index >= 0; --index)
			_block[_used++] = static_cast<uint8_t>(bits >> (index * 8));
		transform(_block.data());
		std::array<uint8_t, 32> result {};
		for (size_t word = 0; word < 8; ++word)
			for (size_t byte = 0; byte < 4; ++byte)
				result[word * 4 + byte] = static_cast<uint8_t>(_state[word] >> (24 - byte * 8));
		return result;
	}

private:
	std::array<uint32_t, 8> _state { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
		0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
	std::array<uint8_t, 64> _block {};
	size_t _used = 0;
	uint64_t _total = 0;

	static uint32_t rotate(uint32_t value, unsigned count)
	{
		return (value >> count) | (value << (32 - count));
	}

	void transform(const uint8_t *block)
	{
		static constexpr uint32_t constants[64] = {
			0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
			0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
			0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
			0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
			0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
			0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
			0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
			0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
		uint32_t words[64] = {};
		for (size_t index = 0; index < 16; ++index)
			words[index] = static_cast<uint32_t>(block[index * 4]) << 24 |
				static_cast<uint32_t>(block[index * 4 + 1]) << 16 |
				static_cast<uint32_t>(block[index * 4 + 2]) << 8 | block[index * 4 + 3];
		for (size_t index = 16; index < 64; ++index)
		{
			const uint32_t s0 = rotate(words[index - 15], 7) ^ rotate(words[index - 15], 18) ^
				(words[index - 15] >> 3);
			const uint32_t s1 = rotate(words[index - 2], 17) ^ rotate(words[index - 2], 19) ^
				(words[index - 2] >> 10);
			words[index] = words[index - 16] + s0 + words[index - 7] + s1;
		}
		uint32_t a = _state[0], b = _state[1], c = _state[2], d = _state[3];
		uint32_t e = _state[4], f = _state[5], g = _state[6], h = _state[7];
		for (size_t index = 0; index < 64; ++index)
		{
			const uint32_t s1 = rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25);
			const uint32_t choice = (e & f) ^ (~e & g);
			const uint32_t first = h + s1 + choice + constants[index] + words[index];
			const uint32_t s0 = rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22);
			const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
			const uint32_t second = s0 + majority;
			h = g; g = f; f = e; e = d + first;
			d = c; c = b; b = a; a = first + second;
		}
		_state[0] += a; _state[1] += b; _state[2] += c; _state[3] += d;
		_state[4] += e; _state[5] += f; _state[6] += g; _state[7] += h;
	}
};

inline std::string sha256_hex(const void *data, size_t size)
{
	sha256 hash;
	hash.update(data, size);
	const auto digest = hash.finish();
	static constexpr char hex[] = "0123456789abcdef";
	std::string result(64, '0');
	for (size_t index = 0; index < digest.size(); ++index)
	{
		result[index * 2] = hex[digest[index] >> 4];
		result[index * 2 + 1] = hex[digest[index] & 15];
	}
	return result;
}
}
