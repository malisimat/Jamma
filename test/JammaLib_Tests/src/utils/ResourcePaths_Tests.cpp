#include "../../../../JammaLib/src/resources/ResourcePaths.h"
#include "../../../../JammaLib/src/resources/WavResource.h"
#include "../../../../JammaLib/src/utils/StringUtils.h"
#include "../../../../JammaLib/src/utils/ImageUtils.h"
#include "../../TestPaths.h"
#include <Windows.h>
#include <gtest/gtest.h>
#include <array>
#include <fstream>

class ResourcePathFiles : public testing::Test
{
protected:
	std::filesystem::path OriginalDirectory;
	std::filesystem::path Directory;
	std::filesystem::path ExecutableDirectory;

	void SetUp() override
	{
		OriginalDirectory = std::filesystem::current_path();
		Directory = TestPaths::FixtureDirectory() / ("resource-paths-" +
			std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
		ASSERT_TRUE(std::filesystem::create_directory(Directory));
		std::array<wchar_t, 32768> executable{};
		const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
		ASSERT_GT(length, 0u);
		ASSERT_LT(length, executable.size());
		ExecutableDirectory = std::filesystem::path(executable.data()).parent_path();
	}

	void TearDown() override
	{
		std::error_code error;
		std::filesystem::current_path(OriginalDirectory, error);
		std::filesystem::remove_all(Directory, error);
	}
};

TEST_F(ResourcePathFiles, ConflictingWorkingDirectoryCannotOverrideExecutableResources)
{
	std::filesystem::create_directories(Directory / "resources");
	std::filesystem::create_directories(Directory / "Jamma" / "resources");
	for (const auto& folder : { Directory, Directory / "resources", Directory / "Jamma" / "resources" })
		std::ofstream(folder / "ResourceList.txt") << "conflicting resource list";
	std::filesystem::current_path(Directory);
	EXPECT_EQ(std::filesystem::path(utils::DecodeUtf8(resources::ResolveResourceListPath())),
		ExecutableDirectory / "resources" / "ResourceList.txt");
	EXPECT_EQ(std::filesystem::current_path(), Directory);
}

TEST_F(ResourcePathFiles, NestedAndMissingResourcesRemainAnchoredToExecutable)
{
	std::filesystem::current_path(Directory);
	EXPECT_EQ(std::filesystem::path(utils::DecodeUtf8(resources::ResolveResourcePath("textures/missing.tga"))),
		ExecutableDirectory / "resources" / "textures" / "missing.tga");
}

TEST_F(ResourcePathFiles, UnicodeResourceNamesRoundTripThroughUtf8)
{
	const std::filesystem::path relative = L"textures/\u6f14\u594f.tga";
	EXPECT_EQ(std::filesystem::path(utils::DecodeUtf8(resources::ResolveResourcePath(utils::EncodeUtf8(relative.wstring())))),
		ExecutableDirectory / "resources" / relative);
}

TEST_F(ResourcePathFiles, TextureReaderOpensUtf8Paths)
{
	const auto file = Directory / L"\u6f14\u594f.tga";
	// One uncompressed 24-bit pixel: an 18-byte TGA header followed by BGR.
	const std::array<unsigned char, 21> tga{ 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 24, 0, 10, 20, 30 };
	{ std::ofstream output(file, std::ios::binary); output.write(reinterpret_cast<const char*>(tga.data()), tga.size()); }
	const auto loaded = utils::ImageUtils::LoadTga(utils::EncodeUtf8(file.wstring()));
	ASSERT_TRUE(loaded.has_value());
	EXPECT_EQ(std::get<1>(*loaded), 1u);
	EXPECT_EQ(std::get<2>(*loaded), 1u);
}

TEST_F(ResourcePathFiles, WavReaderAndWriterRoundTripUnicodePaths)
{
	const auto unicode = Directory / L"\u6f14\u594f.wav";
	const std::vector<float> samples{ 0.0f, 0.25f, -0.25f, 0.0f };
	ASSERT_TRUE(io::WavReadWriter().Write(unicode.wstring(), samples, static_cast<unsigned int>(samples.size()), 44100u));
	const auto loaded = resources::WavResource::Load(utils::EncodeUtf8(unicode.wstring()));
	ASSERT_TRUE(loaded.has_value());
	EXPECT_EQ(std::get<1>(*loaded), samples.size());
	EXPECT_EQ(std::get<2>(*loaded), 44100u);
	ASSERT_EQ(std::get<0>(*loaded).size(), samples.size());
	for (std::size_t index = 0; index < samples.size(); ++index)
		EXPECT_NEAR(std::get<0>(*loaded)[index], samples[index], 1.0f / 32767.0f);
}
