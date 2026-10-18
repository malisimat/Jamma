///////////////////////////////////////////////////////////
//
// Author 2019 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#include "Json.h"

#include <exception>
#include <cmath>
#include <algorithm>
#include <locale>
#include <cctype>
#include <limits>
#include <string>
#include <type_traits>

using namespace io;

// Rig persistence uses this reader; legacy Jam files retain FromStream semantics.
class Json::StrictReader
{
public:
	explicit StrictReader(std::stringstream& stream) : Stream(stream) {}

	std::optional<JsonValue> Read()
	{
		auto value = ReadValue(0u);
		SkipSpace();
		if (!value || Stream.peek() != std::char_traits<char>::eof())
			return std::nullopt;
		return value;
	}

private:
	std::stringstream& Stream;
	static constexpr unsigned int MaxDepth = 64u;

	void SkipSpace()
	{
		while (Stream.peek() == ' ' || Stream.peek() == '\t' ||
			Stream.peek() == '\r' || Stream.peek() == '\n')
			Stream.get();
	}

	bool Consume(char expected)
	{
		if (Stream.peek() != expected)
			return false;
		Stream.get();
		return true;
	}

	bool ReadHex(unsigned int& value)
	{
		value = 0u;
		for (unsigned int i = 0u; i < 4u; ++i)
		{
			const auto ch = Stream.get();
			unsigned int digit;
			if (ch >= '0' && ch <= '9') digit = ch - '0';
			else if (ch >= 'a' && ch <= 'f') digit = ch - 'a' + 10u;
			else if (ch >= 'A' && ch <= 'F') digit = ch - 'A' + 10u;
			else return false;
			value = (value << 4u) | digit;
		}
		return true;
	}

	static void AppendUtf8(std::string& text, unsigned int point)
	{
		if (point <= 0x7fu) text.push_back(static_cast<char>(point));
		else if (point <= 0x7ffu)
		{
			text.push_back(static_cast<char>(0xc0u | (point >> 6u)));
			text.push_back(static_cast<char>(0x80u | (point & 0x3fu)));
		}
		else if (point <= 0xffffu)
		{
			text.push_back(static_cast<char>(0xe0u | (point >> 12u)));
			text.push_back(static_cast<char>(0x80u | ((point >> 6u) & 0x3fu)));
			text.push_back(static_cast<char>(0x80u | (point & 0x3fu)));
		}
		else
		{
			text.push_back(static_cast<char>(0xf0u | (point >> 18u)));
			text.push_back(static_cast<char>(0x80u | ((point >> 12u) & 0x3fu)));
			text.push_back(static_cast<char>(0x80u | ((point >> 6u) & 0x3fu)));
			text.push_back(static_cast<char>(0x80u | (point & 0x3fu)));
		}
	}

	std::optional<std::string> ReadString()
	{
		if (!Consume('"')) return std::nullopt;
		std::string text;
		for (;;)
		{
			const auto ch = Stream.get();
			if (ch == std::char_traits<char>::eof() || ch < 0x20)
				return std::nullopt;
			if (ch == '"') return text;
			if (ch != '\\')
			{
				if (ch < 0x80)
					text.push_back(static_cast<char>(ch));
				else
				{
					unsigned int count;
					unsigned int point;
					unsigned int minimum;
					if (ch >= 0xc2 && ch <= 0xdf) { count = 1u; point = ch & 0x1f; minimum = 0x80u; }
					else if (ch >= 0xe0 && ch <= 0xef) { count = 2u; point = ch & 0x0f; minimum = 0x800u; }
					else if (ch >= 0xf0 && ch <= 0xf4) { count = 3u; point = ch & 0x07; minimum = 0x10000u; }
					else return std::nullopt;
					for (unsigned int i = 0u; i < count; ++i)
					{
						const auto next = Stream.get();
						if (next < 0x80 || next > 0xbf) return std::nullopt;
						point = (point << 6u) | (next & 0x3f);
					}
					if (point < minimum || point > 0x10ffffu ||
						(point >= 0xd800u && point <= 0xdfffu))
						return std::nullopt;
					AppendUtf8(text, point);
				}
				continue;
			}
			switch (Stream.get())
			{
			case '"': text.push_back('"'); break;
			case '\\': text.push_back('\\'); break;
			case '/': text.push_back('/'); break;
			case 'b': text.push_back('\b'); break;
			case 'f': text.push_back('\f'); break;
			case 'n': text.push_back('\n'); break;
			case 'r': text.push_back('\r'); break;
			case 't': text.push_back('\t'); break;
			case 'u':
			{
				unsigned int point;
				if (!ReadHex(point)) return std::nullopt;
				if (point >= 0xd800u && point <= 0xdbffu)
				{
					unsigned int low;
					if (!Consume('\\') || !Consume('u') || !ReadHex(low) ||
						low < 0xdc00u || low > 0xdfffu)
						return std::nullopt;
					point = 0x10000u + ((point - 0xd800u) << 10u) + low - 0xdc00u;
				}
				else if (point >= 0xdc00u && point <= 0xdfffu)
					return std::nullopt;
				AppendUtf8(text, point);
				break;
			}
			default: return std::nullopt;
			}
		}
	}

	std::optional<JsonValue> ReadNumber()
	{
		std::string token;
		if (Consume('-')) token.push_back('-');
		if (Consume('0')) token.push_back('0');
		else
		{
			if (Stream.peek() < '1' || Stream.peek() > '9') return std::nullopt;
			do { token.push_back(static_cast<char>(Stream.get())); }
			while (Stream.peek() >= '0' && Stream.peek() <= '9');
		}
		bool floating = false;
		if (Consume('.'))
		{
			floating = true;
			token.push_back('.');
			if (Stream.peek() < '0' || Stream.peek() > '9') return std::nullopt;
			do { token.push_back(static_cast<char>(Stream.get())); }
			while (Stream.peek() >= '0' && Stream.peek() <= '9');
		}
		if (Stream.peek() == 'e' || Stream.peek() == 'E')
		{
			floating = true;
			token.push_back(static_cast<char>(Stream.get()));
			if (Stream.peek() == '+' || Stream.peek() == '-')
				token.push_back(static_cast<char>(Stream.get()));
			if (Stream.peek() < '0' || Stream.peek() > '9') return std::nullopt;
			do { token.push_back(static_cast<char>(Stream.get())); }
			while (Stream.peek() >= '0' && Stream.peek() <= '9');
		}
		try
		{
			if (!floating)
			{
				if (token.front() == '-')
				{
					const auto value = std::stoll(token);
					if (value >= (std::numeric_limits<long>::min)() &&
						value <= (std::numeric_limits<long>::max)())
						return static_cast<long>(value);
				}
				else
				{
					const auto value = std::stoull(token);
					if (value <= (std::numeric_limits<unsigned long>::max)())
						return static_cast<unsigned long>(value);
				}
			}
		}
		catch (const std::exception&) {}
		std::stringstream number(token);
		number.imbue(std::locale::classic());
		double value;
		if (!(number >> value) || !std::isfinite(value)) return std::nullopt;
		return value;
	}

	static bool AppendArray(JsonArray& array, const JsonValue& value)
	{
		return std::visit([&array](const auto& item) -> bool
		{
			using ValueType = std::decay_t<decltype(item)>;
			if constexpr (std::is_same_v<ValueType, JsonArray>) return false;
			else
			{
				using VectorType = std::vector<ValueType>;
				if (array.Length == 0u) array.Array = VectorType{};
				auto values = std::get_if<VectorType>(&array.Array);
				if (!values || array.Length == (std::numeric_limits<unsigned int>::max)())
					return false;
				values->push_back(item);
				++array.Length;
				return true;
			}
		}, value);
	}

	std::optional<JsonValue> ReadValue(unsigned int depth)
	{
		if (depth > MaxDepth) return std::nullopt;
		SkipSpace();
		if (Stream.peek() == '"')
		{
			auto value = ReadString();
			if (!value) return std::nullopt;
			return *value;
		}
		if (Consume('{'))
		{
			JsonPart object;
			SkipSpace();
			if (Consume('}')) return object;
			for (;;)
			{
				SkipSpace();
				auto key = ReadString();
				SkipSpace();
				if (!key || !Consume(':')) return std::nullopt;
				auto value = ReadValue(depth + 1u);
				if (!value || !object.KeyValues.emplace(*key, *value).second)
					return std::nullopt;
				object.KeyOrder.push_back(*key);
				SkipSpace();
				if (Consume('}')) return object;
				if (!Consume(',')) return std::nullopt;
			}
		}
		if (Consume('['))
		{
			JsonArray array{0u, std::vector<bool>{}};
			SkipSpace();
			if (Consume(']')) return array;
			for (;;)
			{
				auto value = ReadValue(depth + 1u);
				if (!value || !AppendArray(array, *value)) return std::nullopt;
				SkipSpace();
				if (Consume(']')) return array;
				if (!Consume(',')) return std::nullopt;
			}
		}
		if (Stream.peek() == '-' || (Stream.peek() >= '0' && Stream.peek() <= '9'))
			return ReadNumber();
		std::string literal;
		while (Stream.peek() >= 'a' && Stream.peek() <= 'z')
			literal.push_back(static_cast<char>(Stream.get()));
		if (literal == "true") return true;
		if (literal == "false") return false;
		if (literal == "null") return std::monostate{};
		return std::nullopt;
	}
};

std::optional<Json::JsonValue> Json::FromStrictStream(std::stringstream ss)
{
	return StrictReader(ss).Read();
}


std::optional<Json::JsonValue> Json::FromStream(std::stringstream ss)
{
	auto root = ParseValue(std::move(ss));
	if (!root.Value.has_value())
		return std::nullopt;
	char trailing = 0;
	while (root.Stream.get(trailing))
		if (!std::isspace(static_cast<unsigned char>(trailing)))
			return std::nullopt;
	return root.Value;
}

bool Json::ToStream(Json::JsonValue json, std::stringstream& ss)
{
	return WriteValue(json, ss);
}

bool Json::WriteString(const std::string& value, std::stringstream& ss)
{
	ss << '"';
	for (const unsigned char ch : value)
	{
		switch (ch)
		{
		case '\\': ss << "\\\\"; break;
		case '"': ss << "\\\""; break;
		case '\b': ss << "\\b"; break;
		case '\f': ss << "\\f"; break;
		case '\n': ss << "\\n"; break;
		case '\r': ss << "\\r"; break;
		case '\t': ss << "\\t"; break;
		default:
			if (ch < 0x20u)
			{
				static constexpr char hexDigits[] = "0123456789ABCDEF";
				ss << "\\u00" << hexDigits[ch >> 4] << hexDigits[ch & 0x0Fu];
			}
			else
			{
				ss << static_cast<char>(ch);
			}
			break;
		}
	}
	ss << '"';
	return true;
}

bool Json::WriteValue(const JsonValue& json, std::stringstream& ss)
{
	return std::visit([&ss](const auto& value) -> bool
		{
			using ValueType = std::decay_t<decltype(value)>;
			if constexpr (std::is_same_v<ValueType, std::monostate>)
			{
				ss << "null";
				return true;
			}
			else if constexpr (std::is_same_v<ValueType, bool>)
			{
				ss << (value ? "true" : "false");
				return true;
			}
			else if constexpr (std::is_same_v<ValueType, long> || std::is_same_v<ValueType, unsigned long>)
			{
				ss << value;
				return true;
			}
			else if constexpr (std::is_same_v<ValueType, double>)
			{
				ss << value;
				return true;
			}
			else if constexpr (std::is_same_v<ValueType, std::string>)
			{
				return WriteString(value, ss);
			}
			else if constexpr (std::is_same_v<ValueType, JsonArray>)
			{
				ss << '[';
				std::visit([&ss](const auto& arrayValues)
					{
						for (std::size_t i = 0; i < arrayValues.size(); ++i)
						{
							if (i > 0u)
								ss << ',';
							Json::WriteValue(arrayValues[i], ss);
						}
					}, value.Array);
				ss << ']';
				return true;
			}
			else if constexpr (std::is_same_v<ValueType, JsonPart>)
			{
				ss << '{';
				bool first = true;
				auto writeMember = [&](const auto& keyValue)
				{
					if (!first) ss << ',';
					first = false;
					WriteString(keyValue.first, ss);
					ss << ':';
					WriteValue(keyValue.second, ss);
				};
				for (const auto& key : value.KeyOrder)
				{
					const auto member = value.KeyValues.find(key);
					if (member != value.KeyValues.end()) writeMember(*member);
				}
				for (const auto& keyValue : value.KeyValues)
					if (std::find(value.KeyOrder.begin(), value.KeyOrder.end(), keyValue.first) == value.KeyOrder.end())
						writeMember(keyValue);
				ss << '}';
				return true;
			}
			else
			{
				return false;
			}
		}, json);
}

bool Json::IsAllDigits(std::string str, bool includePeriod)
{
	auto isFirstChar = true;

	for (auto ch : str)
	{
		if (!std::isdigit(static_cast<unsigned char>(ch)))
		{
			if (includePeriod)
			{
				if ('.' == ch)
					continue;
			}

			if (isFirstChar)
			{
				if ('-' != ch)
					return false;
			}
			else
				return false;
		}

		isFirstChar = false;
	}

	return true;
}

bool Json::IsTrue(std::string str)
{
	std::vector<char> charBuf;
	for (auto letter : str)
		charBuf.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(letter))));

	charBuf.push_back('\0');
	auto strLower = std::string(charBuf.data(), charBuf.size() - 1u);
	return "true" == strLower;
}

std::optional<std::string> Json::GetString(const JsonPart& json, const std::string& key)
{
	auto iter = json.KeyValues.find(key);
	if (iter == json.KeyValues.end())
		return std::nullopt;

	if (const auto value = std::get_if<std::string>(&iter->second))
		return *value;

	return std::nullopt;
}

std::optional<unsigned int> Json::GetUnsigned(const JsonPart& json, const std::string& key)
{
	auto iter = json.KeyValues.find(key);
	if (iter == json.KeyValues.end())
		return std::nullopt;

	if (const auto value = std::get_if<unsigned long>(&iter->second))
		return static_cast<unsigned int>(*value);

	return std::nullopt;
}

std::string Json::NormaliseStringArrayValue(std::string value)
{
	if ((value.size() >= 2u) && (value.front() == '"') && (value.back() == '"'))
		value = value.substr(1u, value.size() - 2u);

	std::string normalised;
	normalised.reserve(value.size());
	for (size_t i = 0; i < value.size(); ++i)
	{
		if (value[i] != '\\' || i + 1u >= value.size())
		{
			normalised.push_back(value[i]);
			continue;
		}

		switch (value[++i])
		{
		case '"': normalised.push_back('"'); break;
		case '\\': normalised.push_back('\\'); break;
		case '/': normalised.push_back('/'); break;
		case 'b': normalised.push_back('\b'); break;
		case 'f': normalised.push_back('\f'); break;
		case 'n': normalised.push_back('\n'); break;
		case 'r': normalised.push_back('\r'); break;
		case 't': normalised.push_back('\t'); break;
		default: normalised.push_back(value[i]); break;
		}
	}

	return normalised;
}

Json::KeyResult Json::ParseKey(std::stringstream ss)
{	
	std::string key;
	char c;
	std::vector<char> charBuf;

	while (ss >> c)
	{
		if ('"' == c)
		{
			charBuf.push_back('\0');
			key = std::string(charBuf.data(), charBuf.size() - 1u);
			break;
		}
		else
			charBuf.push_back(c);
	}

	if (key.empty())
		return { std::move(ss), std::nullopt };

	return { std::move(ss), key };
}

Json::ValueResult Json::ParseValue(std::stringstream ss)
{
	char c;
	bool isInsideQuotes = false;
	std::vector<char> charBuf;

	while (ss >> c)
	{
		if ('{' == c)
		{
			// Value is a JsonPart (map of key values)
			auto res = ParseJsonPart(std::move(ss));

			if (std::nullopt != res.Part)
				return { std::move(res.Stream), res.Part };
			else
				return { std::move(res.Stream), std::nullopt };
		}
		else if ('[' == c)
		{
			charBuf.clear();
			// Value is array
			auto isNextPart = false;
			auto isArrayEnd = false;
			auto isFirstIt = true;
			auto isQuotedString = false;
			std::vector<std::string> values;

			while (ss.get(c))
			{
				if (!isQuotedString && std::isspace(static_cast<unsigned char>(c)))
					continue;

				if (isFirstIt)
				{
					if ('{' == c)
					{
						std::vector<JsonPart> parts;

						while (ss && !isArrayEnd)
						{
							auto part = ParseJsonPart(std::move(ss));
							if (part.Part.has_value())
								parts.push_back(part.Part.value());

							ss = std::move(part.Stream);
							while (ss >> c)
							{
								isNextPart = ',' == c;
								isArrayEnd = ']' == c;
								if (isNextPart || isArrayEnd)
									break;
							}

							if (isNextPart && !isArrayEnd)
							{
								while (ss >> c)
								{
									if ('{' == c)
										break;
								}
							}
						}

						JsonArray jsonArray;
						jsonArray.Length = (unsigned int)parts.size();
						jsonArray.Array = parts;
						return { std::move(ss), jsonArray };
					}

					isFirstIt = false;
				}

				if ('"' == c)
				{
					isQuotedString = !isQuotedString;
					continue;
				}

				isArrayEnd = !isQuotedString && (']' == c);
				if (!isQuotedString && ((',' == c) || isArrayEnd))
				{
					charBuf.push_back('\0');
					values.push_back(std::string(charBuf.data(), charBuf.size() - 1u));

					if (isArrayEnd)
						break;
					else
						charBuf.clear();
				}
				else
					charBuf.push_back(c);
			}

			return { std::move(ss), ParseJsonArray(values) };
		}
		else if ('"' == c)
		{
			charBuf.clear();
			bool isEscaped = false;

			while (ss.get(c))
			{
				if (isEscaped)
				{
					switch (c)
					{
					case '"':
						charBuf.push_back('"');
						break;
					case '\\':
						charBuf.push_back('\\');
						break;
					case '/':
						charBuf.push_back('/');
						break;
					case 'b':
						charBuf.push_back('\b');
						break;
					case 'f':
						charBuf.push_back('\f');
						break;
					case 'n':
						charBuf.push_back('\n');
						break;
					case 'r':
						charBuf.push_back('\r');
						break;
					case 't':
						charBuf.push_back('\t');
						break;
					case 'u':
					{
						// Minimal unicode handling: consume 4 hex digits and preserve
						// ASCII code points directly. Non-ASCII code points are ignored.
						char hex[4];
						if (ss.get(hex[0]) && ss.get(hex[1]) && ss.get(hex[2]) && ss.get(hex[3]))
						{
							unsigned int codePoint = 0u;
							for (auto i = 0; i < 4; i++)
							{
								codePoint <<= 4;
								if (hex[i] >= '0' && hex[i] <= '9') codePoint += static_cast<unsigned int>(hex[i] - '0');
								else if (hex[i] >= 'a' && hex[i] <= 'f') codePoint += static_cast<unsigned int>(hex[i] - 'a' + 10);
								else if (hex[i] >= 'A' && hex[i] <= 'F') codePoint += static_cast<unsigned int>(hex[i] - 'A' + 10);
							}

							if (codePoint <= 0x7f)
								charBuf.push_back(static_cast<char>(codePoint));
						}
						break;
					}
					default:
						charBuf.push_back(c);
						break;
					}

					isEscaped = false;
					continue;
				}

				if ('\\' == c)
				{
					isEscaped = true;
					continue;
				}

				if ('"' == c)
				{
					charBuf.push_back('\0');
					return { std::move(ss), std::string(charBuf.data(), charBuf.size() - 1u) };
				}

				charBuf.push_back(c);
			}
		}
		else
		{
			charBuf.clear();
			charBuf.push_back(c);

			bool finished = false;

			while (!finished)
			{
				char peeked = ss.peek();

				if (('}' == peeked) || (']' == peeked) || ('\n' == peeked))
					finished = true;
				else if (ss >> c)
				{
					if (('}' == c) || (']' == c))
					{
						ss.putback(c);
						finished = true;
					}
					else
						finished = (',' == c);
				}

				if (finished)
				{
					charBuf.push_back('\0');
					std::string str(charBuf.data());
					std::stringstream ss2(str);

					if ((!str.empty()) && IsAllDigits(str, false))
					{
						if ('-' == str[0])
						{
							try
							{
								const long long v = std::stoll(str);
								if (v >= static_cast<long long>((std::numeric_limits<long>::min)()) &&
									v <= static_cast<long long>((std::numeric_limits<long>::max)()))
									return { std::move(ss), static_cast<long>(v) };
								return { std::move(ss), static_cast<double>(v) };
							}
							catch (const std::exception&)
							{
							}
						}
						else
						{
							try
							{
								const unsigned long long v = std::stoull(str);
								if (v <= static_cast<unsigned long long>((std::numeric_limits<unsigned long>::max)()))
									return { std::move(ss), static_cast<unsigned long>(v) };
								return { std::move(ss), static_cast<double>(v) };
							}
							catch (const std::exception&)
							{
							}
						}
					}
					else if (IsAllDigits(str, true))
					{
						double v;
						if (ss2 >> v)
							return { std::move(ss), v };
					}
					else
					{
						std::vector<char> charBuf2;
						for (auto letter : str)
							charBuf2.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(letter))));

						charBuf2.push_back('\0');
						std::string strLower(charBuf2.data());
						if ("true" == strLower)
							return { std::move(ss), true };
						else if ("false" == strLower)
							return { std::move(ss), false };
					}

					charBuf.push_back('\0');
					return { std::move(ss), std::string(charBuf.data(), charBuf.size() - 1u) };
				}
				else
					charBuf.push_back(c);
			}
		}
	}

	return { std::move(ss), std::nullopt };
}

Json::PartResult Json::ParseJsonPart(std::stringstream ss)
{
	char c;
	JsonPart part;

	while (ss >> c)
	{
		if ('}' == c)
			return { std::move(ss), part };
		else if ('\"' == c)
		{
			auto keyRes = ParseKey(std::move(ss));
			if (keyRes.Key.has_value())
			{
				ss = std::move(keyRes.Stream);

				while (ss >> c)
				{
					if (':' == c)
					{
						auto valueRes = ParseValue(std::move(ss));
						if (valueRes.Value.has_value())
							part.KeyValues[keyRes.Key.value()] = valueRes.Value.value();
						
						ss = std::move(valueRes.Stream);
						break;
					}
				}
			}
		}
	}

	return { std::move(ss), part };
}

Json::JsonArray Json::ParseJsonArray(std::vector<std::string> values)
{
	// Try to parse as bool, long, unsigned long, double, string or struct
	JsonArray jsonArray;
	jsonArray.Length = (unsigned int)values.size();

	// Work out type of the first entry
	auto jsonType = JSON_UNKNOWN;
	if (!values.empty())
	{
		auto firstValue = values[0];
		if (!firstValue.empty())
		{
			if (IsAllDigits(firstValue, false))
			{
				if ('-' == firstValue[0])
					jsonType = JSON_LONG;
				else
					jsonType = JSON_ULONG;
			}
			else if (IsAllDigits(firstValue, true))
				jsonType = JSON_DOUBLE;
			else
			{
				std::vector<char> charBuf;
				for (auto letter : firstValue)
					charBuf.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(letter))));

				charBuf.push_back('\0');
				firstValue = std::string(charBuf.data(), charBuf.size() - 1u);
				if (("true" == firstValue) || ("false" == firstValue))
					jsonType = JSON_BOOL;
				else
					jsonType = JSON_STRING;
			}
		}
	}

	std::vector<bool> boolVec;
	std::vector<long> longVec;
	std::vector<unsigned long> ulongVec;
	std::vector<double> doubleVec;

	switch (jsonType)
	{
	case JSON_BOOL:
		for (auto str : values)
		{
			boolVec.push_back(IsTrue(str));
		}
		jsonArray.Array = boolVec;
		break;
	case JSON_LONG:
		for (auto str : values)
		{
			std::stringstream ss(str);
			long v;
			if (ss >> v)
				longVec.push_back(v);
			else
				longVec.push_back(0);
		}
		jsonArray.Array = longVec;
		break;
	case JSON_ULONG:
		for (auto str : values)
		{
			std::stringstream ss(str);
			unsigned long v;
			if (ss >> v)
				ulongVec.push_back(v);
			else
				ulongVec.push_back(0);
		}
		jsonArray.Array = ulongVec;
		break;
	case JSON_DOUBLE:
		for (auto str : values)
		{
			std::stringstream ss(str);
			double v;
			if (ss >> v)
				doubleVec.push_back(v);
			else
				doubleVec.push_back(0.0);
		}
		jsonArray.Array = doubleVec;
		break;
	case JSON_STRING:
		jsonArray.Array = values;
		break;
	}

	return jsonArray;
}
