#pragma once
#include <windows.h>
#include <commdlg.h>
#include <filesystem>
#include <atomic>
#include <functional>
#include <memory>
#include "../../JammaLib/src/engine/Scene.h"
#include "../../JammaLib/src/io/InitFile.h"
#pragma comment(lib, "Comdlg32.lib")

// UI owns dialogs, in-memory defaults and completion. Existing Scene job owner
// writes detached values; its immutable result is handed back atomically.
// Main drains Scene shutdown before consuming the final result or destroying us.
class RigDefaultSave
{
public:
	using RigWriter = std::function<bool(const std::wstring&, const io::RigFile&)>;
	using DefaultsWriter = std::function<bool(const std::wstring&, const std::string&)>;
	struct Result { bool RigSaved = false; bool DefaultsSaved = false; io::InitFile Defaults; };
	RigDefaultSave(std::wstring directory, std::wstring defaultsPath, RigWriter rigWriter, DefaultsWriter defaultsWriter)
		: _directory(std::move(directory)), _defaultsPath(std::move(defaultsPath)),
				_writeRig(std::move(rigWriter)), _writeDefaults(std::move(defaultsWriter)) {}
	static bool ValidFilename(const std::filesystem::path& path, const std::filesystem::path& directory)
	{
		for (const auto& part : path) if (part == L"..") return false;
		if (path.parent_path().lexically_normal() != directory.lexically_normal() || path.extension() != L".rig") return false;
		const auto name = path.filename().wstring();
		if (name.empty() || name.find_first_of(L"<>:\"/\\|?*") != std::wstring::npos) return false;
		auto stem = path.stem().wstring();
		std::transform(stem.begin(), stem.end(), stem.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
		if (stem.empty() || stem == L"defaults" || stem.ends_with(L".tmp") || stem.back() == L'.' || stem.back() == L' ') return false;
		return stem != L"con" && stem != L"prn" && stem != L"aux" && stem != L"nul" &&
			!(stem.size() == 4u && (stem.starts_with(L"com") || stem.starts_with(L"lpt")) && stem[3] >= L'1' && stem[3] <= L'9');
	}
	void Start(engine::Scene& scene, HWND owner, const io::InitFile& defaults)
	{
		if (_busy) return;
		scene.SetRigSaveDialogOpen(true);
		std::error_code error;
		std::filesystem::create_directories(_directory, error);
		if (error) { scene.SetRigSaveDialogOpen(false); MessageBoxW(owner, L"Cannot create the Jamma rig directory.", L"Save rig", MB_OK | MB_ICONERROR); return; }
		wchar_t filename[32768] = L"My rig.rig";
		OPENFILENAMEW dialog{};
		dialog.lStructSize = sizeof(dialog); dialog.hwndOwner = owner;
		dialog.lpstrFilter = L"Jamma rig (*.rig)\0*.rig\0\0";
		dialog.lpstrFile = filename; dialog.nMaxFile = static_cast<DWORD>(std::size(filename));
		dialog.lpstrInitialDir = _directory.c_str(); dialog.lpstrDefExt = L"rig";
		dialog.lpstrTitle = L"Save rig as default";
		dialog.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST;
		if (!GetSaveFileNameW(&dialog)) { scene.SetRigSaveDialogOpen(false); return; }
		if (!ValidFilename(std::filesystem::path(filename), std::filesystem::path(_directory)))
		{ scene.SetRigSaveDialogOpen(false); MessageBoxW(owner, L"Choose a .rig filename in the Jamma folder. Reserved filenames are unavailable.", L"Save rig", MB_OK | MB_ICONWARNING); return; }
		const auto rig = scene.CaptureCommittedRig();
		scene.SetRigSaveDialogOpen(false);
		if (!rig) { MessageBoxW(owner, L"A station or rig change is applying. Please save again when it finishes.", L"Save rig", MB_OK); return; }
		auto updated = defaults;
		updated.Rig = filename; updated.RigLoadType = io::InitFile::LOAD_SPECIFIC; updated.RigOrigin = "user";
		_busy = true;
		scene.QueueExplicitRigSave([this, rig = *rig, updated = std::move(updated)]() {
			auto result = WriteCaptured(rig, updated, _defaultsPath, _writeRig, _writeDefaults);
			_result.store(std::move(result), std::memory_order_release);
		});
	}
	static std::shared_ptr<Result> WriteCaptured(const io::RigFile& rig, const io::InitFile& defaults,
		const std::wstring& defaultsPath, const RigWriter& writeRig, const DefaultsWriter& writeDefaults)
	{
		auto result = std::make_shared<Result>(); result->Defaults = defaults;
		try
		{
			result->RigSaved = writeRig(defaults.Rig, rig);
			if (result->RigSaved)
			{
				std::stringstream json;
				result->DefaultsSaved = io::InitFile::ToStream(defaults, json) && writeDefaults(defaultsPath, json.str());
			}
		}
		catch (...) { /* Preserve the successfully completed steps. */ }
		return result;
	}
	bool Poll(engine::Scene& scene, HWND owner, io::InitFile& defaults, bool show = true)
	{
		const auto result = _result.exchange({}, std::memory_order_acq_rel);
		if (!result) return false;
		_busy = false; scene.FinishExplicitRigSave();
		if (result->DefaultsSaved) defaults = result->Defaults;
		if (show) MessageBoxW(owner, result->DefaultsSaved ? L"Rig saved as the startup default." :
			result->RigSaved ? L"Rig saved; startup default unchanged." : L"Rig could not be saved. Startup default unchanged.",
			L"Save rig", MB_OK | (result->DefaultsSaved ? MB_ICONINFORMATION : MB_ICONWARNING));
		return result->DefaultsSaved;
	}
	bool Busy() const noexcept { return _busy; }
private:
	std::wstring _directory, _defaultsPath;
	RigWriter _writeRig;
	DefaultsWriter _writeDefaults;
	bool _busy = false; // UI owner only.
	std::atomic<std::shared_ptr<const Result>> _result;
};
