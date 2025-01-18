///////////////////////////////////////////////////////////
//
// Author 2019 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#include "Json.h"

using namespace io;

std::optional<Json::JsonValue> Json::FromStream(std::stringstream ss)
{
	auto root = ParseValue(std::move(ss));

	return root.Value;
}

bool Json::ToStream(Json::JsonValue json, std::stringstream ss)
{
	return false;
}

bool Json::IsAllDigits(std::string str, bool includePeriod)
{
	auto isFirstChar = true;

	for (auto ch : str)
	{
		if (!std::isdigit(ch))
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
		charBuf.push_back(std::tolower(letter));

	charBuf.push_back('\0');
	auto strLower = std::string(charBuf.data());
	return "true" == strLower;
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
			key = std::string(charBuf.data());
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
			std::vector<std::string> values;

			while (ss >> c)
			{
				if (' ' == c)
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
					continue;

				isArrayEnd = ']' == c;
				if ((',' == c) || isArrayEnd)
				{
					charBuf.push_back('\0');
					values.push_back(std::string(charBuf.data()));

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

			while (ss >> c)
			{
				if ('"' == c)
				{
					charBuf.push_back('\0');
					return { std::move(ss), std::string(charBuf.data()) };
				}
				else
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

				if (('}' == peeked) || ('\n' == peeked))
					finished = true;
				else if (ss >> c)
					finished = (',' == c);

				if (finished)
				{
					charBuf.push_back('\0');
					std::string str(charBuf.data());
					std::stringstream ss2(str);

					if ((!str.empty()) && IsAllDigits(str, false))
					{
						if ('-' == str[0])
						{
							long v;
							if (ss2 >> v)
								return { std::move(ss), v };
						}
						else
						{
							unsigned long v;
							if (ss2 >> v)
								return { std::move(ss), unsigned long(v) };
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
							charBuf2.push_back(std::tolower(letter));

						charBuf2.push_back('\0');
						std::string strLower(charBuf2.data());
						if ("true" == strLower)
							return { std::move(ss), true };
						else if ("false" == strLower)
							return { std::move(ss), false };
					}

					charBuf.push_back('\0');
					return { std::move(ss), std::string(charBuf.data()) };
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
					charBuf.push_back(std::tolower(letter));

				charBuf.push_back('\0');
				firstValue = std::string(charBuf.data());
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
