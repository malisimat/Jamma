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
	struct Result
	{
		bool RigSaved = false, DefaultsSaved = false, ChangesDefaults = false;
		io::InitFile Defaults;
		std::wstring Path;
		std::uint64_t Revision = 0u;
	};
	RigDefaultSave(std::wstring directory, std::wstring defaultsPath, RigWriter rigWriter, DefaultsWriter defaultsWriter)
		: _directory(std::move(directory)), _defaultsPath(std::move(defaultsPath)),
				_writeRig(std::move(rigWriter)), _writeDefaults(std::move(defaultsWriter)) {}
	static io::RigFile WithHardwarePreferences(const io::RigFile& saved, const io::RigFile& live)
	{
		// Hardware choices do not publish runtime membership, triggers or routing.
		auto updated = saved;
		updated.User.Audio = live.User.Audio;
		updated.User.Midi.Devices = live.User.Midi.Devices;
		return updated;
	}
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
	void InitializeCurrentPath(const io::InitFile& defaults, bool userRig, std::uint64_t revision)
	{
		if (userRig && defaults.RigLoadType == io::InitFile::LOAD_SPECIFIC &&
			ValidFilename(defaults.Rig, _directory)) { _currentPath = defaults.Rig; _savedRevision = revision; }
	}
	const std::wstring& CurrentPath() const { return _currentPath; }
	std::uint64_t SavedRevision() const { return _savedRevision; }
	void InvalidateSavedRevision() { _savedRevision = 0u; } // A replacement Scene starts its own revision sequence.
	bool Dirty(std::uint64_t revision) const { return _currentPath.empty() || revision != _savedRevision; }
	void AcceptSuccessfulWrite(const Result& result)
	{
		if (!result.RigSaved || (result.ChangesDefaults && !result.DefaultsSaved)) return;
		_currentPath = result.Path; _savedRevision = result.Revision;
	}
	void Start(engine::Scene& scene, HWND owner, const io::InitFile& defaults,
		engine::Scene::RigSaveMode mode = engine::Scene::RigSaveMode::StartupDefault)
	{
		if (_busy) return;
		std::wstring target = _currentPath;
		const bool setDefault = mode == engine::Scene::RigSaveMode::StartupDefault;
		const bool choose = mode != engine::Scene::RigSaveMode::Save || target.empty() || !ValidFilename(target, _directory);
		scene.SetRigSaveDialogOpen(true);
		std::error_code error;
		std::filesystem::create_directories(_directory, error);
		if (error) { scene.SetRigSaveDialogOpen(false); MessageBoxW(owner, L"Cannot create the Jamma rig directory.", L"Save rig", MB_OK | MB_ICONERROR); return; }
		wchar_t filename[32768] = L"My rig.rig";
		if (!_currentPath.empty()) wcscpy_s(filename, std::filesystem::path(_currentPath).filename().c_str());
		OPENFILENAMEW dialog{};
		dialog.lStructSize = sizeof(dialog); dialog.hwndOwner = owner;
		dialog.lpstrFilter = L"Jamma rig (*.rig)\0*.rig\0\0";
		dialog.lpstrFile = filename; dialog.nMaxFile = static_cast<DWORD>(std::size(filename));
		dialog.lpstrInitialDir = _directory.c_str(); dialog.lpstrDefExt = L"rig";
		dialog.lpstrTitle = setDefault ? L"Save rig as startup default" : L"Save rig as";
		dialog.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST;
		if (choose && !GetSaveFileNameW(&dialog)) { scene.SetRigSaveDialogOpen(false); return; }
		if (choose) target = filename;
		if (!ValidFilename(std::filesystem::path(target), std::filesystem::path(_directory)))
		{ scene.SetRigSaveDialogOpen(false); MessageBoxW(owner, L"Choose a .rig filename in the Jamma folder. Reserved filenames are unavailable.", L"Save rig", MB_OK | MB_ICONWARNING); return; }
		scene.SetRigSaveDialogOpen(false);
		const auto rig = scene.BeginExplicitRigSave();
		if (!rig) { MessageBoxW(owner, L"A station or rig change is applying. Please save again when it finishes.", L"Save rig", MB_OK); return; }
		auto updated = defaults;
		if (setDefault) { updated.Rig = target; updated.RigLoadType = io::InitFile::LOAD_SPECIFIC; updated.RigOrigin = "user"; }
		_busy = true;
		scene.QueueExplicitRigSave([this, capture = *rig, updated = std::move(updated), target = std::move(target), setDefault]() {
			auto result = WriteRequest(capture.Rig, target, capture.Revision, setDefault, updated, _defaultsPath, _writeRig, _writeDefaults);
			_result.store(std::move(result), std::memory_order_release);
		});
	}
	static std::shared_ptr<Result> WriteRequest(const io::RigFile& rig, const std::wstring& path,
		std::uint64_t revision, bool changesDefaults, const io::InitFile& defaults, const std::wstring& defaultsPath,
		const RigWriter& writeRig, const DefaultsWriter& writeDefaults)
	{
		auto result = std::make_shared<Result>(); result->Defaults = defaults;
		result->Path = path; result->Revision = revision; result->ChangesDefaults = changesDefaults;
		try
		{
			result->RigSaved = writeRig(path, rig);
			if (result->RigSaved && changesDefaults)
			{
				std::stringstream json;
				result->DefaultsSaved = io::InitFile::ToStream(defaults, json) && writeDefaults(defaultsPath, json.str());
			}
		}
		catch (...) { /* Preserve the successfully completed steps. */ }
		return result;
	}
	static std::shared_ptr<Result> WriteCaptured(const io::RigFile& rig, const io::InitFile& defaults,
		const std::wstring& defaultsPath, const RigWriter& writeRig, const DefaultsWriter& writeDefaults)
	{
		return WriteRequest(rig, defaults.Rig, 0u, true, defaults, defaultsPath, writeRig, writeDefaults);
	}
	bool Poll(engine::Scene& scene, HWND owner, io::InitFile& defaults, bool show = true)
	{
		const auto result = _result.exchange({}, std::memory_order_acq_rel);
		if (!result) return false;
		_busy = false; scene.FinishExplicitRigSave();
		AcceptSuccessfulWrite(*result);
		if (result->DefaultsSaved) defaults = result->Defaults;
		std::wstring message = result->DefaultsSaved ? L"Rig saved as the startup default." :
			result->RigSaved ? L"Rig saved; startup default unchanged." : L"Rig could not be saved. Current path and startup default unchanged.";
		if (result->RigSaved)
		{
			message += L"\nSaved accepted rig revision " + std::to_wstring(result->Revision) + L".";
			const auto now = scene.CaptureCommittedRigVersion();
			if (!now || now->Revision != result->Revision) message += L" Newer changes remain unsaved.";
		}
		if (show) MessageBoxW(owner, message.c_str(), L"Save rig", MB_OK | (result->RigSaved ? MB_ICONINFORMATION : MB_ICONWARNING));
		return result->DefaultsSaved;
	}
	bool Busy() const noexcept { return _busy; }
private:
	std::wstring _directory, _defaultsPath;
	std::wstring _currentPath; // Shell/window owner; changes only on a complete successful explicit save.
	std::uint64_t _savedRevision = 0u;
	RigWriter _writeRig;
	DefaultsWriter _writeDefaults;
	bool _busy = false; // UI owner only.
	std::atomic<std::shared_ptr<const Result>> _result;
};
