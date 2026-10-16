#pragma once

#include <filesystem>
#include <stdexcept>

class TestPaths
{
public:
	static std::filesystem::path FixtureDirectory()
	{
		// /FC keeps this header path absolute in the native test build.
		auto repo = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
		if (!repo.is_absolute() || !std::filesystem::is_regular_file(repo / "Jamma.sln"))
			throw std::runtime_error("Cannot locate the test worktree; refusing external fixture I/O");

		repo = std::filesystem::canonical(repo);
		const auto parent = std::filesystem::canonical(
			repo / "test" / "JammaLib_Tests" / "obj");
		RequireInside(repo, parent);
		const auto directory = parent / "fixtures";
		std::filesystem::create_directory(directory);
		const auto resolved = std::filesystem::canonical(directory);
		RequireInside(repo, resolved);
		return resolved;
	}

private:
	static void RequireInside(const std::filesystem::path& repo, const std::filesystem::path& directory)
	{
		const auto relative = directory.lexically_relative(repo);
		if (relative.empty() || relative.is_absolute() || *relative.begin() == "..")
			throw std::runtime_error("Test fixture directory escapes the worktree");
	}
};
