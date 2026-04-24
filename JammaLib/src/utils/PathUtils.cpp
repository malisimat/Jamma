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

	// Initialise COM for this call if not already done on this thread.
	struct ComGuard {
		bool initialised;
		ComGuard() { initialised = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)); }
		~ComGuard() { if (initialised) CoUninitialize(); }
	} comGuard;

	IFileOpenDialog* pfd = nullptr;
	if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
	                            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd))))
		return result;

	DWORD opts = 0;
	pfd->GetOptions(&opts);
	pfd->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
	pfd->SetTitle(title.c_str());

	if (SUCCEEDED(pfd->Show(nullptr)))
	{
		IShellItem* psi = nullptr;
		if (SUCCEEDED(pfd->GetResult(&psi)))
		{
			PWSTR path = nullptr;
			if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &path)))
			{
				result = path;
				CoTaskMemFree(path);
			}
			psi->Release();
		}
	}
	pfd->Release();
	return result;
}
