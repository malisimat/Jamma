///////////////////////////////////////////////////////////
//
// Copyright(c) 2018-2024 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#include <windows.h>
#include <shobjidl_core.h>
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
	std::wstring result;

	struct ComGuard
	{
		bool DidInit = false;
		ComGuard()
		{
			auto hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
			DidInit = SUCCEEDED(hr);
		}
		~ComGuard()
		{
			if (DidInit)
				CoUninitialize();
		}
	} comGuard;

	IFileOpenDialog* dialog = nullptr;
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
		IShellItem* item = nullptr;
		if (SUCCEEDED(dialog->GetResult(&item)))
		{
			PWSTR path = nullptr;
			if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
			{
				result = path;
				CoTaskMemFree(path);
			}
			item->Release();
		}
	}

	dialog->Release();
	return result;
}
