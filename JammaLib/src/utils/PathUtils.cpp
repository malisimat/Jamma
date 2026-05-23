///////////////////////////////////////////////////////////
//
// Copyright(c) 2018-2024 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#include <windows.h>
#include <shobjidl_core.h>
#include <wrl/client.h>
#include "PathUtils.h"

std::wstring utils::GetPath(PathType pathType)
{
	switch (pathType)
	{
	case PATH_ROAMING:
		LPWSTR path = NULL;
		auto hr = SHGetKnownFolderPath(FOLDERID_RoamingAppData,
			KF_FLAG_CREATE,
			nullptr,
			&path);

		if (SUCCEEDED(hr))
			return std::wstring(path);
	}

	return std::wstring();
}

std::wstring utils::GetParentDirectory(std::wstring dir)
{
	std::filesystem::path p(dir);
	return p.parent_path();
}

std::wstring utils::PickDirectory(const std::wstring& title)
{
	using Microsoft::WRL::ComPtr;

	std::wstring result;

	struct ComInitGuard
	{
		bool DidInit = false;
		ComInitGuard()
		{
			auto hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
			DidInit = SUCCEEDED(hr);
		}
		~ComInitGuard()
		{
			if (DidInit)
				CoUninitialize();
		}
	} comInitGuard;

	ComPtr<IFileOpenDialog> dialog;
	if (FAILED(CoCreateInstance(CLSID_FileOpenDialog,
		nullptr,
		CLSCTX_INPROC_SERVER,
		IID_PPV_ARGS(&dialog))))
		return result;

	DWORD options = 0;
	dialog->GetOptions(&options);
	dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
	dialog->SetTitle(title.c_str());

	if (SUCCEEDED(dialog->Show(nullptr)))
	{
		ComPtr<IShellItem> item;
		if (SUCCEEDED(dialog->GetResult(&item)))
		{
			PWSTR path = nullptr;
			if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
			{
				result = path;
				CoTaskMemFree(path);
			}
		}
	}

	return result;
}

std::wstring utils::PickFile(const std::wstring& title)
{
	using Microsoft::WRL::ComPtr;

	std::wstring result;

	struct ComInitGuard
	{
		bool DidInit = false;
		ComInitGuard()
		{
			auto hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
			DidInit = SUCCEEDED(hr);
		}
		~ComInitGuard()
		{
			if (DidInit)
				CoUninitialize();
		}
	} comInitGuard;

	ComPtr<IFileOpenDialog> dialog;
	if (FAILED(CoCreateInstance(CLSID_FileOpenDialog,
		nullptr,
		CLSCTX_INPROC_SERVER,
		IID_PPV_ARGS(&dialog))))
		return result;

	COMDLG_FILTERSPEC filters[] =
	{
		{ L"VST plugins (*.vst3;*.dll)", L"*.vst3;*.dll" },
		{ L"VST3 plugins (*.vst3)", L"*.vst3" },
		{ L"DLL files (*.dll)", L"*.dll" },
		{ L"All files (*.*)", L"*.*" }
	};

	DWORD options = 0;
	dialog->GetOptions(&options);
	dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
	dialog->SetTitle(title.c_str());
	dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
	dialog->SetFileTypeIndex(1);

	if (SUCCEEDED(dialog->Show(nullptr)))
	{
		ComPtr<IShellItem> item;
		if (SUCCEEDED(dialog->GetResult(&item)))
		{
			PWSTR path = nullptr;
			if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
			{
				result = path;
				CoTaskMemFree(path);
			}
		}
	}

	return result;
}
