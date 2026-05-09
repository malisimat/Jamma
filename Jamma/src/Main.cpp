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
#include <objbase.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>
#include <fstream>
#include <sstream>

using namespace engine;
using namespace base;
using namespace resources;
using namespace graphics;
using namespace utils;
using namespace io;

#define MAX_JSON_CHARS 1000000u

namespace
{
	bool ReadEnvFlag(const wchar_t* name)
	{
		wchar_t value[32]{};
		const auto len = GetEnvironmentVariableW(name, value, static_cast<DWORD>(_countof(value)));
		if (len == 0 || len >= _countof(value))
			return false;

		return (_wcsicmp(value, L"0") != 0)
			&& (_wcsicmp(value, L"false") != 0)
			&& (_wcsicmp(value, L"off") != 0)
			&& (_wcsicmp(value, L"no") != 0);
	}

	std::string BuildDebugStatus(const engine::Scene& scene,
		uint64_t renderHeartbeat,
		bool autoOpenEnabled,
		uint64_t autoOpenAttempts,
		bool autoOpenSucceeded)
	{
		auto snapshot = scene.GetDebugSnapshot();

		std::ostringstream ss;
		ss << "render_heartbeat=" << renderHeartbeat << '\n';
		ss << "audio_callback_count=" << snapshot.AudioCallbackCount << '\n';
		ss << "job_tick_count=" << snapshot.JobTickCount << '\n';
		ss << "scene_vst_editor_open_attempts=" << snapshot.VstEditorOpenAttempts << '\n';
		ss << "scene_vst_editor_open_successes=" << snapshot.VstEditorOpenSuccesses << '\n';
		ss << "auto_open_enabled=" << (autoOpenEnabled ? 1 : 0) << '\n';
		ss << "auto_open_attempts=" << autoOpenAttempts << '\n';
		ss << "auto_open_succeeded=" << (autoOpenSucceeded ? 1 : 0) << '\n';
		return ss.str();
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

std::optional<io::InitFile> LoadIni()
{
	std::wstring roamingPath = GetPath(PATH_ROAMING) + L"\\Jamma";
	std::wstring initPath = roamingPath + L"/defaults.json";
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

	auto defaults = LoadIni();

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

	auto scene = Scene::FromFile(sceneParams, jam, rig, utils::GetParentDirectory(defaults.value().Jam));
	if (!scene.has_value())
	{
		std::cout << "Failed to create Scene... quitting" << std::endl;
		return -1;
	}

	// Wire the scene pointer so the TUI submit handler can forward chat.
	sceneRaw.store(scene.value().get(), std::memory_order_release);

	ResourceLib resourceLib;
	Window window(*(scene.value()), resourceLib);
	const bool debugEnabled = ReadEnvFlag(L"JAMMA_VST_DEBUG");
	const bool debugAutoOpenEnabled = ReadEnvFlag(L"JAMMA_VST_DEBUG_AUTO_OPEN");
	const auto debugDir = GetPath(PATH_ROAMING) + L"\\Jamma";
	const auto debugStatusPath = debugDir + L"\\debug-status.txt";
	std::atomic<uint64_t> renderHeartbeat{ 0 };
	std::atomic<uint64_t> autoOpenAttempts{ 0 };
	std::atomic<bool> autoOpenSucceeded{ false };
	std::atomic<bool> debugThreadQuit{ false };
	std::thread debugWriter;

	if (window.Create(hInstance, nCmdShow) != 0)
		PostQuitMessage(1);
	
	scene.value()->InitAudio();

	if (debugEnabled)
	{
		std::wcout << L"[Debug] Live status path: " << debugStatusPath << std::endl;
		debugWriter = std::thread([&]() {
			io::TextReadWriter writer;
			while (!debugThreadQuit.load(std::memory_order_relaxed))
			{
				auto status = BuildDebugStatus(*scene.value(),
					renderHeartbeat.load(std::memory_order_relaxed),
					debugAutoOpenEnabled,
					autoOpenAttempts.load(std::memory_order_relaxed),
					autoOpenSucceeded.load(std::memory_order_relaxed));
				writer.Write(debugStatusPath, status, static_cast<unsigned int>(status.size()), 0);
				std::this_thread::sleep_for(std::chrono::milliseconds(250));
			}

			auto status = BuildDebugStatus(*scene.value(),
				renderHeartbeat.load(std::memory_order_relaxed),
				debugAutoOpenEnabled,
				autoOpenAttempts.load(std::memory_order_relaxed),
				autoOpenSucceeded.load(std::memory_order_relaxed));
			writer.Write(debugStatusPath, status, static_cast<unsigned int>(status.size()), 0);
		});
	}

	MSG msg;
	bool active = true;
	uint64_t nextAutoOpenFrame = 30;
	uint64_t nextUiHeartbeatFrame = 300;
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

		const auto currentRenderHeartbeat = renderHeartbeat.load(std::memory_order_relaxed);
		if (debugEnabled && currentRenderHeartbeat >= nextUiHeartbeatFrame)
		{
			auto snapshot = scene.value()->GetDebugSnapshot();
			std::cout << "[Debug] UI still pumping: render=" << currentRenderHeartbeat
				<< ", audio=" << snapshot.AudioCallbackCount
				<< ", job=" << snapshot.JobTickCount
				<< std::endl;
			nextUiHeartbeatFrame += 300;
		}

		if (debugAutoOpenEnabled
			&& !autoOpenSucceeded.load(std::memory_order_relaxed)
			&& currentRenderHeartbeat >= nextAutoOpenFrame)
		{
			autoOpenAttempts.fetch_add(1, std::memory_order_relaxed);
			const bool opened = scene.value()->TryOpenFirstAvailableVstEditor();
			autoOpenSucceeded.store(opened, std::memory_order_relaxed);
			nextAutoOpenFrame += 120;
		}

		window.Render();
		renderHeartbeat.fetch_add(1, std::memory_order_relaxed);
		window.Swap();
	}

	debugThreadQuit.store(true, std::memory_order_relaxed);
	if (debugWriter.joinable())
		debugWriter.join();

	window.Release();

	// Stop the TUI before returning so console mode and cout/cerr rdbufs
	// are fully restored before the CRT shuts down.
	tui->Stop();

	if (uiComInitialized)
		OleUninitialize();

	return (int)msg.wParam;
}
