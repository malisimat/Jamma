#include "../../../../JammaLib/src/utils/PathUtils.h"
#include "../../TestPaths.h"
#include <gtest/gtest.h>
#include <fstream>
#include <vector>

class DialogDirectoryFiles : public testing::Test
{
protected:
	std::filesystem::path Directory;
	void SetUp() override
	{
		Directory = TestPaths::FixtureDirectory() / (L"Jamma-dialog-directory-" +
			std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
		ASSERT_TRUE(std::filesystem::create_directory(Directory));
	}
	void TearDown() override { std::error_code error; std::filesystem::remove_all(Directory, error); }
};

TEST_F(DialogDirectoryFiles, ExistingUnicodeParentDoesNotRequireJamFileToExist)
{
	const auto folder = Directory / L"sessions-\u6f14\u594f";
	ASSERT_TRUE(std::filesystem::create_directory(folder));
	EXPECT_TRUE(utils::IsUsableDialogDirectory(utils::GetParentDirectory((folder / L"missing.jam").wstring())));
	std::filesystem::remove(folder);
	EXPECT_FALSE(utils::IsUsableDialogDirectory(folder.wstring()));
}

TEST_F(DialogDirectoryFiles, UnusableDirectoriesFallBackWithoutFilesystemExceptions)
{
	const auto file = Directory / L"file.jam";
	{ std::ofstream output(file); output << "test"; }
	const std::vector<std::wstring> candidates{
		L"", L".", L"relative/sessions", L"C:relative", file.wstring(),
		(Directory / L"missing").wstring(), (file / L"child").wstring(),
		(Directory / L"invalid<>directory").wstring(),
		Directory.wstring() + std::wstring(1u, L'\0') + L"ignored"
	};
	for (const auto& candidate : candidates)
	{
		SCOPED_TRACE(candidate);
		EXPECT_NO_THROW(EXPECT_FALSE(utils::IsUsableDialogDirectory(candidate)));
	}
}
