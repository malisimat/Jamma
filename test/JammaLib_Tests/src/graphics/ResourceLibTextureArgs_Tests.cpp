#include "gtest/gtest.h"
#include "resources/ResourceLib.h"

#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

using resources::ResourceLib;

TEST(ResourceLibTextureArgsTest, AcceptsNoArgsAsNormalTexture)
{
	bool isNinePatch = true;
	unsigned int borderX = 99;
	unsigned int borderY = 77;

	const auto parsed = ResourceLib::ParseTextureArgs({}, isNinePatch, borderX, borderY);

	ASSERT_TRUE(parsed);
	EXPECT_FALSE(isNinePatch);
	EXPECT_EQ(0u, borderX);
	EXPECT_EQ(0u, borderY);
}

TEST(ResourceLibTextureArgsTest, AcceptsStrictNinePatchArgs)
{
	bool isNinePatch = false;
	unsigned int borderX = 0;
	unsigned int borderY = 0;

	const auto parsed = ResourceLib::ParseTextureArgs({ "ninepatch", "12", "34" }, isNinePatch, borderX, borderY);

	ASSERT_TRUE(parsed);
	EXPECT_TRUE(isNinePatch);
	EXPECT_EQ(12u, borderX);
	EXPECT_EQ(34u, borderY);
}

TEST(ResourceLibTextureArgsTest, RejectsMalformedArgs)
{
	bool isNinePatch = false;
	unsigned int borderX = 0;
	unsigned int borderY = 0;

	EXPECT_FALSE(ResourceLib::ParseTextureArgs({ "ninepatch", "12" }, isNinePatch, borderX, borderY));
	EXPECT_FALSE(ResourceLib::ParseTextureArgs({ "ninepatch", "12", "x" }, isNinePatch, borderX, borderY));
	EXPECT_FALSE(ResourceLib::ParseTextureArgs({ "ninepatch", "-1", "2" }, isNinePatch, borderX, borderY));
	EXPECT_FALSE(ResourceLib::ParseTextureArgs({ "ninepatch", "1", "2", "3" }, isNinePatch, borderX, borderY));
	EXPECT_FALSE(ResourceLib::ParseTextureArgs({ "stretch", "1", "2" }, isNinePatch, borderX, borderY));
}

TEST(ResourceLibShaderUniforms, MidiEditorContextUniformsAreRegistered)
{
	// ShaderResource uploads context values only for names in ResourceList.
	// A valid, linked shader can silently render incorrectly without this entry.
	auto root = std::filesystem::absolute(__FILE__);
	for (int parent = 0; parent < 5; ++parent) root = root.parent_path();
	const auto resources = root / "Jamma" / "resources";
	std::ifstream list(resources / "ResourceList.txt");
	ASSERT_TRUE(list.is_open());
	std::set<std::string> registered;
	std::string line;
	while (std::getline(list, line))
	{
		std::istringstream fields(line);
		std::string type, name, uniform;
		fields >> type >> name;
		if (type != "2" || name != "midi_note") continue;
		while (fields >> uniform) registered.insert(uniform);
	}
	ASSERT_FALSE(registered.empty());
	const std::regex declaration(R"(^[ \t]*uniform[ \t]+\w+[ \t]+(\w+)[ \t]*;)",
		std::regex::ECMAScript | std::regex::multiline);
	for (const auto* filename : {"midi_note.vert", "midi_note.frag"})
	{
		std::ifstream shader(resources / "shaders" / filename);
		ASSERT_TRUE(shader.is_open());
		std::ostringstream contents; contents << shader.rdbuf();
		const auto source = contents.str();
		for (auto match = std::sregex_iterator(source.begin(), source.end(), declaration);
			match != std::sregex_iterator(); ++match)
		{
			const auto uniform = (*match)[1].str();
			// DrawMesh assigns these per draw directly via glUniform, not context.
			if (uniform == "GeometryPass" || uniform == "EditorWrapCopy") continue;
			EXPECT_TRUE(registered.contains(uniform)) << filename << ": " << uniform;
		}
	}
}
