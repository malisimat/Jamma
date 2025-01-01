///////////////////////////////////////////////////////////
//
// Author 2019 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#pragma once

#include <vector>
#include <stack>
#include <map>
#include <optional>
#include <variant>
#include <iostream>
#include <sstream>

namespace io
{
	class Json
	{
	public:
		struct JsonArray;
		struct JsonPart;
		typedef std::variant<bool, long, unsigned long, double, std::string, JsonArray, JsonPart> JsonValue;
		typedef std::variant<bool, long, unsigned long, double, std::string> JsonScalar;
		typedef std::variant<std::vector<bool>, std::vector<long>, std::vector<unsigned long>, std::vector<double>, std::vector<std::string>, std::vector<JsonPart>> JsonArrayType;

		struct JsonArray
		{
			unsigned int Length;
			JsonArrayType Array;
		};

		struct JsonPart
		{
			std::map<std::string, JsonValue> KeyValues;
		};

		static std::optional<JsonValue> FromStream(std::stringstream ss);
		static bool ToStream(JsonValue json, std::stringstream ss);

		static bool IsAllDigits(std::string str, bool includePeriod);
		static bool IsTrue(std::string str);

	protected:
		enum JsonScalarType
		{
			JSON_UNKNOWN,
			JSON_BOOL,
			JSON_LONG,
			JSON_ULONG,
			JSON_DOUBLE,
			JSON_STRING,
			JSON_STRUCT
		};

		struct KeyResult
		{
			std::stringstream Stream;
			std::optional<std::string> Key;
		};

		struct ValueResult
		{
			std::stringstream Stream;
			std::optional<JsonValue> Value;

		};

		struct PartResult
		{
			std::stringstream Stream;
			std::optional<JsonPart> Part;
		};

		static KeyResult ParseKey(std::stringstream ss);
		static ValueResult ParseValue(std::stringstream ss);
		static PartResult ParseJsonPart(std::stringstream ss);
		static JsonArray ParseJsonArray(std::vector<std::string> values);
	};
}
