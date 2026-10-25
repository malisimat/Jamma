///////////////////////////////////////////////////////////
//
// Copyright(c) 2018-2024 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#include "NetworkSession.h"
#include "ConsoleWindow.h"
#include "../../console/GlobalKeyGate.h"
#include "../../console/CommandMailbox.h"
#include "../../console/CommandParsing.h"
#include "../../console/Help.h"
#include "../../console/LineRing.h"
#include "Main.h"
#include "../Resource.h"
#include "RigDefaultSave.h"
#include "Window.h"
#include "PathUtils.h"
#include "../ninjam/NinjamSession.h"
#include "../io/TextReadWriter.h"
#include "../io/InitFile.h"
#include "../io/StartupConfig.h"
#include "ConsoleCapture.h"
#include "CrtCapture.h"
#include "StartupLog.h"
#include "JobFailureNotice.h"
#include "../vst/Vst3Plugin.h"
#include <objbase.h>
#include <dbt.h>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <deque>
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
static void PrintNinjamServers()
{
	auto servers = ninjam::NinjamSession::GetReachablePublicServers();
	std::ostringstream output;
	output << "[NINJAM] Commands:\n"
	          << "[NINJAM]   /ninjam  /n          List/refresh public servers\n"
	          << "[NINJAM]   /  /?  /help         General help and controls (offline)\n"
	          << "[NINJAM]   /c <n>  /connect <n> Connect to server by number\n"
	          << "[NINJAM]   /d /q /quit /exit /disconnect  Disconnect\n"
	          << "[NINJAM] Servers:\n";
	for (std::size_t i = 0; i < servers.size(); ++i)
	{
		output << "[NINJAM]   " << (i + 1) << ". "
		          << servers[i].Host
		          << ninjam::NinjamSession::FormatPublicServerSummary(servers[i])
		          << "\n";
	}
	const auto text = output.str();
	std::cout << text << std::flush;
}

static void WriteConsoleEventLine(std::string line)
{
	std::cout << line << std::endl;
}

// Returns true when the message was a slash command (consumed; should NOT
// be forwarded as chat). Returns false for ordinary chat text.
static bool HandleSlashCommand(const std::string& msg, Scene* scene)
{
	const auto parsed = console::ParseCommand(msg);
	if (parsed.Kind == console::CommandKind::Chat) return false;

	if (console::TryWriteGeneralHelp(parsed, std::cout))
	{
		std::cout << std::flush;
		return true;
	}

	if (parsed.Kind == console::CommandKind::Ninjam)
	{
		const auto snapshot = ninjam::NinjamSession::GetPublicServerDirectorySnapshot();
		const bool refreshStarted = ninjam::NinjamSession::RefreshPublicServerDirectoryAsync(
			PrintNinjamServers,
			[] { console::ConsoleCapture::EnableForCurrentThread(true); },
			[] { WriteConsoleEventLine("[NINJAM] Refreshing live metadata from autosong.ninjam.com..."); });
		if (!refreshStarted && (snapshot.RefreshInFlight || !snapshot.HasLiveData))
		{
			WriteConsoleEventLine("[NINJAM] Refreshing live metadata from autosong.ninjam.com...");
		}
		else if (!refreshStarted)
		{
			PrintNinjamServers();
		}
		return true;
	}

	if (parsed.Kind == console::CommandKind::Connect)
	{
		if (parsed.Arguments.empty())
		{
			WriteConsoleEventLine("[NINJAM] Usage: /c <number> or /connect <number>  (type /ninjam or /n for list)");
			return true;
		}
		const int idx = parsed.ServerNumber;

		auto snapshot = ninjam::NinjamSession::GetPublicServerDirectorySnapshot();
		auto servers = ninjam::NinjamSession::GetReachablePublicServers();
		const auto serverCount = static_cast<int>(servers.size());

		if (serverCount == 0)
		{
			WriteConsoleEventLine("[NINJAM] No reachable servers in the current list  (type /ninjam or /n to refresh)");
			return true;
		}

		if (idx < 1 || idx > serverCount)
		{
			WriteConsoleEventLine("[NINJAM] Server number must be 1-"
				+ std::to_string(serverCount) + "  (type /ninjam or /n for list)");
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
			WriteConsoleEventLine("[NINJAM] Not ready yet");
		return true;
	}

	if (parsed.Kind == console::CommandKind::Disconnect)
	{
		if (scene)
			scene->DisconnectNinjam();
		else
			WriteConsoleEventLine("[NINJAM] Not connected");
		return true;
	}

	WriteConsoleEventLine("[CONSOLE] Unknown command /" + parsed.Verb
		+ "  (type / or /help for controls; /ninjam or /n for servers)");
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
	const auto normalized = io::RigFile::Normalize(rig);
	if (!normalized || !io::RigFile::ToJsonStream(*normalized, json))
		return false;
	// Validate the exact bytes before opening the staging file. A malformed
	// candidate must never replace a curated rig, including invalid UTF-8.
	const auto parsed = io::RigFile::FromStream(std::stringstream(json.str()));
	if (!parsed || *parsed != *normalized) return false;
	return UpdateIni(finalPath, json.str());
}

static bool PublishDefaultJam(const std::wstring& savedPath, const std::wstring& defaultPath)
{
	if (savedPath.empty() || defaultPath.empty() || savedPath == defaultPath) return false;
	const bool originalExists = GetFileAttributesW(defaultPath.c_str()) != INVALID_FILE_ATTRIBUTES;
	if (originalExists)
	{
		// Never overwrite a prior backup. Copying first leaves the startup jam
		// available even if staging or replacement later fails.
		bool backedUp = false;
		for (unsigned int n = 0; n < 10000u; ++n)
		{
			const auto backup = defaultPath + L".bak" + (n ? L"." + std::to_wstring(n) : L"");
			if (CopyFileW(defaultPath.c_str(), backup.c_str(), TRUE)) { backedUp = true; break; }
			if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) return false;
		}
		if (!backedUp) return false;
	}
	for (unsigned int n = 0; n < 10000u; ++n)
	{
		const auto staged = defaultPath + L".new" + (n ? L"." + std::to_wstring(n) : L"");
		if (!CopyFileW(savedPath.c_str(), staged.c_str(), TRUE))
		{
			if (GetLastError() == ERROR_FILE_EXISTS || GetLastError() == ERROR_ALREADY_EXISTS) continue;
			return false;
		}
		const bool published = MoveFileExW(staged.c_str(), defaultPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
		if (!published) DeleteFileW(staged.c_str());
		return published;
	}
	return false;
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

	std::cout << "[BOOT] Loading JAM from defaults path: " << EncodeUtf8(ini.Jam) << std::endl;
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
		std::cerr << "[BOOT] JAM is unreadable; starting with an empty session: " << EncodeUtf8(ini.Jam) << std::endl;
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
		std::cerr << "[LOAD] Could not read JAM: " << EncodeUtf8(path) << std::endl;
		return std::nullopt;
	}

	auto [json, numChars, unused] = std::move(contents.value());
	std::stringstream stream(std::move(json));
	auto parsed = JamFile::FromStream(std::move(stream));
	if (!parsed.has_value())
		std::cerr << "[LOAD] JAM is unreadable: " << EncodeUtf8(path) << std::endl;
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

static const char* MidiStatusText(midi::MidiConnectionStatus status) noexcept
{
	switch (status)
	{
	case midi::MidiConnectionStatus::Disabled: return "disabled";
	case midi::MidiConnectionStatus::Missing: return "missing";
	case midi::MidiConnectionStatus::Ambiguous: return "ambiguous";
	case midi::MidiConnectionStatus::Failed: return "failed";
	case midi::MidiConnectionStatus::Connected: return "connected";
	case midi::MidiConnectionStatus::CapacityExceeded: return "capacity-exceeded";
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
	RigFile::Diagnostics diagnostics;
	auto parsed = RigFile::FromStream(std::move(ss), &diagnostics);
	for (const auto& diagnostic : diagnostics)
		std::cerr << "[RIG] " << (diagnostic.Level == RigFile::Severity::Error ? "Error: " : "Warning: ")
			<< diagnostic.Path << ": " << diagnostic.Message << std::endl;
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
	const auto initPath = ResolveIniPath();
	console::StartupLog startupLog(initPath);
	auto consoleLines = std::make_shared<console::LineRing>();
	console::ConsoleCapture capture(*consoleLines, {}, [&](std::string_view line) {
		startupLog.Append(line);
		if (auto failure = startupLog.TakeFailure(); !failure.empty())
		{
			consoleLines->Publish(failure);
			OutputDebugStringA(failure.c_str()); // No recursive stream logging.
		}
	});
	startupLog.Write(console::StartupLog::RunStart(LIB_VERSION));
	console::CrtCapture crtCapture(capture);
	const auto drainBackendDiagnostics = [] {
		if (auto fault = console::DiagnosticThread::TakeBackendError())
		{
			console::DiagnosticThread::StartupScope origin(fault->Startup);
			std::cerr << (fault->Type <= static_cast<int>(RtAudioError::DEBUG_WARNING) ? "[WARNING]" : "[ERROR]")
				<< "[ASIO] backend type=" << fault->Type << " message="
				<< std::string_view(fault->Text.data(), fault->Length) << std::endl;
		}
		if (const auto status = console::DiagnosticThread::ConsumeStartupAudioStatus())
		{
			console::DiagnosticThread::StartupScope origin;
			std::cerr << "[WARNING][ASIO] callback stream status=" << status << " (input overflow/output underflow)" << std::endl;
		}
		if (const auto lost = console::DiagnosticThread::ConsumeBackendDropped())
			std::cerr << "[WARNING][ASIO] backend diagnostics dropped/truncated=" << lost << std::endl;
		if (const auto status = console::DiagnosticThread::ConsumeAudioStatus())
		{
			console::DiagnosticThread::StartupScope origin(false);
			std::cerr << "[WARNING][ASIO] callback stream status=" << status << " (input overflow/output underflow)" << std::endl;
		}
		if (const auto suppressed = console::DiagnosticThread::ConsumeSuppressed())
			std::cerr << "[WARNING][diagnostics] suppressed " << suppressed << " callback stream writes; use bounded subsystem fault events" << std::endl;
	};
	try
	{
	struct DirectoryShutdown
	{
		~DirectoryShutdown() { ninjam::NinjamSession::ShutdownPublicServerDirectory(); }
	} directoryShutdown;
	console::ConsoleCapture::EnableForCurrentThread(true);
	{
		std::wstring executable(32768u, L'\0');
		const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
		if (length && length < executable.size())
		{
			executable.resize(length);
			std::cout << "[BOOT] executable=" << EncodeUtf8(executable) << std::endl;
		}
		const DWORD cwdLength = GetCurrentDirectoryW(0, nullptr);
		if (cwdLength > 0)
		{
			std::wstring cwd(cwdLength, L'\0');
			GetCurrentDirectoryW(cwdLength, cwd.data());
			if (!cwd.empty() && cwd.back() == L'\0')
				cwd.pop_back();
			std::cout << "[BOOT] cwd=" << EncodeUtf8(cwd) << std::endl;
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

	// Commands and controls have the same app/UI owner. Scene is resolved only
	// when the main loop executes a command, including after session replacement.
	console::CommandMailbox consoleCommands;
	console::ConsoleWindow consoleWindow(consoleCommands);
	console::GlobalKeyGate globalConsoleKeys;
	NetworkSession socketSession;
	if (!socketSession.IsInitialised())
	{
		startupLog.Write("[NINJAM] Failed to initialise socket library");
		return -1;
	}

	startupLog.Write("[BOOT] defaults=" + EncodeUtf8(initPath) + " support-log=" +
		EncodeUtf8(startupLog.Path()) + (startupLog.Available() ? "" : " (unavailable)"));
	const bool defaultsExisted = GetFileAttributesW(initPath.c_str()) != INVALID_FILE_ATTRIBUTES;
	startupLog.Write(std::string("[BOOT] defaults pre-existing=") + (defaultsExisted ? "yes" : "no"));
	auto defaults = LoadIni(initPath);
	bool defaultsValid = defaults.has_value();
	if (!defaultsValid)
	{
		if (defaultsExisted)
		{
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
	sceneParams.StartupDiagnostics = true;
	sceneParams.OnJobThreadStart = [] {
		console::ConsoleCapture::EnableForCurrentThread(true);
	};
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
		// Unavailable station targets remain in the complete canonical rig.
		// Recovery is reserved for unreadable or invalid definitions, never a
		// different jam or disconnected hardware. Load/close do not save the rig.
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
	const auto midiInventory = midi::MidiDevice::InventoryInputDevices();
	const bool hasConfiguredMidiInput = std::any_of(rig.User.Midi.Devices.begin(), rig.User.Midi.Devices.end(),
		[](const auto& device) { return device.Enabled; });
	if (hasConfiguredMidiInput && !midiInventory.Error.empty())
	{
		const auto failure = "[MIDI] configured input inventory failed: " + midiInventory.Error;
		startupLog.Write(failure);
		std::cout << failure << std::endl;
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

	std::function<bool(const io::RigFile&)> publishInitialRig;
	const auto generatedDirectory = utils::GetParentDirectory(initPath);
	const auto generatedRigPath = rigGenerated ? NewGeneratedPath(generatedDirectory, L"default", L".rig") : std::nullopt;
	const auto generatedRigPublished = std::make_shared<std::atomic<bool>>(false);
	if (generatedRigPath)
	{
		const auto path = *generatedRigPath;
		publishInitialRig = [path, generatedRigPublished](const io::RigFile& candidate) {
			const bool saved = !generatedRigPublished->load(std::memory_order_acquire) && SaveGeneratedRig(path, candidate);
			if (saved) generatedRigPublished->store(true, std::memory_order_release);
			return saved;
		};
	}

	// UI publishes the selected saved default to lifecycle callers, including
	// the MIDI job owner. Readers retain an immutable target through disk work.
	const auto hardwareDefaults = std::make_shared<std::atomic<std::shared_ptr<const io::InitFile>>>(
		std::make_shared<const io::InitFile>(*defaults));
	const auto saveRig = [hardwareDefaults](const io::RigFile& candidate) {
		const auto target = hardwareDefaults->load(std::memory_order_acquire);
		if (!target) return false;
		auto savedDefaults = *target;
		bool readable = false;
		auto saved = LoadRig(savedDefaults, readable);
		return saved && SaveRigAtomic(target->Rig, RigDefaultSave::WithHardwarePreferences(*saved, candidate));
	};
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
		startupLog.Write("[BOOT] Failed to create empty Scene; quitting");
		std::cout << "Failed to create empty Scene... quitting" << std::endl;
		return -1;
	}
	if (scene.value()->SnapshotStations().empty())
	{
		std::cerr << "[BOOT] Scene has no constructed Station." << std::endl;
		return -1;
	}

	sceneParams.StartupDiagnostics = false; // Runtime replacement jobs have runtime origin.
	if (defaults.has_value())
		scene.value()->SetLogging(defaults.value().Logging);
	ResourceLib resourceLib;
	Window window(*(scene.value()), resourceLib);

	if (window.Create(hInstance, nCmdShow, IDI_JAMMA, IDI_JAMMA_SMALL) != 0)
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
		if (!asioInventory.EnumerationError.empty())
		{
			const auto failure = "[ASIO] configured audio inventory failed: " + asioInventory.EnumerationError;
			startupLog.Write(failure);
			std::cout << failure << std::endl;
		}
	}
	if (!rigGenerated && !rig.User.Audio.Name.empty())
		for (const auto& device : asioInventory.Devices)
			if (device.Name == rig.User.Audio.Name && !device.ProbeError.empty())
			{
				const auto failure = "[ASIO] selected device '" + device.Name +
					"' could not be probed: " + device.ProbeError;
				startupLog.Write(failure);
				std::cout << failure << std::endl;
				break;
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
		const auto connection = scene.value()->GetMidiConnectionResult();
		const auto acceptedBeforeFinalisation = scene.value()->AcceptedRigSnapshot();
		const auto& openedRig = acceptedBeforeFinalisation ? acceptedBeforeFinalisation->Rig : rig;
		auto finalRig = io::StartupConfig::WithOpenedInputs(openedRig, actual.NumInputChannels,
			connection.ConnectedRegistryIds, jamGenerated ? "Station1" : jamForPublication.Stations.front().Name);

		std::stringstream initialStream, finalStream;
		RigFile::ToJsonStream(openedRig, initialStream);
		RigFile::ToJsonStream(finalRig, finalStream);
		if (initialStream.str() == finalStream.str())
		{
			if (publishInitialRig) publishInitialRig(finalRig);
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
		// First-run publication is explicit and independent of runtime transition acceptance.
		if (accepted && acceptedStream.str() == finalStream.str() && publishInitialRig) publishInitialRig(accepted->Rig);
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
	bool rigReadyForDefaults = !rigGenerated ||
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
		for (const auto channel : trigger.CaptureInputs.Audio)
			route << channel << "(physical " << channel + 1u << "),";
		route << " loop-midi=";
		for (const auto& name : trigger.CaptureInputs.Midi) route << "'" << name << "',";
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

	if (!jamGenerated && defaults)
	{
		scene.value()->SetSessionSaveFolder(utils::GetParentDirectory(defaults->Jam));
		if (defaults->JamOrigin != io::StartupConfig::GeneratedOrigin)
			scene.value()->SetCurrentJamPath(defaults->Jam);
	}
	const auto configureDefaultJamSave = [&]() {
		scene.value()->SetRuntimeDialogFallback([owner = window.GetWindowHandle()](const std::string& title, const std::string& message) {
			const auto wideTitle = utils::DecodeUtf8(title), wideMessage = utils::DecodeUtf8(message);
			MessageBoxW(owner,wideMessage.c_str(),wideTitle.c_str(),MB_OK | MB_ICONWARNING);
		});
		if (const auto currentDefaults = LoadIni(initPath))
			scene.value()->SetDefaultJamSaver(currentDefaults->Jam, [&](const std::wstring& savedPath, bool useNamedJam) {
			auto current = LoadIni(initPath);
			if (!current) return false;
			if (!useNamedJam) return PublishDefaultJam(savedPath, current->Jam);
			current->Jam = savedPath;
			current->JamLoadType = io::InitFile::LOAD_SPECIFIC;
			current->JamOrigin = "user";
			std::stringstream stream;
			if (!io::InitFile::ToStream(*current, stream) || !UpdateIni(initPath, stream.str())) return false;
			*defaults = *current;
			return true;
		});
	};
	configureDefaultJamSave();
	hardwareDefaults->store(std::make_shared<const io::InitFile>(*defaults), std::memory_order_release);
	RigDefaultSave rigDefaultSave(GetPath(PATH_ROAMING) + L"\\Jamma", initPath, SaveRigAtomic, UpdateIni);
	if (const auto accepted = scene.value()->AcceptedRigSnapshot())
		rigDefaultSave.InitializeCurrentPath(*defaults, !rigGenerated && defaults->RigOrigin != io::StartupConfig::GeneratedOrigin, accepted->Revision);
	MSG msg{};
	bool active = true;
	const auto startupDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	bool startupCompletionArmed = false;
	bool startupCaptureFinished = false;
	const auto startupJobsFinished = std::make_shared<std::atomic<bool>>(false);
	bool consoleCreationAttempted = false;
	bool pendingConsoleToggle = false;
	bool explicitConsoleAction = false;
	std::deque<std::string> pendingConsoleOutput;
	std::size_t pendingCaptureLoss = 0;
	JobFailureNotice jobFailureNotice;
	const auto workerFailurePath = JobFailureNotice::FallbackPath(initPath);
	const auto retainExitConsole = [&] {
		if (consoleWindow.Created()) JobFailureNotice::Drain(*consoleLines, pendingConsoleOutput, [&](const std::string& line) {
			consoleWindow.Append(std::span<const std::string>(&line, 1));
		});
	};
	while (active)
	{
		if (scene.value()->JobThreadFailed()) { retainExitConsole(); break; } // UI owns orderly stop and join below.
		// Dispatch one message before checking the budget so queued work always progresses.
		constexpr auto messagePumpBudget = std::chrono::milliseconds(2);
		const auto messagePumpStart = std::chrono::steady_clock::now();
		while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
		{
			if (msg.message == WM_DEVICECHANGE && msg.wParam == DBT_DEVNODES_CHANGED)
				scene.value()->RequestMidiRefresh();
			if (msg.message == WM_QUIT)
			{
				consoleCommands.Close();
				consoleWindow.Destroy();
				scene.value()->Shutdown();
				vst::DrainUiThreadDestroyQueue();
				active = false;
				break;
			}

			bool dispatch = true;
			if (msg.message == WM_KEYDOWN || msg.message == WM_KEYUP
				|| msg.message == WM_SYSKEYDOWN || msg.message == WM_SYSKEYUP)
			{
				const bool down = msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN;
				dispatch = globalConsoleKeys.Observe(static_cast<unsigned>(msg.wParam), down,
					consoleWindow.Contains(msg.hwnd), msg.hwnd == window.GetWindowHandle(),
					(msg.lParam & (1LL << 30)) != 0);
			}
			if (dispatch)
			{
				TranslateMessage(&msg);
				DispatchMessage(&msg);
			}
			if (std::chrono::steady_clock::now() - messagePumpStart >= messagePumpBudget)
				break;
		}

		if (!active)
			break;
		// Only admit another command after its ordered output has reached the view.
		// At most two ring snapshots, one echo/result and a loss notice are pending;
		// results cannot be lost when a command generates more output than the ring.
		drainBackendDiagnostics();
		if (!startupCaptureFinished)
		{
			if (startupJobsFinished->load(std::memory_order_acquire))
			{
				capture.CompleteStartup("[BOOT] startup restore and initial jobs complete");
				startupCaptureFinished = true;
			}
			else if (std::chrono::steady_clock::now() >= startupDeadline)
			{
				capture.CompleteStartup("[WARNING][BOOT] startup diagnostic window timed out after 30 seconds; continuous run.log capture remains active");
				startupCaptureFinished = true;
			}
		}
		std::size_t drainedThisFrame = 0;
		auto captureConsoleLines = [&] {
			pendingCaptureLoss += consoleLines->ConsumeDropped();
			if (pendingCaptureLoss)
			{
				pendingConsoleOutput.push_back("[CONSOLE] Lost " + std::to_string(pendingCaptureLoss) + " log entries (capture overflow)");
				pendingCaptureLoss = 0;
			}
			for (unsigned captured = 0; captured < console::LineRing::Capacity; ++captured)
			{
				auto line = consoleLines->Take();
				if (!line) break;
				pendingConsoleOutput.push_back(std::move(*line));
			}
		};
		auto drainConsoleLines = [&] {
			std::vector<std::string> batch;
			batch.reserve(console::ConsoleWindow::MaxAppendEntries);
			while (!pendingConsoleOutput.empty() && drainedThisFrame < console::ConsoleWindow::MaxAppendEntries)
			{
				batch.push_back(std::move(pendingConsoleOutput.front()));
				pendingConsoleOutput.pop_front();
				++drainedThisFrame;
			}
			consoleWindow.Append(batch);
		};
		if (consoleWindow.Created())
		{
			if (pendingConsoleOutput.empty())
			{
				captureConsoleLines();
				// A command uses the current Scene and its result remains behind all
				// accepted synchronous log output. One command per frame also bounds work.
				if (auto submission = consoleCommands.Take())
				{
					pendingConsoleOutput.push_back("[CONSOLE] > " + submission->Text);
					std::string result = "Command handled";
					try
					{
						if (!HandleSlashCommand(submission->Text, scene.value().get()))
							result = scene.value()->SendNinjamChat(submission->Text) ? "Chat sent" : "Chat not sent";
					}
					catch (const std::exception& error) { result = std::string("Command failed: ") + error.what(); }
					catch (...) { result = "Command failed"; }
					captureConsoleLines();
					if (result.size() > console::CommandMailbox::MaxTextBytes)
					{
						auto end = console::CommandMailbox::MaxTextBytes;
						// Keep the last accepted UTF-8 character complete when bounding errors.
						while (end && (static_cast<unsigned char>(result[end]) & 0xc0) == 0x80) --end;
						result.resize(end);
					}
					pendingConsoleOutput.push_back("[CONSOLE] " + result);
				}
			}
			drainConsoleLines();
		}
		consoleWindow.UpdateStatus(scene.value()->NinjamConnected());

		actions::KeyAction globalKeyAction;
		for (unsigned tracked = 0; tracked < 3 && scene.value()->PumpGlobalKeyCapture(globalKeyAction); ++tracked)
			if (globalConsoleKeys.Forward(globalKeyAction.KeyChar,
				globalKeyAction.KeyActionType == actions::KeyAction::KEY_DOWN, consoleWindow.Foreground()))
				window.OnAction(globalKeyAction);

		if (window.ConsumeConsoleToggleRequest())
		{
			explicitConsoleAction = true;
			if (consoleCreationAttempted)
			{
				// Retry only for an explicit user action, never once per frame.
				if (!consoleWindow.Created() && !consoleWindow.CreateHidden(hInstance))
					std::cerr << "[CONSOLE] Could not create native command window.\n";
				else consoleWindow.Toggle();
			}
			else pendingConsoleToggle = !pendingConsoleToggle;
		}
		if (scene.value()->ConsumePanelRigLoadRequest())
		{
			wchar_t filename[32768] = L"";
			OPENFILENAMEW dialog{};
			dialog.lStructSize = sizeof(dialog); dialog.hwndOwner = window.GetWindowHandle();
			dialog.lpstrFilter = L"Jamma rig (*.rig)\0*.rig\0\0";
			dialog.lpstrFile = filename; dialog.nMaxFile = static_cast<DWORD>(std::size(filename));
			dialog.lpstrTitle = L"Load rig";
			dialog.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST;
			if (GetOpenFileNameW(&dialog))
			{
				auto selected = *defaults; selected.Rig = filename;
				bool readable = false;
				const auto candidate = LoadRig(selected, readable);
				engine::RigCoordinator::EditReceiptPtr receipt;
				if (!candidate || scene.value()->RequestRigEdit(*candidate, &receipt) != engine::RigCoordinator::EditResult::Pending)
					scene.value()->ReportRuntimeNotice("Load rig", "Rig could not be applied. Check the log; device setup changes require a restart.");
				else rigDefaultSave.TrackLoadedPath(filename, scene.value()->Identity(), std::move(receipt));
			}
		}
		const bool windowLoadRequested = window.ConsumeJamLoadRequest();
		const bool panelLoadRequested = scene.value()->ConsumePanelLoadRequest();
		if (windowLoadRequested || panelLoadRequested)
		{
			// Resolve the picker before admission/suspension; cancel changes nothing.
			scene.value()->PickSessionForLoad();
		}
		scene.value()->PumpDeviceAndFileControls();
		const auto pendingJamPath = scene.value()->TakeReadySessionLoad();
		if (!pendingJamPath.empty())
		{
			std::optional<std::shared_ptr<Scene>> replacement;
			std::string loadError;
			try
			{
				auto selectedJam = LoadJamFile(pendingJamPath);
				if (selectedJam)
				{
					const auto activeRig = scene.value()->AcceptedRigSnapshot();
					replacement = Scene::FromFile(sceneParams, std::move(*selectedJam),
						activeRig ? activeRig->Rig : rig, utils::GetParentDirectory(pendingJamPath), saveRig);
				}
			}
			catch (const std::exception& error) { loadError = error.what(); }
			catch (...) { loadError = "Unknown construction error"; }
			if (!replacement)
				scene.value()->FinishSessionLoad(false, "Load failed; current session retained: " + loadError);
			else
			{
				if (defaults) replacement.value()->SetLogging(defaults->Logging);
				scene.value()->FinishSessionLoad(true);
				scene.value()->Shutdown();
				if (scene.value()->JobThreadFailed()) break; // Keep outgoing failure for owner notification below.
				rigDefaultSave.PollLoadedPath(scene.value()->Identity()); // Replacement carries this accepted rig.
				window.ReplaceScene(*replacement.value());
				scene = std::move(replacement);
				rigDefaultSave.InvalidateSavedRevision();
				vst::DrainUiThreadDestroyQueue();
				scene.value()->SetSessionSaveFolder(utils::GetParentDirectory(pendingJamPath));
				scene.value()->SetCurrentJamPath(pendingJamPath);
				configureDefaultJamSave();
				scene.value()->InitGlobalKeyCapture();
				// Preserve rig hardware choices and inventory across loaded content.
				scene.value()->InitAudio(false, nullptr, true);
				const bool started = scene.value()->GetAsioOpenReport().StartedParams.has_value();
				scene.value()->ReportFileOutcome(started ? "Loaded session (check log for asset warnings)" : "Loaded session; audio stopped, retry available");
			}
		}

		if (rigDefaultSave.Poll(*scene.value(), window.GetWindowHandle(), *defaults)) { rigReadyForDefaults = true; defaultsValid = true; hardwareDefaults->store(std::make_shared<const io::InitFile>(*defaults), std::memory_order_release); }
		if (!rigDefaultSave.Busy() && scene.value()->ConsumeRigSaveRequest())
			rigDefaultSave.Start(*scene.value(), window.GetWindowHandle(), *defaults, scene.value()->RequestedRigSaveMode());
		window.Render();
		window.Swap();
		if (!startupCompletionArmed)
		{
			startupCompletionArmed = true;
			scene.value()->CompleteStartupWhenIdle([startupJobsFinished] {
				startupJobsFinished->store(true, std::memory_order_release);
			});
		}
		if (!consoleCreationAttempted)
		{
			consoleCreationAttempted = true;
			if (!consoleWindow.CreateHidden(hInstance))
				std::cerr << "[CONSOLE] Could not create native command window.\n";
			else if (pendingConsoleToggle) consoleWindow.Toggle();
			else if (!explicitConsoleAction && defaults && defaults->ConsoleAutoStart) consoleWindow.ShowPassive(window.GetWindowHandle());
		}

		// Destroy any VST plugins that failed to load on the job thread.
		// They were PreInit'd on this thread, so cleanup must run here too.
		vst::DrainUiThreadDestroyQueue();
	}

	// Shutdown stops the audio callback before it closes VST editor windows and
	// releases their plugins, while this main thread's COM STA is still valid.
	consoleCommands.Close();
	scene.value()->Shutdown();
	retainExitConsole();
	consoleWindow.Destroy();

	const bool jobThreadFailed = scene.value()->JobThreadFailed();
	jobFailureNotice.Report(scene.value()->JobThreadFailureDetail(),
		[&](std::string_view detail) { return JobFailureNotice::Show(window.GetWindowHandle(), detail); },
		[&](std::string_view detail) { return JobFailureNotice::Persist(workerFailurePath.c_str(), detail); },
		JobFailureNotice::Debug);
	if (!jobThreadFailed && rigDefaultSave.Poll(*scene.value(), window.GetWindowHandle(), *defaults, false)) { rigReadyForDefaults = true; defaultsValid = true; hardwareDefaults->store(std::make_shared<const io::InitFile>(*defaults), std::memory_order_release); }
	if (!jobThreadFailed && defaultsValid && rigReadyForDefaults && jamReadyForDefaults)
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

	// Join Scene and directory workers before restoring global stream buffers.
	ninjam::NinjamSession::ShutdownPublicServerDirectory();
	scene.reset();

	// Drain once more for any failed-load plugins queued late during Scene teardown.
	vst::DrainUiThreadDestroyQueue();

	if (uiComInitialized)
		CoUninitialize();
	drainBackendDiagnostics();
	crtCapture.Stop();
	capture.Stop();

	return jobThreadFailed ? 1 : (int)msg.wParam;
	}
	catch (const std::exception& error)
	{
		drainBackendDiagnostics();
		std::cerr << "[ERROR][app] unexpected startup/runtime exception: " << error.what() << std::endl;
	}
	catch (...)
	{
		drainBackendDiagnostics();
		std::cerr << "[ERROR][app] unexpected startup/runtime exception; exiting" << std::endl;
	}
	return -1;
}
