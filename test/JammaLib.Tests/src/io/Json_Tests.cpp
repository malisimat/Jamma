
#include "gtest/gtest.h"
#include <regex>
#include "resources/ResourceLib.h"
#include "io/Json.h"

using io::Json;

class JsonMock :
	public Json
{
public:
	static std::optional<Json::JsonPart> FromStream(std::stringstream ss)
	{
		return std::get<Json::JsonPart>(Json::FromStream(std::move(ss)).value());
	}
};

TEST(Json, ParsesBool) {
	auto str = "{\"bool\":true}";
	auto testStream = std::stringstream(str);
	auto json = JsonMock::FromStream(std::move(testStream));

	ASSERT_TRUE(json.has_value());
	ASSERT_FALSE(json.value().KeyValues.empty());

	auto kv = json.value().KeyValues["bool"];
	auto res = std::get<bool>(kv);

	ASSERT_EQ(true,res);
}

TEST(Json, ParsesInt) {
	auto str = "{\"int\":-12}";
	auto testStream = std::stringstream(str);
	auto json = JsonMock::FromStream(std::move(testStream));

	ASSERT_TRUE(json.has_value());
	ASSERT_FALSE(json.value().KeyValues.empty());

	auto kv = json.value().KeyValues["int"];
	auto res = (int)std::get<long>(kv);

	ASSERT_EQ(-12, res);
}

TEST(Json, ParsesUnsignedLong) {
	auto str = "{\"ulong\":2147483648}";
	auto testStream = std::stringstream(str);
	auto json = JsonMock::FromStream(std::move(testStream));

	ASSERT_TRUE(json.has_value());
	ASSERT_FALSE(json.value().KeyValues.empty());

	auto kv = json.value().KeyValues["ulong"];
	auto res = std::get<unsigned long>(kv);

	ASSERT_EQ(2147483648ul, res);
}

TEST(Json, ParsesDouble) {
	auto str = "{\"double\":0.34}";
	auto testStream = std::stringstream(str);
	auto json = JsonMock::FromStream(std::move(testStream));

	ASSERT_TRUE(json.has_value());
	ASSERT_FALSE(json.value().KeyValues.empty());

	auto kv = json.value().KeyValues["double"];
	auto res = std::get<double>(kv);

	ASSERT_EQ(0.34, res);
}

TEST(Json, ParsesString) {
	auto str = "{\"string\":\"hello\"}";
	auto testStream = std::stringstream(str);
	auto json = JsonMock::FromStream(std::move(testStream));

	ASSERT_TRUE(json.has_value());
	ASSERT_FALSE(json.value().KeyValues.empty());

	auto kv = json.value().KeyValues["string"];
	auto res = std::get<std::string>(kv);

	ASSERT_EQ("hello", res);
}

TEST(Json, ParsesStringInt) {
	auto str = "{\"string\":\"hello\",\"int\":-7}";
	auto testStream = std::stringstream(str);
	auto json = JsonMock::FromStream(std::move(testStream));

	ASSERT_TRUE(json.has_value());
	ASSERT_FALSE(json.value().KeyValues.empty());

	auto kv1 = json.value().KeyValues["string"];
	auto res1 = std::get<std::string>(kv1);
	auto kv2 = json.value().KeyValues["int"];
	auto res2 = (int)std::get<long>(kv2);

	ASSERT_EQ("hello", res1);
	ASSERT_EQ(-7, res2);
}

TEST(Json, ParsesBoolArray) {
	auto str = "{\"arr\":[TrUe,fAlsE]}";
	auto testStream = std::stringstream(str);
	auto json = JsonMock::FromStream(std::move(testStream));

	ASSERT_TRUE(json.has_value());
	ASSERT_FALSE(json.value().KeyValues.empty());

	auto kv = json.value().KeyValues["arr"];
	auto g = std::get<Json::JsonArray>(kv);
	auto res = std::get<std::vector<bool>>(g.Array);

	ASSERT_EQ(2, res.size());
	ASSERT_EQ(true, res[0]);
	ASSERT_EQ(false, res[1]);
}

TEST(Json, ParsesIntArray) {
	auto str = "{\"arr\":[-3,4]}";
	auto testStream = std::stringstream(str);
	auto json = JsonMock::FromStream(std::move(testStream));

	ASSERT_TRUE(json.has_value());
	ASSERT_FALSE(json.value().KeyValues.empty());

	auto kv = json.value().KeyValues["arr"];
	auto g = std::get<Json::JsonArray>(kv);
	auto res = std::get<std::vector<long>>(g.Array);

	ASSERT_EQ(2, res.size());
	ASSERT_EQ(-3, (int)res[0]);
	ASSERT_EQ(4, (int)res[1]);
}

TEST(Json, ParsesUnsignedLongArray) {
	auto str = "{\"arr\":[333,444]}";
	auto testStream = std::stringstream(str);
	auto json = JsonMock::FromStream(std::move(testStream));

	ASSERT_TRUE(json.has_value());
	ASSERT_FALSE(json.value().KeyValues.empty());

	auto kv = json.value().KeyValues["arr"];
	auto g = std::get<Json::JsonArray>(kv);
	auto res = std::get<std::vector<unsigned long>>(g.Array);

	ASSERT_EQ(2, res.size());
	ASSERT_EQ(333ul, res[0]);
	ASSERT_EQ(444ul, res[1]);
}

TEST(Json, ParsesDoubleArray) {
	auto str = "{\"arr\":[1.1,2.2]}";
	auto testStream = std::stringstream(str);
	auto json = JsonMock::FromStream(std::move(testStream));

	ASSERT_TRUE(json.has_value());
	ASSERT_FALSE(json.value().KeyValues.empty());

	auto kv = json.value().KeyValues["arr"];
	auto g = std::get<Json::JsonArray>(kv);
	auto res = std::get<std::vector<double>>(g.Array);

	ASSERT_EQ(2, res.size());
	ASSERT_EQ(1.1, res[0]);
	ASSERT_EQ(2.2, res[1]);
}

TEST(Json, ParsesStringArray) {
	auto str = "{\"arr\":[\"hello\",\"you\"]}";
	auto testStream = std::stringstream(str);
	auto json = JsonMock::FromStream(std::move(testStream));

	ASSERT_TRUE(json.has_value());
	ASSERT_FALSE(json.value().KeyValues.empty());

	auto kv = json.value().KeyValues["arr"];
	auto g = std::get<Json::JsonArray>(kv);
	auto res = std::get<std::vector<std::string>>(g.Array);

	ASSERT_EQ(2, res.size());
	ASSERT_EQ("hello", res[0]);
	ASSERT_EQ("you", res[1]);
}

TEST(Json, ParsesStruct) {
	auto str = "{\"struct\":{\"name\":\"you\",\"business\":\"monkeying\"}}";
	auto testStream = std::stringstream(str);
	auto json = JsonMock::FromStream(std::move(testStream));

	ASSERT_TRUE(json.has_value());
	ASSERT_FALSE(json.value().KeyValues.empty());

	auto kv = json.value().KeyValues["struct"];
	auto h = std::get<Json::JsonPart>(kv);
	auto res1 = std::get<std::string>(h.KeyValues["name"]);
	auto res2 = std::get<std::string>(h.KeyValues["business"]);

	ASSERT_EQ("you", res1);
	ASSERT_EQ("monkeying", res2);
}

TEST(Json, ParsesJamFile) {
	static const char* jamJson = R"(
		{
			"name": "default",
			"stations": [
				{ "name": "Mic", "takes": [] },
				{ "name": "HiHat", "takes": [] }
			],
			"quantisesamps": 0,
			"quantisation": "multiple"
		}
	)";

	auto testStream = std::stringstream(jamJson);
	auto json = JsonMock::FromStream(std::move(testStream));

	ASSERT_TRUE(json.has_value());

	auto kv = json.value().KeyValues["stations"];
	auto arr = std::get<Json::JsonArray>(kv);
	auto stations = std::get<std::vector<Json::JsonPart>>(arr.Array);

	ASSERT_EQ(2u, stations.size());

	auto name0 = std::get<std::string>(stations[0].KeyValues["name"]);
	auto name1 = std::get<std::string>(stations[1].KeyValues["name"]);

	ASSERT_EQ("Mic", name0);
	ASSERT_EQ("HiHat", name1);
}

TEST(Json, ParsesJamFileWithVst) {
	static const char* jamJson = R"(
		{
			"name": "default",
			"stations": [
				{ "name": "Mic", "takes": [], "vst": [{ "path": "C:/VST/test.vst3", "bypass": false }] },
				{ "name": "HiHat", "takes": [] }
			],
			"quantisesamps": 0,
			"quantisation": "multiple"
		}
	)";

	auto testStream = std::stringstream(jamJson);
	auto json = JsonMock::FromStream(std::move(testStream));

	ASSERT_TRUE(json.has_value());

	auto kv = json.value().KeyValues["stations"];
	auto arr = std::get<Json::JsonArray>(kv);
	auto stations = std::get<std::vector<Json::JsonPart>>(arr.Array);

	ASSERT_EQ(2u, stations.size());

	auto name0 = std::get<std::string>(stations[0].KeyValues["name"]);
	auto name1 = std::get<std::string>(stations[1].KeyValues["name"]);

	ASSERT_EQ("Mic", name0);
	ASSERT_EQ("HiHat", name1);

	auto vstKv = stations[0].KeyValues["vst"];
	auto vstArr = std::get<Json::JsonArray>(vstKv);
	auto vstParts = std::get<std::vector<Json::JsonPart>>(vstArr.Array);

	ASSERT_EQ(1u, vstParts.size());

	auto path = std::get<std::string>(vstParts[0].KeyValues["path"]);
	auto bypass = std::get<bool>(vstParts[0].KeyValues["bypass"]);

	ASSERT_EQ("C:/VST/test.vst3", path);
	ASSERT_EQ(false, bypass);
}
