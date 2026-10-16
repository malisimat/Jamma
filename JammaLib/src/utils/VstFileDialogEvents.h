#pragma once
#include <Windows.h>
#include <shobjidl_core.h>
#include <wrl.h>
#include <filesystem>
#include <iostream>
#include <string>
namespace utils
{
	// UI-only COM event owner. Advise holds a reference until Unadvise; this
	// handler retains no dialog pointer, so it cannot form a reference cycle.
	class VstFileDialogEvents final : public Microsoft::WRL::RuntimeClass<
		Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IFileDialogEvents>
	{
	public:
		const std::wstring& SelectedBundle() const noexcept { return _selectedBundle; }
		bool ShouldSelectBundle(const std::wstring& path) const
		{
			if (!_opened) return false; // Initial remembered folder is not a user selection.
			std::filesystem::path candidate(path);
			while (candidate.filename().empty() && candidate.parent_path() != candidate)
				candidate = candidate.parent_path();
			if (_wcsicmp(candidate.extension().c_str(), L".vst3") != 0) return false;
			std::error_code error;
			return std::filesystem::is_directory(candidate, error) && !error;
		}
		HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog*) override { _opened = true; return S_OK; }
		HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog* dialog, IShellItem* folder) override
		{
			if (!_opened || !dialog || !folder) return S_OK;
			PWSTR raw = nullptr;
			if (FAILED(folder->GetDisplayName(SIGDN_FILESYSPATH, &raw))) return S_OK;
			try
			{
				std::wstring path(raw);
				CoTaskMemFree(raw); raw = nullptr;
				if (!ShouldSelectBundle(path)) return S_OK;
				_selectedBundle = std::move(path);
				const auto closed = dialog->Close(S_OK);
				if (SUCCEEDED(closed)) return S_FALSE;
				_selectedBundle.clear();
				std::cerr << "[ERROR][VST picker] selecting bundle failed HRESULT=" << closed << std::endl;
				return S_OK; // Keep file navigation usable if shell close failed.
			}
			catch (...)
			{
				CoTaskMemFree(raw);
				_selectedBundle.clear();
				std::cerr << "[ERROR][VST picker] unexpected bundle-navigation exception; direct file navigation remains available" << std::endl;
				return S_OK;
			}
		}
		HRESULT STDMETHODCALLTYPE OnFileOk(IFileDialog*) override { return S_OK; }
		HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog*) override { return E_NOTIMPL; }
		HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog*) override { return E_NOTIMPL; }
		HRESULT STDMETHODCALLTYPE OnShareViolation(IFileDialog*, IShellItem*, FDE_SHAREVIOLATION_RESPONSE*) override { return E_NOTIMPL; }
		HRESULT STDMETHODCALLTYPE OnOverwrite(IFileDialog*, IShellItem*, FDE_OVERWRITE_RESPONSE*) override { return E_NOTIMPL; }
	private:
		bool _opened = false;
		std::wstring _selectedBundle;
	};
}
