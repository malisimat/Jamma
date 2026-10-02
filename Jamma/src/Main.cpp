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
#include "../io/StartupConfig.h"
#include "../io/ConsoleTui.h"
#include "../vst/Vst3Plugin.h"
#include <objbase.h>
#include <dbt.h>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <iostream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#include <fstream>
#include <functional>
#include <filesystem>
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
static void PrintNinjamHelp()
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
static bool HandleSlashCommand(const std::string& msg, Scene* scene)
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
		{
			ninjam::NinjamTempoJoinOptions options;
			options.PushLocalTempoOnJoin = true;
			options.PromptBeforeApplyingRemoteTempo = true;
			scene->ConnectNinjam(servers[idx - 1].Host, options);
		}
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
using namespace io;

#define MAX_JSON_CHARS 1000000u

static std::optional<std::wstring> ReadEnvironmentVariable(const wchar_t* name)
{
	const auto required = GetEnvironmentVariableW(name, nullptr, 0);
	if (required == 0)
		return std::nullopt;

	std::wstring value(required - 1, L'\0');
	GetEnvironmentVariableW(name, value.data(), required);
	return value;
}

static std::wstring DefaultIniPath()
{
	return GetPath(PATH_ROAMING) + L"\\Jamma\\defaults.json";
}

static std::wstring ResolveIniPath()
{
	if (auto initPath = ReadEnvironmentVariable(L"JAMMA_DEFAULTS_PATH"); initPath.has_value() && !initPath->empty())
	{
		std::error_code error;
		const auto absolute = std::filesystem::absolute(*initPath, error);
		return error ? *initPath : absolute.wstring();
	}

	return DefaultIniPath();
}

static bool IsWindowPlacementVisible(const utils::Position2d& position, const utils::Size2d& size)
{
	RECT rect{
		static_cast<LONG>(position.X),
		static_cast<LONG>(position.Y),
		static_cast<LONG>(position.X + static_cast<int>(size.Width)),
		static_cast<LONG>(position.Y + static_cast<int>(size.Height))
	};

	RECT workArea{};
	if (!Window::GetMonitorWorkAreaForRect(rect, workArea))
		return true;

	return rect.right > workArea.left
		&& rect.bottom > workArea.top
		&& rect.left < workArea.right
		&& rect.top < workArea.bottom;
}

static bool UpdateIni(const std::wstring& finalPath, const std::string& data)
{
	std::error_code directoryError;
	std::filesystem::create_directories(std::filesystem::path(finalPath).parent_path(), directoryError);
	if (directoryError) return false;
	const std::wstring tempPath = finalPath + L".tmp";
	const HANDLE file = CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return false;
	DWORD written = 0;
	const bool flushed = WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr)
		&& written == data.size() && FlushFileBuffers(file);
	CloseHandle(file);
	if (!flushed)
	{
		DeleteFileW(tempPath.c_str());
		return false;
	}

	if (!MoveFileExW(tempPath.c_str(), finalPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
	{
		DeleteFileW(tempPath.c_str());
		return false;
	}

	return true;
}

static bool SaveRigAtomic(const std::wstring& finalPath, const io::RigFile& rig)
{
	std::stringstream json;
	if (!io::RigFile::ToJsonStream(rig, json))
		return false;
	return UpdateIni(finalPath, json.str());
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
	io::TextReadWriter txtFile;
	auto res = txtFile.Read(initPath, MAX_JSON_CHARS);
	if (!res) return std::nullopt;
	auto [ini, numChars, unused] = std::move(*res);
	std::stringstream ss(std::move(ini));
	auto parsed = InitFile::FromStream(std::move(ss));
	if (!parsed || parsed->Jam.empty() || parsed->Rig.empty()) return std::nullopt;
	return parsed;
}

std::optional<io::JamFile> LoadJam(io::InitFile& ini, bool& readable)
{
	io::TextReadWriter txtFile;

	std::wcout << L"[BOOT] Loading JAM from defaults path: " << ini.Jam << std::endl;
	auto res = txtFile.Read(ini.Jam, MAX_JSON_CHARS);
	readable = res.has_value();
	if (!res.has_value())
	{
		return std::nullopt;
	}
	auto [jamJson, numChars, unused] = std::move(res.value());

	std::stringstream ss(jamJson);
	auto parsed = JamFile::FromStream(std::move(ss));
	if (parsed.has_value() && !io::StartupConfig::ValidateJam(*parsed))
		parsed.reset();
	if (!parsed.has_value())
		std::wcerr << L"[BOOT] JAM is unreadable; starting with an empty session: " << ini.Jam << std::endl;
	else
		std::cout << "[BOOT] Parsed JAM '" << parsed->Name << "' from " << EncodeUtf8(ini.Jam)
			<< " with " << parsed->Stations.size() << " station descriptor(s)." << std::endl;
	return parsed;
}

std::optional<io::JamFile> LoadJamFile(const std::wstring& path)
{
	io::TextReadWriter reader;
	auto contents = reader.Read(path, MAX_JSON_CHARS);
	if (!contents.has_value())
	{
		std::wcerr << L"[LOAD] Could not read JAM: " << path << std::endl;
		return std::nullopt;
	}

	auto [json, numChars, unused] = std::move(contents.value());
	std::stringstream stream(std::move(json));
	auto parsed = JamFile::FromStream(std::move(stream));
	if (!parsed.has_value())
		std::wcerr << L"[LOAD] JAM is unreadable: " << path << std::endl;
	else
		std::cout << "[LOAD] Parsed JAM '" << parsed->Name << "' from " << EncodeUtf8(path)
			<< " with " << parsed->Stations.size() << " station descriptor(s)." << std::endl;
	return parsed;
}

io::JamFile EmptyJam()
{
	std::stringstream stream(JamFile::DefaultJson);
	auto parsed = JamFile::FromStream(std::move(stream));
	if (!parsed.has_value())
		throw std::runtime_error("Built-in empty JAM manifest is invalid");

	auto jam = std::move(parsed.value());
	jam.Name = "empty";
	jam.Ninjam.reset();
	jam.AbsoluteSamplePos = 0u;
	jam.GlobalPhaseOffsetSamps = 0;
	jam.TransportOffsetLoopFrac = 0.0;
	for (auto& station : jam.Stations)
	{
		station.LoopTakes.clear();
		station.VstChain.clear();
		station.MidiRoutes.clear();
		station.StationPhaseOffsetSamps = 0;
	}
	return jam;
}

// Startup diagnostics contain only configuration paths, device names, and outcomes.
// Each launch rotates a bounded history before defaults or hardware are read.
class StartupLog
{
public:
	explicit StartupLog(const std::wstring& defaultsPath)
	{
		const auto directory = std::filesystem::path(defaultsPath).parent_path();
		std::error_code error;
		std::filesystem::create_directories(directory, error);
		if (error) return;
		_path = (directory / L"startup.log").wstring();
		const auto oldest = _path + L".3";
		DeleteFileW(oldest.c_str());
		for (int index = 2; index >= 1; --index)
		{
			const auto from = _path + L"." + std::to_wstring(index);
			const auto to = _path + L"." + std::to_wstring(index + 1);
			MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING);
		}
		MoveFileExW(_path.c_str(), (_path + L".1").c_str(), MOVEFILE_REPLACE_EXISTING);
		_file = CreateFileW(_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL, nullptr);
	}

	~StartupLog() { if (_file != INVALID_HANDLE_VALUE) CloseHandle(_file); }
	StartupLog(const StartupLog&) = delete;
	StartupLog& operator=(const StartupLog&) = delete;

	void Write(const std::string& line)
	{
		std::cout << line << std::endl;
		if (_file == INVALID_HANDLE_VALUE || _bytes >= MaxBytes) return;
		const auto text = line + "\r\n";
		if (_bytes + text.size() > MaxBytes) return;
		DWORD written = 0;
		if (WriteFile(_file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr))
			_bytes += written;
		FlushFileBuffers(_file);
	}

	const std::wstring& Path() const noexcept { return _path; }
	bool Available() const noexcept { return _file != INVALID_HANDLE_VALUE; }

private:
	static constexpr size_t MaxBytes = 1024u * 1024u;
	std::wstring _path;
	HANDLE _file = INVALID_HANDLE_VALUE;
	size_t _bytes = 0u;
};

static const char* MidiStatusText(midi::MidiConnectionStatus status) noexcept
{
	switch (status)
	{
	case midi::MidiConnectionStatus::Disabled: return "disabled";
	case midi::MidiConnectionStatus::Missing: return "missing";
	case midi::MidiConnectionStatus::Ambiguous: return "ambiguous";
	case midi::MidiConnectionStatus::Failed: return "failed";
	case midi::MidiConnectionStatus::Connected: return "connected";
	}
	return "unknown";
}

static io::JamFile RecoveryJam(const io::RigFile& rig, bool selectedRig)
{
	auto jam = EmptyJam();
	jam.Name = "First-run jam";
	if (!selectedRig) return jam;
	const auto stationTemplate = jam.Stations.front();
	jam.Stations.clear();
	for (const auto& trigger : rig.Triggers)
	{
		if (!trigger.StationTarget || trigger.StationTarget->empty()) continue;
		const auto duplicate = std::any_of(jam.Stations.begin(), jam.Stations.end(),
			[&](const auto& station) { return station.Name == *trigger.StationTarget; });
		if (!duplicate)
		{
			if (jam.Stations.size() >= io::JamFile::MaxStations) break;
			auto station = stationTemplate;
			station.Name = *trigger.StationTarget;
			jam.Stations.push_back(std::move(station));
		}
	}
	if (jam.Stations.empty()) jam.Stations.push_back(stationTemplate);
	return jam;
}

std::optional<io::RigFile> LoadRig(io::InitFile& ini, bool& readable)
{
	io::TextReadWriter txtFile;

	auto res = txtFile.Read(ini.Rig, MAX_JSON_CHARS);
	readable = res.has_value();
	if (!res.has_value())
		return std::nullopt;
	auto [rigJson, numChars, unused] = std::move(res.value());

	std::stringstream ss(rigJson);
	auto parsed = RigFile::FromStream(std::move(ss));
	if (parsed.has_value() && !io::StartupConfig::ValidateRig(*parsed))
		parsed.reset();
	return parsed;
}

static std::optional<std::wstring> NewGeneratedPath(const std::wstring& directory,
	const wchar_t* stem, const wchar_t* extension)
{
	std::error_code error;
	std::filesystem::create_directories(directory, error);
	if (error) return std::nullopt;
	for (unsigned int suffix = 0; suffix < 1000u; ++suffix)
	{
		const auto name = std::wstring(stem) + (suffix ? L"-" + std::to_wstring(suffix) : L"") + extension;
		const auto candidate = (std::filesystem::path(directory) / name).wstring();
		if (!std::filesystem::exists(candidate, error) && !error)
			return candidate;
		if (error) return std::nullopt;
	}
	return std::nullopt;
}

// Generated files are never allowed to replace a pre-existing target.
static bool PublishNewFile(const std::wstring& finalPath, const std::string& data)
{
	std::error_code directoryError;
	std::filesystem::create_directories(std::filesystem::path(finalPath).parent_path(), directoryError);
	if (directoryError) return false;
	for (unsigned int suffix = 0; suffix < 100u; ++suffix)
	{
		const auto temp = finalPath + L".tmp-" + std::to_wstring(GetCurrentProcessId())
			+ L"-" + std::to_wstring(suffix);
		const HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
			FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE)
		{
			if (GetLastError() == ERROR_FILE_EXISTS) continue;
			return false;
		}
		DWORD written = 0;
		const bool flushed = WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr)
			&& written == data.size() && FlushFileBuffers(file);
		CloseHandle(file);
		if (!flushed || !MoveFileExW(temp.c_str(), finalPath.c_str(), MOVEFILE_WRITE_THROUGH))
		{
			DeleteFileW(temp.c_str());
			return false;
		}
		return true;
	}
	return false;
}

static bool SaveGeneratedRig(const std::wstring& path, const io::RigFile& rig)
{
	std::stringstream stream;
	if (!io::RigFile::ToJsonStream(rig, stream)) return false;
	const auto parsed = io::RigFile::FromStream(std::stringstream(stream.str()));
	return parsed && io::StartupConfig::ValidateRig(*parsed)
		&& PublishNewFile(path, stream.str());
}

static bool SaveGeneratedJam(const std::wstring& path, const io::JamFile& jam)
{
	std::stringstream stream;
	if (!io::JamFile::ToStream(jam, stream)) return false;
	const auto parsed = io::JamFile::FromStream(std::stringstream(stream.str()));
	return parsed && io::StartupConfig::ValidateJam(*parsed)
		&& PublishNewFile(path, stream.str());
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
	// The pointer and mutex let the submit handler forward chat safely while the
	// UI thread replaces a complete Scene after an explicit JAM load.
	auto tui = std::make_unique<io::ConsoleTui>();
	std::atomic<Scene*> sceneRaw{ nullptr };
	std::mutex sceneRawMutex;
	tui->Start("> ", [&sceneRaw, &sceneRawMutex](const std::string& msg) {
		std::scoped_lock lock(sceneRawMutex);
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
	StartupLog startupLog(initPath);
	SYSTEMTIME startupTime{};
	GetLocalTime(&startupTime);
	std::ostringstream startupHeader;
	startupHeader << "[BOOT] " << startupTime.wYear << '-'
		<< std::setw(2) << std::setfill('0') << startupTime.wMonth << '-'
		<< std::setw(2) << startupTime.wDay << ' '
		<< std::setw(2) << startupTime.wHour << ':'
		<< std::setw(2) << startupTime.wMinute << ':'
		<< std::setw(2) << startupTime.wSecond << " local Jamma v" LIB_VERSION;
	startupLog.Write(startupHeader.str());
	startupLog.Write("[BOOT] defaults=" + EncodeUtf8(initPath) + " support-log=" +
		EncodeUtf8(startupLog.Path()) + (startupLog.Available() ? "" : " (unavailable)"));
	const bool defaultsExisted = GetFileAttributesW(initPath.c_str()) != INVALID_FILE_ATTRIBUTES;
	startupLog.Write(std::string("[BOOT] defaults pre-existing=") + (defaultsExisted ? "yes" : "no"));
	auto defaults = LoadIni(initPath);
	const bool defaultsValid = defaults.has_value();
	if (!defaultsValid)
	{
		if (defaultsExisted)
		{
			startupLog.Write("[BOOT] existing defaults invalid or unreadable; preserving original");
			std::cerr << "[BOOT] Existing defaults.json is invalid or unreadable; preserving it and using in-memory defaults." << std::endl;
		}
		else
			std::cout << "[BOOT] No defaults.json; using in-memory first-run defaults." << std::endl;
		defaults = InitFile::FromStream(std::stringstream(
			InitFile::DefaultJson(EncodeUtf8(utils::GetParentDirectory(initPath)))));
	}
	if (!defaults)
	{
		startupLog.Write("[BOOT] built-in defaults invalid; startup aborted");
		return -1;
	}

	SceneParams sceneParams(DrawableParams{ "" },
		MoveableParams{ {0, 0}, {0, 0, 0}, 1.0 },
		SizeableParams{ 1400, 1000 });
	JamFile jam = EmptyJam();
	RigFile rig = RigFile::FromStream(std::stringstream(RigFile::DefaultJson)).value();
	bool rigGenerated = true;
	bool jamGenerated = true;

	if (defaults.has_value())
	{
		sceneParams.Position = defaults.value().WinPos;
		sceneParams.Size = defaults.value().WinSize;

		if (!IsWindowPlacementVisible(sceneParams.Position, sceneParams.Size))
			sceneParams.Position = Window::Center(sceneParams.Size);

		try
		{
			const bool jamExists = GetFileAttributesW(defaults->Jam.c_str()) != INVALID_FILE_ATTRIBUTES;
			bool jamReadable = false;
			auto jamOpt = LoadJam(defaults.value(), jamReadable);
			const auto jamDecision = io::StartupConfig::Decide(
				io::StartupConfig::Classify(jamExists, jamReadable, jamOpt.has_value()), defaults->JamOrigin);
			jamGenerated = jamDecision.Generate;
			std::cout << "[JAM] " << (jamDecision.UseExisting ? "loaded existing" : jamDecision.Recovery ? "recovery needed" : "generation needed")
				<< " path=" << EncodeUtf8(defaults->Jam) << std::endl;
			if (jamOpt.has_value())
				jam = std::move(jamOpt.value());
			else
				std::cout << "[BOOT] Continuing with an empty JAM. Use Load JAM to choose a compatible session." << std::endl;
		}
		catch (const std::exception& error)
		{
			std::cerr << "[BOOT] JAM restore failed; continuing with an empty session: " << error.what() << std::endl;
		}
		catch (...)
		{
			std::cerr << "[BOOT] JAM restore failed with an unknown error; continuing with an empty session." << std::endl;
		}

		const bool rigExists = GetFileAttributesW(defaults->Rig.c_str()) != INVALID_FILE_ATTRIBUTES;
		bool rigReadable = false;
		auto rigOpt = LoadRig(defaults.value(), rigReadable);
		if (rigOpt && !jamGenerated && !io::StartupConfig::ValidateRigForJam(*rigOpt, jam))
		{
			startupLog.Write("[RIG] selected rig has unresolved Station target(s); preserving original and generating recovery rig path=" +
				EncodeUtf8(defaults->Rig));
			rigOpt.reset();
		}
		const auto rigDecision = io::StartupConfig::Decide(
			io::StartupConfig::Classify(rigExists, rigReadable, rigOpt.has_value()), defaults->RigOrigin);
		rigGenerated = rigDecision.Generate;
		std::cout << "[RIG] " << (rigDecision.UseExisting ? "loaded existing" : rigDecision.Recovery ? "recovery needed" : "generation needed")
			<< " path=" << EncodeUtf8(defaults->Rig) << std::endl;
		if (rigOpt.has_value())
			rig = rigOpt.value();
		startupLog.Write("[RIG] " + std::string(rigDecision.UseExisting ? "selected" :
			rigDecision.Recovery ? "recovery" : "generation") + " path=" + EncodeUtf8(defaults->Rig));
		startupLog.Write("[JAM] " + std::string(jamGenerated ? "generation/recovery" : "selected") +
			" path=" + EncodeUtf8(defaults->Jam) + " stations=" + std::to_string(jam.Stations.size()));
	}

	const auto asioInventory = audio::AudioDevice::DiscoverAsio();
	startupLog.Write("[ASIO] inventory=" + std::to_string(asioInventory.Devices.size()) +
		" default-input=" + (asioInventory.DefaultInputId ? std::to_string(*asioInventory.DefaultInputId) : "none") +
		" default-output=" + (asioInventory.DefaultOutputId ? std::to_string(*asioInventory.DefaultOutputId) : "none"));
	std::cout << "[ASIO] Discovered " << asioInventory.Devices.size() << " device(s); default input="
		<< (asioInventory.DefaultInputId ? std::to_string(*asioInventory.DefaultInputId) : "none")
		<< " output=" << (asioInventory.DefaultOutputId ? std::to_string(*asioInventory.DefaultOutputId) : "none") << std::endl;
	if (!asioInventory.EnumerationError.empty())
		startupLog.Write("[ASIO] enumeration error=" + asioInventory.EnumerationError);
	for (const auto& device : asioInventory.Devices)
	{
		std::ostringstream detail;
		detail << "[ASIO] device=" << device.Id << " name='" << device.Name << "' in=" << device.InputChannels
			<< " out=" << device.OutputChannels << " probed=" << device.Probed
			<< " default-in=" << device.DefaultInput << " default-out=" << device.DefaultOutput
			<< " preferred-rate=" << device.PreferredSampleRate << " rates=";
		for (const auto rate : device.SampleRates) detail << rate << ',';
		if (!device.ProbeError.empty()) detail << " error=" << device.ProbeError;
		startupLog.Write(detail.str());
		std::cout << "[ASIO] " << device.Id << " '" << device.Name << "' in=" << device.InputChannels
			<< " out=" << device.OutputChannels << " probed=" << device.Probed
			<< " default-in=" << device.DefaultInput << " default-out=" << device.DefaultOutput
			<< " preferred-rate=" << device.PreferredSampleRate << " rates=";
		for (const auto rate : device.SampleRates) std::cout << rate << " ";
		std::cout << device.ProbeError << std::endl;
	}
	const auto midiInventory = midi::MidiDevice::InventoryInputDevices();
	if (!rigGenerated)
	{
		for (auto& device : rig.User.Midi.Devices)
			device.Name = midi::MidiDevice::ResolveSavedInputName(device.Name, midiInventory.Devices);
		for (auto& trigger : rig.Triggers)
		{
			for (auto& name : trigger.MidiInputDevices)
				name = midi::MidiDevice::ResolveSavedInputName(name, midiInventory.Devices);
			if (trigger.MidiTrigger)
				trigger.MidiTrigger->Device = midi::MidiDevice::ResolveSavedInputName(
					trigger.MidiTrigger->Device, midiInventory.Devices);
		}
	}
	startupLog.Write("[MIDI] inventory=" + std::to_string(midiInventory.Devices.size()) +
		(midiInventory.Error.empty() ? "" : " error=" + midiInventory.Error));
	std::cout << "[MIDI] Discovered " << midiInventory.Devices.size() << " input(s) " << midiInventory.Error << std::endl;
	for (const auto& device : midiInventory.Devices)
	{
		startupLog.Write("[MIDI] port=" + std::to_string(device.DeviceId) + " name='" + device.Name + "'");
		std::cout << "[MIDI] " << device.DeviceId << " '" << device.Name << "'" << std::endl;
	}

	if (rigGenerated)
	{
		std::vector<std::string> midiNames;
		for (const auto& device : midiInventory.Devices)
			midiNames.push_back(device.Name);
		const audio::AsioDeviceInfo* preferred = nullptr;
		const auto ordered = audio::AudioDevice::OrderedCandidateIds(asioInventory, rig.User.Audio, true);
		for (const auto id : ordered)
			for (const auto& device : asioInventory.Devices)
				if (!preferred && device.Id == id && device.Probed && device.OutputChannels > 0u)
					preferred = &device;
		if (preferred)
		{
			rig.User.Audio.Name = preferred->Name;
			rig.User.Audio.NumChannelsIn = preferred->InputChannels;
			rig.User.Audio.NumChannelsOut = preferred->OutputChannels;
		}
		rig = io::StartupConfig::GeneratedRig(rig, preferred ? preferred->InputChannels : 0u,
			midiNames, jamGenerated ? "Station1" : jam.Stations.front().Name);
	}

	std::function<bool(const io::RigFile&)> saveRig;
	const auto generatedDirectory = utils::GetParentDirectory(initPath);
	const auto generatedRigPath = rigGenerated ? NewGeneratedPath(generatedDirectory, L"default", L".rig") : std::nullopt;
	const auto generatedRigPublished = std::make_shared<std::atomic<bool>>(false);
	if (defaults.has_value() && !rigGenerated)
	{
		const auto rigPath = defaults->Rig;
		saveRig = [rigPath](const io::RigFile& candidate) { return SaveRigAtomic(rigPath, candidate); };
	}
	else if (generatedRigPath)
	{
		const auto path = *generatedRigPath;
		saveRig = [path, generatedRigPublished](const io::RigFile& candidate) {
			const bool saved = generatedRigPublished->load(std::memory_order_acquire) ? SaveRigAtomic(path, candidate) : SaveGeneratedRig(path, candidate);
			if (saved) generatedRigPublished->store(true, std::memory_order_release);
			return saved;
		};
	}

	if (jamGenerated)
		jam = RecoveryJam(rig, !rigGenerated);
	const auto jamDirectory = defaults.has_value() ?
		utils::GetParentDirectory(defaults->Jam) :
		utils::GetParentDirectory(initPath);
	startupLog.Write("[BOOT] generated-directory=" + EncodeUtf8(generatedDirectory));
	const auto createScene = [&](JamFile source)
		-> std::optional<std::shared_ptr<Scene>>
	{
		try
		{
			return Scene::FromFile(sceneParams, std::move(source), rig, jamDirectory, saveRig);
		}
		catch (const std::exception& error)
		{
			std::cerr << "[BOOT] Scene creation failed: " << error.what() << std::endl;
		}
		catch (...)
		{
			std::cerr << "[BOOT] Scene creation failed with an unknown error." << std::endl;
		}
		return std::nullopt;
	};

	const JamFile jamForPublication = jam;
	auto scene = createScene(std::move(jam));
	bool jamSceneFallback = false;

	if (!scene.has_value())
	{
		std::cout << "[BOOT] Could not restore JAM; starting with an empty session." << std::endl;
		jamGenerated = true;
		jamSceneFallback = true;
		scene = createScene(RecoveryJam(rig, !rigGenerated));
	}
	if (!scene.has_value())
	{
		std::cout << "Failed to create empty Scene... quitting" << std::endl;
		return -1;
	}
	if (scene.value()->SnapshotStations().empty())
	{
		std::cerr << "[BOOT] Scene has no constructed Station." << std::endl;
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

	scene.value()->InitGlobalKeyCapture();

	scene.value()->SetMidiInputInventory(midiInventory);
	{
		const auto& requested = rig.User.Audio;
		std::ostringstream detail;
		detail << "[ASIO] requested name='" << requested.Name << "' in=" << requested.NumChannelsIn
			<< " out=" << requested.NumChannelsOut << " rate=" << requested.SampleRate
			<< " buffer=" << requested.BufSize << " buffers=" << requested.NumBuffers;
		startupLog.Write(detail.str());
	}
	scene.value()->InitAudio(rigGenerated, &asioInventory);
	const auto& asioReport = scene.value()->GetAsioOpenReport();
	for (const auto& attempt : asioReport.Attempts)
	{
		std::ostringstream detail;
		detail << "[ASIO] attempt device='" << attempt.Name << "' in=" << attempt.InputChannels
			<< " out=" << attempt.OutputChannels << " rate=" << attempt.SampleRate
			<< " buffer=" << attempt.BufferSize << " buffers=" << attempt.NumBuffers
			<< " opened=" << attempt.Opened << " started=" << attempt.Started;
		if (!attempt.Error.empty()) detail << " error=" << attempt.Error;
		startupLog.Write(detail.str());
		std::cout << "[ASIO] Attempt '" << attempt.Name << "' in=" << attempt.InputChannels
			<< " out=" << attempt.OutputChannels << " rate=" << attempt.SampleRate
			<< " buffer=" << attempt.BufferSize << " opened=" << attempt.Opened
			<< " started=" << attempt.Started << " " << attempt.Error << std::endl;
	}
	for (const auto& attempt : scene.value()->GetMidiConnectionResult().Attempts)
	{
		startupLog.Write("[MIDI] request='" + attempt.RequestedName + "' connected='" +
			attempt.ConnectedName + "' status=" + MidiStatusText(attempt.Status) +
			(attempt.Error.empty() ? "" : " error=" + attempt.Error));
		std::cout << "[MIDI] Request '" << attempt.RequestedName << "' connected='"
			<< attempt.ConnectedName << "' status=" << MidiStatusText(attempt.Status)
			<< " " << attempt.Error << std::endl;
	}
	if (asioReport.StartedParams)
	{
		const auto& actual = *asioReport.StartedParams;
		std::ostringstream detail;
		detail << "[ASIO] started name='" << actual.Name << "' in=" << actual.NumInputChannels
			<< " out=" << actual.NumOutputChannels << " rate=" << actual.SampleRate
			<< " buffer=" << actual.BufSize << " buffers=" << actual.NumBuffers
			<< " latency-in=" << actual.InputLatency << " latency-out=" << actual.OutputLatency;
		startupLog.Write(detail.str());
		if (actual.NumInputChannels == 0u)
			startupLog.Write("[ASIO] warning: no input channels opened; audio recording is unavailable; Trigger input is []");
	}
	else startupLog.Write("[ASIO] no stream started; audio recording and MIDI input are unavailable; empty scene remains usable");
	if (!rigGenerated && asioReport.StartedParams)
	{
		const auto& actual = *asioReport.StartedParams;
		const auto& requested = rig.User.Audio;
		if (requested.Name != actual.Name || requested.NumChannelsIn != actual.NumInputChannels ||
			requested.NumChannelsOut != actual.NumOutputChannels || requested.SampleRate != actual.SampleRate ||
			requested.BufSize != actual.BufSize || requested.NumBuffers != actual.NumBuffers)
			startupLog.Write("[RIG] warning: selected audio request differs from started stream; selected file retained");
	}
	if (!rigGenerated)
	{
		for (const auto& attempt : scene.value()->GetMidiConnectionResult().Attempts)
			if (attempt.Status != midi::MidiConnectionStatus::Connected &&
				attempt.Status != midi::MidiConnectionStatus::Disabled)
				startupLog.Write("[RIG] warning: selected MIDI request '" + attempt.RequestedName +
					"' status=" + MidiStatusText(attempt.Status) + "; selected file retained");
	}

	if (rigGenerated && asioReport.StartedParams && generatedRigPath)
	{
		const auto& actual = *asioReport.StartedParams;
		std::vector<std::string> connectedMidi;
		for (const auto& device : scene.value()->GetMidiConnectionResult().Connected)
			connectedMidi.push_back(device.Name);
		auto finalRig = io::StartupConfig::GeneratedRig(rig, actual.NumInputChannels, connectedMidi,
			jamGenerated ? "Station1" : jamForPublication.Stations.front().Name);
		finalRig.User.Audio.Name = actual.Name;
		finalRig.User.Audio.SampleRate = actual.SampleRate;
		finalRig.User.Audio.BufSize = actual.BufSize;
		finalRig.User.Audio.NumBuffers = actual.NumBuffers;
		finalRig.User.Audio.LatencyIn = actual.InputLatency;
		finalRig.User.Audio.LatencyOut = actual.OutputLatency;
		finalRig.User.Audio.NumChannelsIn = actual.NumInputChannels;
		finalRig.User.Audio.NumChannelsOut = actual.NumOutputChannels;

		std::stringstream initialStream, finalStream;
		RigFile::ToJsonStream(rig, initialStream);
		RigFile::ToJsonStream(finalRig, finalStream);
		if (initialStream.str() == finalStream.str())
		{
			if (saveRig) saveRig(finalRig);
		}
		else
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
			bool submitted = false;
			while (std::chrono::steady_clock::now() < deadline)
			{
				const auto accepted = scene.value()->AcceptedRigSnapshot();
				if (submitted && accepted)
				{
					std::stringstream acceptedStream;
					RigFile::ToJsonStream(accepted->Rig, acceptedStream);
					if (acceptedStream.str() == finalStream.str()) break;
				}
				if (!submitted)
				{
					const auto result = scene.value()->RequestRigEdit(finalRig);
					if (result == engine::RigCoordinator::EditResult::Pending) submitted = true;
					else if (result != engine::RigCoordinator::EditResult::AudioCallbackInactive &&
						result != engine::RigCoordinator::EditResult::EditsDisabled) break;
				}
				Sleep(10);
			}
		}
		rig = std::move(finalRig);
		const auto accepted = scene.value()->AcceptedRigSnapshot();
		std::stringstream acceptedStream;
		if (accepted) RigFile::ToJsonStream(accepted->Rig, acceptedStream);
		if (generatedRigPublished->load(std::memory_order_acquire) && acceptedStream.str() == finalStream.str())
		{
			defaults->Rig = *generatedRigPath;
			defaults->RigOrigin = std::string(io::StartupConfig::GeneratedOrigin);
			std::cout << "[RIG] Published " << EncodeUtf8(*generatedRigPath)
				<< " inputs=" << actual.NumInputChannels << " outputs=" << actual.NumOutputChannels
				<< " rate=" << actual.SampleRate << "; Trigger input indices are zero-based." << std::endl;
			startupLog.Write("[RIG] published=" + EncodeUtf8(*generatedRigPath) +
				" target=" + *rig.Triggers.front().StationTarget + " input indices are zero-based");
		}
		else
		{
			std::cerr << "[RIG] Final routing was not accepted and published; retaining in-memory startup rig." << std::endl;
			startupLog.Write("[RIG] final routing not accepted or published");
		}
	}
	else if (rigGenerated)
	{
		std::cerr << "[RIG] Audio did not start; generated rig will not be published." << std::endl;
		startupLog.Write("[RIG] no generated rig published because audio did not start");
	}

	bool generatedJamPublished = false;
	if (jamGenerated)
	{
		const auto jamPath = NewGeneratedPath(generatedDirectory, L"default", L".jam");
		const auto candidate = jamSceneFallback ? RecoveryJam(rig, !rigGenerated) : jamForPublication;
		if (jamPath && SaveGeneratedJam(*jamPath, candidate))
		{
			generatedJamPublished = true;
			defaults->Jam = *jamPath;
			defaults->JamOrigin = std::string(io::StartupConfig::GeneratedOrigin);
			std::cout << "[JAM] Published " << EncodeUtf8(*jamPath) << std::endl;
			startupLog.Write("[JAM] published=" + EncodeUtf8(*jamPath) +
				" stations=" + std::to_string(candidate.Stations.size()));
		}
		else
		{
			std::cerr << "[JAM] Could not publish generated session." << std::endl;
			startupLog.Write("[JAM] generated session publication failed");
		}
	}
	const bool rigReadyForDefaults = !rigGenerated ||
		(generatedRigPath && defaults->Rig == *generatedRigPath && generatedRigPublished->load(std::memory_order_acquire));
	const bool jamReadyForDefaults = !jamGenerated || generatedJamPublished;
	if (!rigReadyForDefaults)
		startupLog.Write("[BOOT] defaults publication deferred: no validated rig file");
	if (!jamReadyForDefaults)
		startupLog.Write("[BOOT] defaults publication deferred: no validated jam file");
	std::wstring publishedDefaultsPath = initPath;
	if (rigReadyForDefaults && jamReadyForDefaults && defaultsValid && (defaults->RigOrigin == io::StartupConfig::GeneratedOrigin ||
		defaults->JamOrigin == io::StartupConfig::GeneratedOrigin))
	{
		std::stringstream stream;
		if (InitFile::ToStream(*defaults, stream) && UpdateIni(initPath, stream.str()))
			startupLog.Write("[BOOT] defaults published=" + EncodeUtf8(initPath));
		else startupLog.Write("[BOOT] defaults publication failed path=" + EncodeUtf8(initPath));
	}
	else if (rigReadyForDefaults && jamReadyForDefaults && !defaultsExisted)
	{
		std::stringstream stream;
		if (InitFile::ToStream(*defaults, stream) && PublishNewFile(initPath, stream.str()))
			startupLog.Write("[BOOT] defaults published=" + EncodeUtf8(initPath));
		else startupLog.Write("[BOOT] defaults publication failed path=" + EncodeUtf8(initPath));
	}
	else if (rigReadyForDefaults && jamReadyForDefaults && !defaultsValid && defaultsExisted)
	{
		const auto recoveryPath = NewGeneratedPath(generatedDirectory, L"defaults-recovered", L".json");
		std::stringstream stream;
		if (recoveryPath && InitFile::ToStream(*defaults, stream) && PublishNewFile(*recoveryPath, stream.str()))
		{
			publishedDefaultsPath = *recoveryPath;
			startupLog.Write("[BOOT] recovery defaults published=" + EncodeUtf8(*recoveryPath) +
				"; set JAMMA_DEFAULTS_PATH to use it; original retained=" + EncodeUtf8(initPath));
		}
		else startupLog.Write("[BOOT] recovery defaults publication failed; original retained=" + EncodeUtf8(initPath));
	}
	for (const auto& trigger : rig.Triggers)
	{
		std::ostringstream route;
		route << "[RIG] trigger='" << trigger.Name << "' id='" << trigger.Id
			<< "' station='" << (trigger.StationTarget ? *trigger.StationTarget : "none") << "' adc=";
		for (const auto channel : trigger.InputChannels)
			route << channel << "(physical " << channel + 1u << "),";
		route << " loop-midi=";
		for (const auto& name : trigger.MidiInputDevices) route << "'" << name << "',";
		startupLog.Write(route.str());
	}
	std::ostringstream startupSummary;
	startupSummary << "[BOOT] startup summary audio=" << (asioReport.StartedParams ? "started" : "unavailable");
	if (asioReport.StartedParams)
	{
		const auto& actual = *asioReport.StartedParams;
		startupSummary << " asio='" << actual.Name << "' rate=" << actual.SampleRate
			<< " in=" << actual.NumInputChannels << " out=" << actual.NumOutputChannels;
	}
	startupSummary << " midi=";
	for (const auto& device : scene.value()->GetMidiConnectionResult().Connected)
		startupSummary << "'" << device.Name << "' ";
	startupSummary << " stations=" << scene.value()->SnapshotStations().size()
		<< " triggers=" << rig.Triggers.size()
		<< " rig=" << EncodeUtf8(defaults->Rig)
		<< " jam=" << EncodeUtf8(defaults->Jam)
		<< " defaults=" << EncodeUtf8(publishedDefaultsPath)
		<< " log=" << EncodeUtf8(startupLog.Path());
	startupLog.Write(startupSummary.str());

	MSG msg;
	bool active = true;
	while (active)
	{
		// Dispatch one message before checking the budget so queued work always progresses.
		constexpr auto messagePumpBudget = std::chrono::milliseconds(2);
		const auto messagePumpStart = std::chrono::steady_clock::now();
		while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
		{
			if (msg.message == WM_DEVICECHANGE && msg.wParam == DBT_DEVNODES_CHANGED)
				scene.value()->RequestMidiRefresh();
			if (msg.message == WM_QUIT)
			{
				scene.value()->Shutdown();
				vst::DrainUiThreadDestroyQueue();
				active = false;
				break;
			}

			TranslateMessage(&msg);
			DispatchMessage(&msg);
			if (std::chrono::steady_clock::now() - messagePumpStart >= messagePumpBudget)
				break;
		}

		if (!active)
			break;

		actions::KeyAction globalKeyAction;
		if (scene.value()->PumpGlobalKeyCapture(globalKeyAction))
			window.OnAction(globalKeyAction);

		if (window.ConsumeJamLoadRequest())
		{
			const bool paused = scene.value()->PauseAudio();
			if (!paused)
				scene.value()->CloseAudio();

			const auto jamPath = utils::PickJamFile();
			if (jamPath.empty())
			{
				if (paused)
					scene.value()->ResumeAudio();
				else
					scene.value()->InitAudio();
			}
			else
			{
				std::optional<std::shared_ptr<Scene>> replacement;
				try
				{
					auto selectedJam = LoadJamFile(jamPath);
					if (selectedJam.has_value())
					{
						const auto activeRig = scene.value()->AcceptedRigSnapshot();
						replacement = Scene::FromFile(sceneParams, std::move(selectedJam.value()),
							activeRig ? activeRig->Rig : rig,
							utils::GetParentDirectory(jamPath), saveRig);
					}
				}
				catch (const std::exception& error)
				{
					std::cerr << "Load JAM failed: " << error.what() << std::endl;
				}
				catch (...)
				{
					std::cerr << "Load JAM failed with an unknown error" << std::endl;
				}

				if (!replacement.has_value())
				{
					std::wcerr << L"Load JAM failed; keeping the current session: " << jamPath << std::endl;
					if (paused)
						scene.value()->ResumeAudio();
					else
						scene.value()->InitAudio();
				}
				else
				{
					if (defaults.has_value())
						replacement.value()->SetLogging(defaults.value().Logging);

					{
						// Exclude the console submit callback while its raw scene view is rebound.
						std::scoped_lock scenePointerLock(sceneRawMutex);
						sceneRaw.store(nullptr, std::memory_order_release);
						// Editors, queued jobs, and plugin instances belong to the outgoing
						// JAM. Tear them down completely before binding the window to the
						// already-constructed replacement Scene.
						scene.value()->Shutdown();
						window.ReplaceScene(*replacement.value());
						scene = std::move(replacement);
						sceneRaw.store(scene.value().get(), std::memory_order_release);
					}
					vst::DrainUiThreadDestroyQueue();

					scene.value()->InitGlobalKeyCapture();
					scene.value()->InitAudio();
				}
			}
		}

		window.Render();
		window.Swap();

		// Destroy any VST plugins that failed to load on the job thread.
		// They were PreInit'd on this thread, so cleanup must run here too.
		vst::DrainUiThreadDestroyQueue();
	}

	// Shutdown stops the audio callback before it closes VST editor windows and
	// releases their plugins, while this main thread's COM STA is still valid.
	scene.value()->Shutdown();

	if (defaultsValid && rigReadyForDefaults && jamReadyForDefaults)
	{
		auto savedDefaults = defaults.value();
		const auto restoreConfig = window.GetRestoreConfig();
		savedDefaults.WinPos = restoreConfig.Position;
		savedDefaults.WinSize = restoreConfig.Size;

		std::stringstream savedDefaultsStream;
		InitFile::ToStream(savedDefaults, savedDefaultsStream);
		if (!UpdateIni(initPath, savedDefaultsStream.str()))
			std::cerr << "[BOOT] Failed to save defaults atomically: " << EncodeUtf8(initPath) << std::endl;
	}

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
