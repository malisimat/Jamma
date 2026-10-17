#include "../../../../JammaLib/src/utils/VstFileDialogEvents.h"
#include "../../../../JammaLib/src/vst/Vst3Plugin.h"
#include "../../../../Jamma/src/ConsoleCapture.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <thread>
#include "../../TestPaths.h"
namespace vst
{
	struct Vst3PluginTestAccess
	{
		static std::wstring Resolve(const std::wstring& path, std::error_code& error) { return Vst3Plugin::ResolveModulePath(path, error); }
	};
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
#include <Shlobj.h>
#include <Oleidl.h>
class NativeVstDialogRun
{
public:
	enum class Action { NavigateBundle, AcceptFile, Cancel };
	static void Phase(const char* phase) { std::cerr << "[native-dialog-test] ms=" << GetTickCount64() << " thread=" << GetCurrentThreadId() << " phase=" << phase << std::endl; }
	struct Result
	{
		HRESULT Shown = E_FAIL;
		HRESULT FileResult = E_FAIL;
		std::wstring Bundle, File;
		bool TimedOut = false;
	};
	static Result Run(const std::filesystem::path& initial, const std::filesystem::path& target, Action action)
	{
		// Earlier native-window tests can leave thread-queue messages behind.
		// Give each modal dialog its own STA and timer queue, as a real UI owner.
		Result result;
		std::thread owner([&] { result = RunOnSta(initial, target, action); });
		owner.join();
		return result;
	}
private:
	static Result RunOnSta(const std::filesystem::path& initial, const std::filesystem::path& target, Action action)
	{
		Result result;
		Phase("CoInitializeEx/begin");
		const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
		Phase("CoInitializeEx/end");
		if (FAILED(initialized)) { result.Shown = initialized; return result; }
		struct ComLifetime { ~ComLifetime() { Phase("CoUninitialize/begin"); CoUninitialize(); Phase("CoUninitialize/end"); } } com;
		Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
		Phase("CoCreateInstance/begin");
		result.Shown = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
		Phase("CoCreateInstance/end");
		if (FAILED(result.Shown)) return result;
		GUID isolatedClient{};
		result.Shown = CoCreateGuid(&isolatedClient);
		if (FAILED(result.Shown)) return result;
		result.Shown = dialog->SetClientGuid(isolatedClient);
		if (FAILED(result.Shown)) return result;
		dialog->SetOptions(FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_DONTADDTORECENT);
		Microsoft::WRL::ComPtr<IShellItem> initialFolder;
		result.Shown = SHCreateItemFromParsingName(initial.c_str(), nullptr, IID_PPV_ARGS(&initialFolder));
		if (FAILED(result.Shown)) return result;
		Phase("SetInitialFolder/begin");
		dialog->SetFolder(initialFolder.Get());
		Phase("SetInitialFolder/end");
		if (action == Action::AcceptFile || (!target.empty() && action == Action::Cancel))
			dialog->SetFileName(target.filename().c_str());
		auto events = Microsoft::WRL::Make<utils::VstFileDialogEvents>();
		DWORD cookie = 0;
		result.Shown = dialog->Advise(events.Get(), &cookie);
		if (FAILED(result.Shown)) return result;
		{
			NativeVstDialogRun script(dialog.Get(), events.Get(), target, action, result);
			Phase("Show/begin");
			result.Shown = script._timer ? dialog->Show(nullptr) : E_FAIL;
			Phase("Show/end");
		}
		result.Bundle = events->SelectedBundle();
		Microsoft::WRL::ComPtr<IShellItem> selected;
		if (SUCCEEDED(result.Shown)) result.FileResult = dialog->GetResult(&selected);
		if (SUCCEEDED(result.FileResult))
		{
			PWSTR raw = nullptr;
			if (SUCCEEDED(selected->GetDisplayName(SIGDN_FILESYSPATH, &raw)))
			{ result.File = raw; CoTaskMemFree(raw); }
		}
		Phase("Unadvise/begin");
		dialog->Unadvise(cookie);
		Phase("Unadvise/end");
		Phase("ClearClientData/begin");
		dialog->ClearClientData();
		Phase("ClearClientData/end");
		Phase("DialogRelease/begin");
		dialog.Reset();
		Phase("DialogRelease/end");
		return result;
	}
private:
	NativeVstDialogRun(IFileDialog* dialog, utils::VstFileDialogEvents* events, std::filesystem::path target, Action action, Result& result)
		: _dialog(dialog), _events(events), _target(std::move(target)), _action(action), _result(result)
	{
		_active = this;
		_timer = SetTimer(nullptr, 0, 20, &OnTimer);
	}
	~NativeVstDialogRun() { if (_timer) KillTimer(nullptr, _timer); _active = nullptr; }
	static void CALLBACK OnTimer(HWND, UINT, UINT_PTR timer, DWORD)
	{
		auto* script = _active;
		if (!script || script->_timer != timer) return;
		if (++script->_ticks >= 150)
		{
			script->_result.TimedOut = true;
			script->_dialog->Close(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
			return;
		}
		if (script->_ticks < 4 || script->_ticks % 4) return;
		if (script->_action == Action::Cancel)
		{
			// Exercise the user's close action on the dialog's owning STA. Close()
			// can return success from this timer before Show actually dismisses it.
			Microsoft::WRL::ComPtr<IOleWindow> window;
			HWND handle = nullptr;
			if (!script->_cancelPosted && SUCCEEDED(script->_dialog->QueryInterface(IID_PPV_ARGS(&window))) &&
				SUCCEEDED(window->GetWindow(&handle)) && IsWindowVisible(handle) && PostMessageW(handle, WM_CLOSE, 0, 0))
			{
				Phase("CancelWindowClose/posted");
				script->_cancelPosted = true;
			}
			return; // Keep the watchdog until Show returns and the owner kills it.
		}
		if (script->_action == Action::NavigateBundle)
		{
			if (!script->_events->ShouldSelectBundle(script->_target.wstring())) return;
			Microsoft::WRL::ComPtr<IShellItem> folder;
			const auto parsed = SHCreateItemFromParsingName(script->_target.c_str(), nullptr, IID_PPV_ARGS(&folder));
			if (FAILED(parsed)) { script->_dialog->Close(parsed); return; }
			script->_dialog->SetFolder(folder.Get());
			return;
		}
		Microsoft::WRL::ComPtr<IOleWindow> window;
		HWND handle = nullptr;
		if (SUCCEEDED(script->_dialog->QueryInterface(IID_PPV_ARGS(&window))) && SUCCEEDED(window->GetWindow(&handle)))
			PostMessageW(handle, WM_COMMAND, IDOK, 0);
	}
	// Non-owning pointers remain on the dialog's STA; timer is killed before Show's
	// owners leave scope. The 3s timer closes/cancels an unattended modal dialog.
	IFileDialog* _dialog;
	utils::VstFileDialogEvents* _events;
	std::filesystem::path _target;
	Action _action;
	Result& _result;
	UINT_PTR _timer = 0;
	unsigned _ticks = 0;
	bool _cancelPosted = false;
	static inline thread_local NativeVstDialogRun* _active = nullptr;
};
TEST_F(Vst3ModuleFiles, DISABLED_NativePickerNavigationClosesWithOuterBundleAndNoFileResult)
{
	const auto bundle = Directory / L"NativeBundle.vst3";
	std::filesystem::create_directories(bundle);
	const auto result = NativeVstDialogRun::Run(Directory, bundle, NativeVstDialogRun::Action::NavigateBundle);
	EXPECT_FALSE(result.TimedOut);
	EXPECT_EQ(result.Shown, S_OK);
	EXPECT_EQ(result.Bundle, bundle.wstring());
	EXPECT_TRUE(FAILED(result.FileResult));
}
TEST_F(Vst3ModuleFiles, DISABLED_NativePickerRememberedBundleCanBeCancelledWithoutSelection)
{
	const auto bundle = Directory / L"RememberedBundle.vst3";
	std::filesystem::create_directories(bundle);
	const auto result = NativeVstDialogRun::Run(bundle, {}, NativeVstDialogRun::Action::Cancel);
	EXPECT_FALSE(result.TimedOut);
	EXPECT_EQ(result.Shown, HRESULT_FROM_WIN32(ERROR_CANCELLED));
	EXPECT_TRUE(result.Bundle.empty());
	EXPECT_TRUE(result.File.empty());
}
TEST_F(Vst3ModuleFiles, DISABLED_NativePickerDirectFileCanBeSelectedNormally)
{
	const auto direct = Directory / L"Direct.vst3";
	{ std::ofstream file(direct); file << "test module"; }
	const auto result = NativeVstDialogRun::Run(Directory, direct, NativeVstDialogRun::Action::AcceptFile);
	EXPECT_FALSE(result.TimedOut);
	EXPECT_EQ(result.Shown, S_OK);
	EXPECT_EQ(result.File, direct.wstring());
	EXPECT_TRUE(result.Bundle.empty());
}
