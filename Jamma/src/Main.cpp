///////////////////////////////////////////////////////////
//
// Copyright(c) 2018-2024 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#include "NetworkSession.h"
#include "Main.h"
#include "Window.h"
#include "PathUtils.h"
#include "../io/TextReadWriter.h"
#include "../io/InitFile.h"
#include "../io/ConsoleTui.h"
#include "../vst/VstDiagnostics.h"
#include <objbase.h>
#include <memory>
#include <vector>
#include <fstream>
#include <sstream>
#include <string_view>

using namespace engine;
using namespace base;
using namespace resources;
using namespace graphics;
using namespace utils;
using namespace io;

#define MAX_JSON_CHARS 1000000u

namespace
{
	std::optional<std::wstring> ReadEnvironmentVariable(const wchar_t* name)
	{
		const auto required = GetEnvironmentVariableW(name, nullptr, 0);
		if (required == 0)
			return std::nullopt;

		std::wstring value(required - 1, L'\0');
		GetEnvironmentVariableW(name, value.data(), required);
		return value;
	}

	bool ParseBoolText(std::wstring value, bool fallback)
	{
		if (value.empty())
			return fallback;

		std::transform(value.begin(), value.end(), value.begin(), towlower);
		if (value == L"1" || value == L"true" || value == L"yes" || value == L"on")
			return true;
		if (value == L"0" || value == L"false" || value == L"no" || value == L"off")
			return false;
		return fallback;
	}

	unsigned int ParseUnsignedText(const std::wstring& value, unsigned int fallback)
	{
		if (value.empty())
			return fallback;

		try
		{
			return static_cast<unsigned int>(std::stoul(value));
		}
		catch (...)
		{
			return fallback;
		}
	}

	std::wstring DefaultIniPath()
	{
		return GetPath(PATH_ROAMING) + L"\\Jamma\\defaults.json";
	}

	std::wstring ResolveIniPath()
	{
		if (auto initPath = ReadEnvironmentVariable(L"JAMMA_DEFAULTS_PATH"); initPath.has_value() && !initPath->empty())
			return initPath.value();

		return DefaultIniPath();
	}

	vst::DebugOptions ResolveVstDebugOptions(const std::optional<io::InitFile>& defaults)
	{
		vst::DebugOptions options;
		if (defaults.has_value())
			options = defaults->VstDebug;

		if (auto value = ReadEnvironmentVariable(L"JAMMA_VST_DEBUG_AUTO_OPEN"); value.has_value())
			options.AutoOpenEditor = ParseBoolText(value.value(), options.AutoOpenEditor);

		if (auto value = ReadEnvironmentVariable(L"JAMMA_VST_DEBUG_LOG_TO_FILE"); value.has_value())
			options.LogToFile = ParseBoolText(value.value(), options.LogToFile);

		if (auto value = ReadEnvironmentVariable(L"JAMMA_VST_DEBUG_LOG_PATH"); value.has_value() && !value->empty())
			options.LogPath = value.value();

		if (auto value = ReadEnvironmentVariable(L"JAMMA_VST_DEBUG_STATION_INDEX"); value.has_value())
			options.StationIndex = ParseUnsignedText(value.value(), options.StationIndex);

		if (auto value = ReadEnvironmentVariable(L"JAMMA_VST_DEBUG_PLUGIN_INDEX"); value.has_value())
			options.PluginIndex = ParseUnsignedText(value.value(), options.PluginIndex);

		if (options.LogPath.empty())
			options.LogPath = GetPath(PATH_ROAMING) + L"\\Jamma\\vst-diagnostic.log";

		return options;
	}
}

void SetupConsole()
{
	AllocConsole();
	FILE* newStdout = nullptr;
	FILE* newStderr = nullptr;
	FILE* newStdin = nullptr;
	freopen_s(&newStdout, "CONOUT$", "w", stdout);
	freopen_s(&newStderr, "CONOUT$", "w", stderr);
	freopen_s(&newStdin, "CONIN$", "r", stdin);
}

std::optional<io::InitFile> LoadIni(const std::wstring& initPath)
{
	const std::wstring roamingPath = utils::GetParentDirectory(initPath);
	io::TextReadWriter txtFile;
	auto res = txtFile.Read(initPath, MAX_JSON_CHARS);

	std::string iniTxt = InitFile::DefaultJson(EncodeUtf8(roamingPath));
	if (!res.has_value())
		txtFile.Write(initPath, iniTxt, (unsigned int)iniTxt.size(), 0);
	else
	{
		auto [ini, numChars, unused] = res.value();
		iniTxt = ini;
	}

	std::stringstream ss(iniTxt);
	return InitFile::FromStream(std::move(ss));
}

std::optional<io::JamFile> LoadJam(io::InitFile& ini)
{
	io::TextReadWriter txtFile;

	std::string jamJson = JamFile::DefaultJson;
	std::wcout << "Load Jam: " << ini.Jam << std::endl;
	auto res = txtFile.Read(ini.Jam, MAX_JSON_CHARS);
	if (!res.has_value())
	{
		ini.Jam = GetPath(PATH_ROAMING) + L"/Jamma/default.jam";
		txtFile.Write(ini.Jam,
			jamJson,
			(unsigned int)jamJson.size(),
			0);
	}
	else
	{
		auto [str, numChars, unused] = res.value();
		jamJson = str;
	}

	std::stringstream ss(jamJson);
	return JamFile::FromStream(std::move(ss));
}

std::optional<io::RigFile> LoadRig(io::InitFile& ini)
{
	io::TextReadWriter txtFile;

	std::string rigJson = RigFile::DefaultJson;
	auto res = txtFile.Read(ini.Rig, MAX_JSON_CHARS);
	if (!res.has_value())
	{
		ini.Rig = GetPath(PATH_ROAMING) + L"/Jamma/default.rig";
		txtFile.Write(ini.Rig,
			rigJson,
			(unsigned int)rigJson.size(),
			0);
	}
	else
	{
		auto [str, numChars, unused] = res.value();
		rigJson = str;
	}

	std::stringstream ss(rigJson);
	return RigFile::FromStream(std::move(ss));
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow)
{
	SetupConsole();
	// VST3 plug-ins built on VSTGUI (e.g. Valhalla) call RegisterDragDrop /
	// OLE APIs from inside IPlugView::attached(). Use OleInitialize (which
	// also performs CoInitializeEx as STA) so attach() does not deadlock or
	// fail because the host thread isn't OLE-initialized.
	const HRESULT uiComInit = OleInitialize(nullptr);
	const bool uiComInitialized = SUCCEEDED(uiComInit);

	// Bring up the console TUI immediately after SetupConsole() so that
	// ALL application logs (socket init, file load, connection lifecycle, etc.)
	// get the coloured/emoji treatment. The TUI's lifetime spans the entire
	// application run and is independent of any NINJAM session.
	//
	// An atomic scene pointer lets the submit handler forward chat safely once
	// the scene is wired in below; before that it prints a "not connected"
	// notice. std::atomic<Scene*> is write-once from the main thread.
	auto tui = std::make_unique<io::ConsoleTui>();
	std::atomic<Scene*> sceneRaw{ nullptr };
	tui->Start("> ", [&sceneRaw](const std::string& msg) {
		auto* s = sceneRaw.load(std::memory_order_acquire);
		if (s)
			s->SendNinjamChat(msg);
		else
			std::cout << "[NINJAM] Not connected - message not sent" << std::endl;
	});

	NetworkSession socketSession;
	if (!socketSession.IsInitialised())
	{
		std::cerr << "[NINJAM] Failed to initialise socket library" << std::endl;
		return -1;
	}

	const auto initPath = ResolveIniPath();
	auto defaults = LoadIni(initPath);
	const auto vstDebugOptions = ResolveVstDebugOptions(defaults);
	vst::VstDiagnostics::Configure(vstDebugOptions, GetPath(PATH_ROAMING) + L"\\Jamma\\vst-diagnostic.log");
	vst::VstDiagnostics::Log("Main", "startup", std::string("initPath=") + utils::EncodeUtf8(initPath)
		+ ", autoOpen=" + (vstDebugOptions.AutoOpenEditor ? "true" : "false")
		+ ", logToFile=" + (vstDebugOptions.LogToFile ? "true" : "false")
		+ ", stationIndex=" + std::to_string(vstDebugOptions.StationIndex)
		+ ", pluginIndex=" + std::to_string(vstDebugOptions.PluginIndex));

	SceneParams sceneParams(DrawableParams{ "" },
		MoveableParams{ {0, 0}, {0, 0, 0}, 1.0 },
		SizeableParams{ 1400, 1000 });
	JamFile jam;
	RigFile rig;

	if (defaults.has_value())
	{
		sceneParams.Position = defaults.value().WinPos;
		sceneParams.Size = defaults.value().WinSize;

		std::stringstream ss;
		InitFile::ToStream(defaults.value(), ss);

		auto jamOpt = LoadJam(defaults.value());
		if (jamOpt.has_value())
			jam = jamOpt.value();

		JamFile::ToStream(jam, ss);

		auto rigOpt = LoadRig(defaults.value());
		if (rigOpt.has_value())
			rig = rigOpt.value();

		RigFile::ToStream(rig, ss);

		std::cout << ss.str() << std::endl;
	}

	const auto jamDirectory = defaults.has_value() ?
		utils::GetParentDirectory(defaults->Jam) :
		utils::GetParentDirectory(initPath);
	auto scene = Scene::FromFile(sceneParams, jam, rig, jamDirectory, vstDebugOptions);
	if (!scene.has_value())
	{
		std::cout << "Failed to create Scene... quitting" << std::endl;
		vst::VstDiagnostics::Log("Main", "scene-create-failed");
		return -1;
	}

	// Wire the scene pointer so the TUI submit handler can forward chat.
	sceneRaw.store(scene.value().get(), std::memory_order_release);

	ResourceLib resourceLib;
	Window window(*(scene.value()), resourceLib);

	if (window.Create(hInstance, nCmdShow) != 0)
		PostQuitMessage(1);

	scene.value()->InitAudio();

	MSG msg;
	bool active = true;
	while (active)
	{
		while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
		{
			if (msg.message == WM_QUIT)
			{
				scene.value()->CloseAudio();
				active = false;
				break;
			}

			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}

		if (!active)
			break;

		window.Render();
		window.Swap();
	}

	window.Release();

	// Stop the TUI before returning so console mode and cout/cerr rdbufs
	// are fully restored before the CRT shuts down.
	tui->Stop();

	if (uiComInitialized)
		OleUninitialize();

	return (int)msg.wParam;
}
