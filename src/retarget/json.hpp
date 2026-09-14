#pragma once

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace hd2aa::json
{
enum class kind { null_value, boolean, number, string, array, object };

struct value
{
	kind type = kind::null_value;
	bool boolean = false;
	double number = 0.0;
	std::string string;
	std::vector<value> array;
	std::vector<std::pair<std::string, value>> object;

	const value *find(const char *name) const
	{
		for (const auto &item : object)
			if (item.first == name)
				return &item.second;
		return nullptr;
	}
};

class parser
{
public:
	parser(const char *begin, const char *end) : _cursor(begin), _end(end) {}

	bool parse(value &out, std::string &error)
	{
		skip_space();
		if (!parse_value(out, error, 0))
			return false;
		skip_space();
		if (_cursor != _end)
			return fail(error, "characters follow the JSON value");
		return true;
	}

private:
	const char *_cursor;
	const char *_end;

	bool fail(std::string &error, const char *message)
	{
		error = message;
		return false;
	}

	void skip_space()
	{
		while (_cursor != _end && (*_cursor == ' ' || *_cursor == '\t' ||
			*_cursor == '\r' || *_cursor == '\n'))
			++_cursor;
	}

	bool consume(char character)
	{
		if (_cursor == _end || *_cursor != character)
			return false;
		++_cursor;
		return true;
	}

	bool literal(const char *text)
	{
		const char *probe = _cursor;
		while (*text != '\0' && probe != _end && *probe == *text)
		{
			++probe;
			++text;
		}
		if (*text != '\0')
			return false;
		_cursor = probe;
		return true;
	}

	static int hex_digit(char value)
	{
		if (value >= '0' && value <= '9') return value - '0';
		if (value >= 'a' && value <= 'f') return value - 'a' + 10;
		if (value >= 'A' && value <= 'F') return value - 'A' + 10;
		return -1;
	}

	bool codepoint(uint32_t &out)
	{
		if (_end - _cursor < 4)
			return false;
		out = 0;
		for (int index = 0; index < 4; ++index)
		{
			const int digit = hex_digit(*_cursor++);
			if (digit < 0)
				return false;
			out = out * 16 + static_cast<uint32_t>(digit);
		}
		return true;
	}

	static void append_utf8(std::string &out, uint32_t point)
	{
		if (point <= 0x7f)
			out.push_back(static_cast<char>(point));
		else if (point <= 0x7ff)
		{
			out.push_back(static_cast<char>(0xc0 | (point >> 6)));
			out.push_back(static_cast<char>(0x80 | (point & 0x3f)));
		}
		else if (point <= 0xffff)
		{
			out.push_back(static_cast<char>(0xe0 | (point >> 12)));
			out.push_back(static_cast<char>(0x80 | ((point >> 6) & 0x3f)));
			out.push_back(static_cast<char>(0x80 | (point & 0x3f)));
		}
		else
		{
			out.push_back(static_cast<char>(0xf0 | (point >> 18)));
			out.push_back(static_cast<char>(0x80 | ((point >> 12) & 0x3f)));
			out.push_back(static_cast<char>(0x80 | ((point >> 6) & 0x3f)));
			out.push_back(static_cast<char>(0x80 | (point & 0x3f)));
		}
	}

	bool parse_string(std::string &out, std::string &error)
	{
		if (!consume('"'))
			return fail(error, "expected a JSON string");
		out.clear();
		while (_cursor != _end)
		{
			const unsigned char character = static_cast<unsigned char>(*_cursor++);
			if (character == '"')
				return true;
			if (character < 0x20)
				return fail(error, "control character in JSON string");
			if (character != '\\')
			{
				out.push_back(static_cast<char>(character));
				continue;
			}
			if (_cursor == _end)
				return fail(error, "truncated JSON escape");
			const char escaped = *_cursor++;
			switch (escaped)
			{
			case '"': case '\\': case '/': out.push_back(escaped); break;
			case 'b': out.push_back('\b'); break;
			case 'f': out.push_back('\f'); break;
			case 'n': out.push_back('\n'); break;
			case 'r': out.push_back('\r'); break;
			case 't': out.push_back('\t'); break;
			case 'u':
			{
				uint32_t point = 0;
				if (!codepoint(point))
					return fail(error, "invalid JSON unicode escape");
				if (point >= 0xd800 && point <= 0xdbff)
				{
					if (_end - _cursor < 6 || _cursor[0] != '\\' || _cursor[1] != 'u')
						return fail(error, "unpaired JSON high surrogate");
					_cursor += 2;
					uint32_t low = 0;
					if (!codepoint(low) || low < 0xdc00 || low > 0xdfff)
						return fail(error, "invalid JSON surrogate pair");
					point = 0x10000 + ((point - 0xd800) << 10) + (low - 0xdc00);
				}
				else if (point >= 0xdc00 && point <= 0xdfff)
					return fail(error, "unpaired JSON low surrogate");
				append_utf8(out, point);
				break;
			}
			default: return fail(error, "invalid JSON escape");
			}
		}
		return fail(error, "unterminated JSON string");
	}

	bool parse_number(value &out, std::string &error)
	{
		const char *begin = _cursor;
		if (_cursor != _end && *_cursor == '-') ++_cursor;
		if (_cursor == _end) return fail(error, "truncated JSON number");
		if (*_cursor == '0') ++_cursor;
		else
		{
			if (*_cursor < '1' || *_cursor > '9') return fail(error, "invalid JSON number");
			while (_cursor != _end && *_cursor >= '0' && *_cursor <= '9') ++_cursor;
		}
		if (_cursor != _end && *_cursor == '.')
		{
			++_cursor;
			if (_cursor == _end || *_cursor < '0' || *_cursor > '9') return fail(error, "invalid JSON fraction");
			while (_cursor != _end && *_cursor >= '0' && *_cursor <= '9') ++_cursor;
		}
		if (_cursor != _end && (*_cursor == 'e' || *_cursor == 'E'))
		{
			++_cursor;
			if (_cursor != _end && (*_cursor == '+' || *_cursor == '-')) ++_cursor;
			if (_cursor == _end || *_cursor < '0' || *_cursor > '9') return fail(error, "invalid JSON exponent");
			while (_cursor != _end && *_cursor >= '0' && *_cursor <= '9') ++_cursor;
		}
		std::string text(begin, _cursor);
		char *after = nullptr;
		errno = 0;
		const double number = std::strtod(text.c_str(), &after);
		if (errno == ERANGE || after != text.c_str() + text.size() || !std::isfinite(number))
			return fail(error, "JSON number is not finite");
		out.type = kind::number;
		out.number = number;
		return true;
	}

	bool parse_value(value &out, std::string &error, unsigned depth)
	{
		if (depth > 64 || _cursor == _end)
			return fail(error, depth > 64 ? "JSON nesting exceeds 64" : "truncated JSON value");
		if (*_cursor == 'n' && literal("null")) { out.type = kind::null_value; return true; }
		if (*_cursor == 't' && literal("true")) { out.type = kind::boolean; out.boolean = true; return true; }
		if (*_cursor == 'f' && literal("false")) { out.type = kind::boolean; out.boolean = false; return true; }
		if (*_cursor == '"') { out.type = kind::string; return parse_string(out.string, error); }
		if (*_cursor == '-' || (*_cursor >= '0' && *_cursor <= '9')) return parse_number(out, error);
		if (*_cursor == '[')
		{
			out.type = kind::array;
			++_cursor;
			skip_space();
			if (consume(']')) return true;
			for (;;)
			{
				out.array.emplace_back();
				if (!parse_value(out.array.back(), error, depth + 1)) return false;
				skip_space();
				if (consume(']')) return true;
				if (!consume(',')) return fail(error, "expected comma in JSON array");
				skip_space();
			}
		}
		if (*_cursor == '{')
		{
			out.type = kind::object;
			++_cursor;
			skip_space();
			if (consume('}')) return true;
			for (;;)
			{
				std::string name;
				if (!parse_string(name, error)) return false;
				for (const auto &item : out.object)
					if (item.first == name) return fail(error, "duplicate JSON object key");
				skip_space();
				if (!consume(':')) return fail(error, "expected colon in JSON object");
				skip_space();
				value child;
				if (!parse_value(child, error, depth + 1)) return false;
				out.object.emplace_back(std::move(name), std::move(child));
				skip_space();
				if (consume('}')) return true;
				if (!consume(',')) return fail(error, "expected comma in JSON object");
				skip_space();
			}
		}
		return fail(error, "invalid JSON value");
	}
};
}
