#include "../../../../JammaLib/src/utils/VstFileDialogEvents.h"
#include "../../../../JammaLib/src/vst/Vst3Plugin.h"
#include "../../../../Jamma/src/ConsoleCapture.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include "../../TestPaths.h"
namespace vst
{
	struct Vst3PluginTestAccess
	{
		static std::wstring Resolve(const std::wstring& path, std::error_code& error) { return Vst3Plugin::ResolveModulePath(path, error); }
		static RenderRole InferRole(RenderRole reported, int inputBuses, int outputChannels)
		{ return Vst3Plugin::InferRoleFromAudioBuses(reported, inputBuses, outputChannels); }
	};
}
TEST(Vst3Role, MissingMetadataUsesAudioBusShape)
{
	using vst::RenderRole;
	using Access = vst::Vst3PluginTestAccess;
	EXPECT_EQ(RenderRole::Instrument, Access::InferRole(RenderRole::Unknown, 0, 2));
	EXPECT_EQ(RenderRole::Effect, Access::InferRole(RenderRole::Unknown, 1, 2));
	EXPECT_EQ(RenderRole::Unknown, Access::InferRole(RenderRole::Unknown, 1, 0));
	EXPECT_EQ(RenderRole::Instrument, Access::InferRole(RenderRole::Instrument, 1, 2));
}
class Vst3ModuleFiles : public testing::Test
{
protected:
	std::filesystem::path Directory;
	void SetUp() override
	{
		Directory = TestPaths::FixtureDirectory() / (L"Jamma-vst3-module-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
		std::filesystem::create_directories(Directory);
	}
	void TearDown() override { std::error_code error; std::filesystem::remove_all(Directory, error); }
};
TEST_F(Vst3ModuleFiles, Vst3ResolvesUnicodeBundleTrailingSeparatorAndDirectFiles)
{
	const auto bundle = Directory / L"Synth-\u97f3.vst3";
	const auto module = bundle / L"Contents" / L"x86_64-win" / bundle.filename();
	std::filesystem::create_directories(module.parent_path());
	{ std::ofstream file(module); file << "test"; }
	std::error_code error;
	EXPECT_EQ(vst::Vst3PluginTestAccess::Resolve(bundle.wstring(), error), module.wstring()); EXPECT_FALSE(error);
	EXPECT_EQ(vst::Vst3PluginTestAccess::Resolve(bundle.wstring() + L"\\", error), module.wstring()); EXPECT_FALSE(error);
	EXPECT_EQ(vst::Vst3PluginTestAccess::Resolve(module.wstring(), error), module.wstring()); EXPECT_FALSE(error);
	const auto missing = Directory / L"missing.vst3";
	EXPECT_EQ(vst::Vst3PluginTestAccess::Resolve(missing.wstring(), error), missing.wstring());
	const auto malformed = Directory / L"Malformed.vst3";
	std::filesystem::create_directories(malformed);
	EXPECT_EQ(vst::Vst3PluginTestAccess::Resolve(malformed.wstring(), error), (malformed / L"Contents" / L"x86_64-win" / malformed.filename()).wstring());
}
TEST_F(Vst3ModuleFiles, Vst3PreInitAndLoadFallbackReportRequestedResolvedAndNativeFailure)
{
	const auto bundle = Directory / L"Bad.vst3";
	std::filesystem::create_directories(bundle);
	console::LineRing ring;
	{
		console::ConsoleCapture capture(ring);
		vst::Vst3Plugin preinit;
		EXPECT_FALSE(preinit.PreInit(bundle.wstring()));
		vst::Vst3Plugin fallback;
		EXPECT_FALSE(fallback.Load(bundle.wstring(), 48000, 128, 2));
	}
	bool preinit = false, fallback = false;
	while (auto line = ring.Take())
	{
		if (line->find("[ERROR][VST3]") == std::string::npos) continue;
		EXPECT_NE(line->find("requested='"), std::string::npos);
		EXPECT_NE(line->find("resolved='"), std::string::npos);
		EXPECT_NE(line->find("Contents\\x86_64-win\\Bad.vst3"), std::string::npos);
		EXPECT_NE(line->find("windows-error="), std::string::npos);
		preinit |= line->find("stage=PreInit/LoadLibraryW") != std::string::npos;
		fallback |= line->find("stage=Load/LoadLibraryW") != std::string::npos;
	}
	EXPECT_TRUE(preinit); EXPECT_TRUE(fallback);
}

TEST_F(Vst3ModuleFiles, Vst3InvalidInternalModuleFailsRecoverablyAtBothEntryPoints)
{
	const auto bundle = Directory / L"WrongArchitecture.vst3";
	const auto module = bundle / L"Contents" / L"x86_64-win" / bundle.filename();
	std::filesystem::create_directories(module.parent_path());
	{ std::ofstream file(module); file << "not a Windows module"; }
	console::LineRing ring;
	{
		console::ConsoleCapture capture(ring);
		vst::Vst3Plugin plugin;
		EXPECT_FALSE(plugin.PreInit(bundle.wstring()));
		EXPECT_FALSE(plugin.IsLoaded());
		EXPECT_FALSE(plugin.Load(bundle.wstring(), 48000, 128, 2));
		plugin.Unload();
	}
	bool nativeBadImage = false;
	while (auto line = ring.Take()) nativeBadImage |= line->find("windows-error=193") != std::string::npos;
	EXPECT_TRUE(nativeBadImage);
}

TEST_F(Vst3ModuleFiles, PickerIgnoresInitialFolderAndSelectsOnlyDeliberateBundleNavigation)
{
	const auto bundle = Directory / L"Synth.VST3";
	std::filesystem::create_directories(bundle);
	const auto direct = Directory / L"Direct.vst3";
	{ std::ofstream file(direct); file << "test"; }
	auto events = Microsoft::WRL::Make<utils::VstFileDialogEvents>();
	ASSERT_TRUE(events);
	EXPECT_FALSE(events->ShouldSelectBundle(bundle.wstring()));
	EXPECT_EQ(events->OnFolderChanging(nullptr, nullptr), S_OK);
	EXPECT_EQ(events->OnFolderChange(nullptr), S_OK);
	EXPECT_TRUE(events->ShouldSelectBundle(bundle.wstring()));
	EXPECT_TRUE(events->ShouldSelectBundle(bundle.wstring() + L"\\"));
	EXPECT_FALSE(events->ShouldSelectBundle(direct.wstring()));
	EXPECT_FALSE(events->ShouldSelectBundle(Directory.wstring()));
	EXPECT_FALSE(events->ShouldSelectBundle((Directory / L"Missing.vst3").wstring()));
	EXPECT_EQ(events->OnFileOk(nullptr), S_OK); // Direct-file results remain accepted.
	EXPECT_TRUE(events->SelectedBundle().empty());
}
