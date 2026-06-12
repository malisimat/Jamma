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
#include "../ninjam/NinjamSession.h"
#include "../io/TextReadWriter.h"
#include "../io/InitFile.h"
#include "../io/ConsoleTui.h"
#include "../vst/Vst3Plugin.h"
#include <objbase.h>
#include <atomic>
#include <cctype>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <string_view>

using namespace engine;
using namespace base;
using namespace resources;
using namespace graphics;
using namespace utils;

// ---------------------------------------------------------------------------
// Known public NINJAM servers
// ---------------------------------------------------------------------------
namespace
{
	void PrintNinjamHelp()
	{
		auto snapshot = ninjam::NinjamSession::GetPublicServerDirectorySnapshot();
		auto servers = ninjam::NinjamSession::GetReachablePublicServers();
		std::cout << "[NINJAM] Commands:\n"
		          << "[NINJAM]   /  /?  /help        Show this help and server list\n"
		          << "[NINJAM]   /c <n>  /connect <n> Connect to server by number\n"
		          << "[NINJAM]   /d  /q  /quit        Disconnect from current server\n"
		          << "[NINJAM] Servers:\n";
		if (snapshot.RefreshInFlight)
			std::cout << "[NINJAM]   Refreshing live metadata from autosong.ninjam.com...\n";

		for (std::size_t i = 0; i < servers.size(); ++i)
		{
			std::cout << "[NINJAM]   " << (i + 1) << ". "
			          << servers[i].Host
			          << ninjam::NinjamSession::FormatPublicServerSummary(servers[i])
			          << "\n";
		}
		std::cout << std::flush;
	}

	// Returns true when the message was a slash command (consumed; should NOT
	// be forwarded as chat). Returns false for ordinary chat text.
	bool HandleSlashCommand(const std::string& msg, Scene* scene)
	{
		if (msg.empty() || msg[0] != '/')
			return false;

		const std::string rest = msg.substr(1);
		const auto sp = rest.find(' ');
		std::string verb = (sp == std::string::npos) ? rest : rest.substr(0, sp);
		std::string args = (sp == std::string::npos) ? std::string{} : rest.substr(sp + 1);

		for (auto& c : verb)
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		while (!args.empty() && args.front() == ' ')
			args.erase(0, 1);

		if (verb.empty() || verb == "?" || verb == "help")
		{
			const auto snapshot = ninjam::NinjamSession::GetPublicServerDirectorySnapshot();
			const bool refreshStarted = ninjam::NinjamSession::RefreshPublicServerDirectoryAsync(PrintNinjamHelp);
			if (refreshStarted || snapshot.RefreshInFlight || !snapshot.HasLiveData)
			{
				std::cout << "[NINJAM] Refreshing live metadata from autosong.ninjam.com..." << std::endl;
			}
			else
			{
				PrintNinjamHelp();
			}
			return true;
		}

		if (verb == "c" || verb == "connect")
		{
			if (args.empty())
			{
				std::cout << "[NINJAM] Usage: /c <number>  (type / for list)" << std::endl;
				return true;
			}
			int idx = 0;
			try { idx = std::stoi(args); }
			catch (const std::exception&) { idx = 0; }

			auto snapshot = ninjam::NinjamSession::GetPublicServerDirectorySnapshot();
			auto servers = ninjam::NinjamSession::GetReachablePublicServers();
			const auto serverCount = static_cast<int>(servers.size());

			if (serverCount == 0)
			{
				std::cout << "[NINJAM] No reachable servers in the current list  (type / to refresh)" << std::endl;
				return true;
			}

			if (idx < 1 || idx > serverCount)
			{
				std::cout << "[NINJAM] Server number must be 1-" << serverCount
				          << "  (type / for list)" << std::endl;
				return true;
			}
			if (scene)
				scene->ConnectNinjam(servers[idx - 1].Host);
			else
				std::cout << "[NINJAM] Not ready yet" << std::endl;
			return true;
		}

		if (verb == "d" || verb == "q" || verb == "quit"
			|| verb == "exit" || verb == "disconnect")
		{
			if (scene)
				scene->DisconnectNinjam();
			else
				std::cout << "[NINJAM] Not connected" << std::endl;
			return true;
		}

		std::cout << "[NINJAM] Unknown command /" << verb
		          << "  (type / for help)" << std::endl;
		return true;
	}
} // namespace
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

	bool IsWindowPlacementVisible(const utils::Position2d& position, const utils::Size2d& size)
	{
		RECT workArea{};
		if (!SystemParametersInfo(SPI_GETWORKAREA, 0, &workArea, 0))
			return true;

		const LONG left = static_cast<LONG>(position.X);
		const LONG top = static_cast<LONG>(position.Y);
		const LONG right = left + static_cast<LONG>(size.Width);
		const LONG bottom = top + static_cast<LONG>(size.Height);

		return right > workArea.left
			&& bottom > workArea.top
			&& left < workArea.right
			&& top < workArea.bottom;
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
	{
		const DWORD cwdLength = GetCurrentDirectoryW(0, nullptr);
		if (cwdLength > 0)
		{
			std::wstring cwd(cwdLength, L'\0');
			GetCurrentDirectoryW(cwdLength, cwd.data());
			if (!cwd.empty() && cwd.back() == L'\0')
				cwd.pop_back();
			std::wcout << L"[BOOT] cwd=" << cwd << std::endl;
		}
	}
	// Initialize COM as STA matching the Steinberg editorhost pattern.
	// CoInitializeEx (not OleInitialize) is used here so that VSTGUI's own
	// OleInitialize call inside Win32Frame::Win32Frame() receives S_OK (first
	// OLE init on this thread) rather than S_FALSE.  Getting S_OK means VSTGUI
	// owns the OLE reference it acquired and will call OleUninitialize on
	// teardown correctly.  Using OleInitialize here steals that reference and
	// causes S_FALSE, which subtly breaks OLE drag-and-drop setup inside
	// attached().  RegisterDragDrop and DirectComposition still work with
	// CoInitializeEx because the thread is still an STA.
	const HRESULT uiComInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
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
		if (HandleSlashCommand(msg, s))
			return;
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

	SceneParams sceneParams(DrawableParams{ "" },
		MoveableParams{ {0, 0}, {0, 0, 0}, 1.0 },
		SizeableParams{ 1400, 1000 });
	JamFile jam;
	RigFile rig;

	if (defaults.has_value())
	{
		sceneParams.Position = defaults.value().WinPos;
		sceneParams.Size = defaults.value().WinSize;

		if (!IsWindowPlacementVisible(sceneParams.Position, sceneParams.Size))
			sceneParams.Position = Window::Center(sceneParams.Size);

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
	auto scene = Scene::FromFile(sceneParams, jam, rig, jamDirectory);
	if (!scene.has_value())
	{
		std::cout << "Failed to create Scene... quitting" << std::endl;
		return -1;
	}

	// Wire the scene pointer so the TUI submit handler can forward chat.
	sceneRaw.store(scene.value().get(), std::memory_order_release);

	if (defaults.has_value())
		scene.value()->SetLogging(defaults.value().Logging);

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

		// Destroy any VST plugins that failed to load on the job thread.
		// They were PreInit'd on this thread, so cleanup must run here too.
		vst::DrainUiThreadDestroyQueue();
	}

	// Close VST editor windows while the main thread's COM STA is still valid.
	// IPlugView::removed() may use COM; calling it after CoUninitialize() risks
	// undefined behaviour. scene is still alive here since it goes out of scope
	// after the explicit CoUninitialize() call below.
	scene.value()->Shutdown();
	scene.value()->CloseAllVstEditorWindows();

	window.Release();

	// Final drain for any plugins queued after the last render iteration.
	vst::DrainUiThreadDestroyQueue();

	// Stop the TUI before returning so console mode and cout/cerr rdbufs
	// are fully restored before the CRT shuts down.
	sceneRaw.store(nullptr, std::memory_order_release);
	tui->Stop();
	FreeConsole();

	// Tear down Scene (joins job thread) before COM teardown, then drain once
	// more for any failed-load plugins queued late during job-thread shutdown.
	scene.reset();
	vst::DrainUiThreadDestroyQueue();

	if (uiComInitialized)
		CoUninitialize();

	return (int)msg.wParam;
}
