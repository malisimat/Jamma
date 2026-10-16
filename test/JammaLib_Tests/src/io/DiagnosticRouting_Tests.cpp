#include "../../../../JammaLib/src/engine/Scene.h"
#include "../../../../Jamma/src/StartupLog.h"
#include "../../../../Jamma/src/ConsoleCapture.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
#include <sstream>
#include "../../../../JammaLib/src/io/TextReadWriter.h"
#include "../../../../JammaLib/src/io/WavReadWriter.h"
namespace console
{
	struct StartupLogTestAccess
	{
		static void FailWrites(StartupLog& log)
		{
			CloseHandle(log._file);
			log._file = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
		}
	};
}
class DiagnosticFiles : public testing::Test
{
protected:
	std::filesystem::path Directory;
	void SetUp() override
	{
		Directory = std::filesystem::temp_directory_path() / (L"Jamma-diagnostics-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
		std::filesystem::create_directories(Directory);
	}
	void TearDown() override { std::error_code error; std::filesystem::remove_all(Directory, error); }
	std::string ReadLog()
	{
		std::ifstream file(Directory / "startup.log", std::ios::binary);
		return std::string(std::istreambuf_iterator<char>(file), {});
	}
};
TEST_F(DiagnosticFiles, StartupLogCapturesEarlyWorkerPartialAndRotatesWithoutRuntimeDuplication)
{
	console::LineRing ring;
	{
		console::StartupLog log((Directory / "defaults.json").wstring());
		ASSERT_TRUE(log.Available());
		console::ConsoleCapture capture(ring, [&](std::string_view line) { log.Append(line); });
		std::cout << "early\n";
		std::thread worker([] { std::cerr << "worker failure" << std::flush; }); worker.join();
		capture.CompleteStartup();
		std::cout << "runtime\n";
	}
	EXPECT_EQ(ReadLog(), "early\r\nworker failure\r\n");
	{
		console::StartupLog log((Directory / "defaults.json").wstring());
		EXPECT_TRUE(std::filesystem::exists(Directory / "startup.log.1"));
		log.Append("next launch");
	}
	EXPECT_EQ(ReadLog(), "next launch\r\n");
}
TEST_F(DiagnosticFiles, StartupLogReportsUnavailableAndSaturatedSinks)
{
	{
		std::ofstream block(Directory / "blocked"); block << "file";
	}
	console::StartupLog unavailable((Directory / "blocked" / "defaults.json").wstring());
	EXPECT_FALSE(unavailable.Available());
	EXPECT_NE(unavailable.TakeFailure().find("create-directory"), std::string::npos);
	EXPECT_TRUE(unavailable.TakeFailure().empty());
	console::StartupLog bounded((Directory / "defaults.json").wstring(), 8);
	bounded.Append("fits"); bounded.Append("too large");
	EXPECT_NE(bounded.TakeFailure().find("size-limit"), std::string::npos);
	bounded.Append("ignored");
	EXPECT_TRUE(bounded.TakeFailure().empty());
	EXPECT_EQ(ReadLog(), "fits\r\n");
}


TEST_F(DiagnosticFiles, StartupLogWriteFailureIsReportedOnceWithoutRecursion)
{
	console::StartupLog log((Directory / "defaults.json").wstring());
	console::StartupLogTestAccess::FailWrites(log);
	log.Append("cannot write");
	EXPECT_FALSE(log.Available());
	EXPECT_NE(log.TakeFailure().find("stage=write"), std::string::npos);
	log.Append("ignored");
	EXPECT_TRUE(log.TakeFailure().empty());
}
TEST_F(DiagnosticFiles, MissingJamAndMaterialReportPathsWithVerboseLoggingDisabled)
{
	console::LineRing ring;
	{
		console::ConsoleCapture capture(ring);
		io::TextReadWriter text;
		EXPECT_FALSE(text.Read((Directory / "missing.jam").wstring(), 4096));
		io::WavReadWriter wav;
		EXPECT_FALSE(wav.Read((Directory / "missing.wav").wstring(), 4096));
	}
	bool jam = false, wav = false;
	while (auto line = ring.Take())
	{
		jam |= line->find("[ERROR][file] read-open path=") != std::string::npos && line->find("missing.jam") != std::string::npos;
		wav |= line->find("[ERROR][WAV] read-open path=") != std::string::npos && line->find("missing.wav") != std::string::npos;
	}
	EXPECT_TRUE(jam); EXPECT_TRUE(wav);
}
TEST(ConsoleSession, BackendFaultMailboxIsBoundedAndReportsSaturationAndStatus)
{
	console::DiagnosticThread::StartupWindow(false);
	while (console::DiagnosticThread::TakeBackendError()) {}
	console::DiagnosticThread::ConsumeBackendDropped();
	std::thread callback([] {
		console::DiagnosticThread::BackendError(8, "device disconnected");
		console::DiagnosticThread::BackendError(8, "second fault");
		console::DiagnosticThread::AudioStatus(1);
		console::DiagnosticThread::AudioStatus(2);
	}); callback.join();
	auto event = console::DiagnosticThread::TakeBackendError();
	ASSERT_TRUE(event);
	EXPECT_EQ(event->Type, 8);
	EXPECT_EQ(std::string_view(event->Text.data(), event->Length), "device disconnected");
	EXPECT_FALSE(console::DiagnosticThread::TakeBackendError());
	EXPECT_EQ(console::DiagnosticThread::ConsumeBackendDropped(), 1u);
	EXPECT_EQ(console::DiagnosticThread::ConsumeAudioStatus(), 3u);
	console::DiagnosticThread::BackendError(8, std::string(1024, 'x'));
	event = console::DiagnosticThread::TakeBackendError();
	ASSERT_TRUE(event); EXPECT_EQ(event->Length, 512u);
	EXPECT_EQ(console::DiagnosticThread::ConsumeBackendDropped(), 1u);
}


TEST_F(DiagnosticFiles, StartupLogFlushesEarlyReturnAndWorkerExit)
{
	console::LineRing ring;
	const auto failEarly = [&] {
		console::StartupLog log((Directory / "defaults.json").wstring());
		console::ConsoleCapture capture(ring, [&](std::string_view line) { log.Append(line); });
		std::thread worker([] { std::cerr << "worker startup failure"; }); worker.join();
		std::cout << "early exit tail";
		return -1;
	};
	EXPECT_EQ(failEarly(), -1);
	EXPECT_EQ(ReadLog(), "worker startup failure\r\nearly exit tail\r\n");
	EXPECT_EQ(ring.Take(), "worker startup failure");
	EXPECT_EQ(ring.Take(), "early exit tail");
	EXPECT_FALSE(ring.Take());
}

namespace engine
{
	class SceneStartupDiagnosticTestAccess
	{
	public:
		static void Enqueue(Scene& scene, actions::JobAction job)
		{
			std::vector<actions::JobAction> jobs;
			jobs.push_back(std::move(job));
			scene._QueueJobs(std::move(jobs));
		}
	};
}
class DelayedDiagnosticReceiver : public base::ActionReceiver
{
public:
	std::atomic<bool> Started{false}, Release{false};
	std::vector<std::pair<std::string, bool>> Origins;
	actions::ActionResult OnAction(actions::JobAction action) override
	{
		if (action.SourceId == "first startup")
		{
			Started.store(true, std::memory_order_release);
			while (!Release.load(std::memory_order_acquire)) std::this_thread::yield();
		}
		Origins.emplace_back(action.SourceId, console::DiagnosticThread::StartupOwned());
		std::cerr << action.SourceId << " failure\n";
		return actions::ActionResult::NoAction();
	}
};
class StartupJobOrigins : public testing::TestWithParam<bool> {};
TEST_P(StartupJobOrigins, InterleavedJobsRespectExplicitOrigin)
{
	console::LineRing ring;
	std::vector<std::string> startup;
	console::ConsoleCapture capture(ring, [&](std::string_view line) { startup.emplace_back(line); });
	auto rig = io::RigFile::FromStream(std::stringstream(io::RigFile::DefaultJson)).value();
	rig.Triggers.clear(); rig.User.Midi.Devices.clear();
	io::JamFile jam{};
	io::JamFile::Station station{}; station.Name = "Diagnostic station"; jam.Stations.push_back(station);
	engine::SceneParams params({ "" }, {}, {640u, 480u});
	params.StartupDiagnostics = true;
	auto created = engine::Scene::FromFile(params, jam, rig, L"", [](const auto&) { return true; });
	ASSERT_TRUE(created);
	auto scene = *created;
	auto receiver = std::make_shared<DelayedDiagnosticReceiver>();
	struct ReleaseOnExit
	{
		std::shared_ptr<DelayedDiagnosticReceiver> Receiver;
		~ReleaseOnExit() { Receiver->Release.store(true, std::memory_order_release); }
	} release{receiver};
	const auto enqueue = [&](const char* source) {
		actions::JobAction job; job.JobActionType = actions::JobAction::JOB_UPDATELOOPS;
		job.SourceId = source; job.Receiver = receiver;
		engine::SceneStartupDiagnosticTestAccess::Enqueue(*scene, std::move(job));
	};
	enqueue("first startup");
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (!receiver->Started.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
		std::this_thread::yield();
	ASSERT_TRUE(receiver->Started.load());
	enqueue("second startup");
	std::atomic<bool> finished{false};
	scene->CompleteStartupWhenIdle([&] { finished.store(true, std::memory_order_release); });
	enqueue("runtime job");
	// Directly close the global window: this simulates the 30s deadline without
	// waiting in the test while the first startup job remains in flight.
	if (GetParam()) capture.CompleteStartup("startup window expired");
	{
		console::DiagnosticThread::StartupScope runtime(false);
		std::cout << "runtime UI failure\n";
	}
	receiver->Release.store(true, std::memory_order_release);
	while (!finished.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
		std::this_thread::yield();
	EXPECT_TRUE(finished.load());
	scene->Shutdown();
	const std::vector<std::pair<std::string, bool>> expected{{"first startup", true}, {"second startup", true}, {"runtime job", false}};
	EXPECT_EQ(receiver->Origins, expected);
	EXPECT_NE(std::find(startup.begin(), startup.end(), "first startup failure"), startup.end());
	EXPECT_NE(std::find(startup.begin(), startup.end(), "second startup failure"), startup.end());
	EXPECT_EQ(std::find(startup.begin(), startup.end(), "runtime job failure"), startup.end());
	EXPECT_EQ(std::find(startup.begin(), startup.end(), "runtime UI failure"), startup.end());
}
INSTANTIATE_TEST_SUITE_P(ConsoleSession, StartupJobOrigins, testing::Bool());
TEST(ConsoleSession, BackendEventsKeepStartupOriginUntilDrainedAfterWindowEnds)
{
	while (console::DiagnosticThread::TakeBackendError()) {}
	console::DiagnosticThread::ConsumeAudioStatus();
	console::DiagnosticThread::ConsumeStartupAudioStatus();
	console::LineRing ring;
	std::vector<std::string> startup;
	console::ConsoleCapture capture(ring, [&](std::string_view line) { startup.emplace_back(line); });
	console::DiagnosticThread::BackendError(8, "startup backend fault");
	console::DiagnosticThread::AudioStatus(1);
	capture.CompleteStartup();
	auto event = console::DiagnosticThread::TakeBackendError(); ASSERT_TRUE(event);
	EXPECT_TRUE(event->Startup);
	{
		console::DiagnosticThread::StartupScope origin(event->Startup);
		std::cerr << std::string_view(event->Text.data(), event->Length) << '\n';
	}
	EXPECT_EQ(console::DiagnosticThread::ConsumeStartupAudioStatus(), 1u);
	console::DiagnosticThread::BackendError(8, "runtime backend fault");
	event = console::DiagnosticThread::TakeBackendError(); ASSERT_TRUE(event);
	EXPECT_FALSE(event->Startup);
	{
		console::DiagnosticThread::StartupScope origin(event->Startup);
		std::cerr << std::string_view(event->Text.data(), event->Length) << '\n';
	}
	EXPECT_EQ(startup, (std::vector<std::string>{"startup backend fault"}));
}

TEST(ConsoleSession, StartupOriginChangeFlushesPartialWithoutTeeingRuntimeTail)
{
	console::LineRing ring;
	std::vector<std::string> startup;
	{
		console::ConsoleCapture capture(ring, [&](std::string_view line) { startup.emplace_back(line); });
		capture.CompleteStartup();
		{
			console::DiagnosticThread::StartupScope origin;
			std::cout << "late startup partial";
		}
		std::cout << "runtime partial" << std::flush;
	}
	EXPECT_EQ(startup, (std::vector<std::string>{"late startup partial"}));
	EXPECT_EQ(ring.Take(), "late startup partial");
	EXPECT_EQ(ring.Take(), "runtime partial");
	EXPECT_FALSE(ring.Take());
}

TEST(ConsoleSession, ExplicitBackendOriginOverridesOpenStartupWindowAndNestedScopesRestore)
{
	using Thread = console::DiagnosticThread;
	while (Thread::TakeBackendError()) {}
	Thread::ConsumeAudioStatus(); Thread::ConsumeStartupAudioStatus();
	console::LineRing ring;
	std::vector<std::string> startup;
	console::ConsoleCapture capture(ring, [&](std::string_view line) { startup.emplace_back(line); });
	const auto drain = [&](bool expectedStartup) {
		auto event = Thread::TakeBackendError(); ASSERT_TRUE(event);
		EXPECT_EQ(event->Startup, expectedStartup);
		Thread::StartupScope origin(event->Startup);
		std::cerr << std::string_view(event->Text.data(), event->Length) << '\n';
	};
	{
		Thread::StartupScope runtime(false);
		Thread::BackendError(8, "runtime during startup"); Thread::AudioStatus(2); drain(false);
		{
			Thread::StartupScope startup;
			Thread::BackendError(8, "explicit startup"); Thread::AudioStatus(1); drain(true);
		}
		Thread::BackendError(8, "runtime after nested startup"); drain(false);
	}
	Thread::BackendError(8, "unspecified during startup"); Thread::AudioStatus(4); drain(true);
	EXPECT_EQ(Thread::ConsumeAudioStatus(), 2u);
	EXPECT_EQ(Thread::ConsumeStartupAudioStatus(), 5u);
	EXPECT_EQ(startup, (std::vector<std::string>{"explicit startup", "unspecified during startup"}));
}
