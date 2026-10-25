#include "../../../../JammaLib/src/engine/Scene.h"
#include "../../../../Jamma/src/StartupLog.h"
#include "../../../../Jamma/src/ConsoleCapture.h"
#include "../../../../Jamma/src/CrtCapture.h"
#include "../../../../Jamma/src/JobFailureNotice.h"
#include "../../../../Jamma/src/ConsoleWindow.h"
#include "../../../../console/CommandMailbox.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
#include <sstream>
#include <set>
#include "../../../../JammaLib/src/io/TextReadWriter.h"
#include "../../../../JammaLib/src/io/WavReadWriter.h"
#include "../../../../JammaLib/src/vst/Vst3Plugin.h"
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
		std::ifstream file(Directory / "run.log", std::ios::binary);
		return std::string(std::istreambuf_iterator<char>(file), {});
	}
};
TEST_F(DiagnosticFiles, RunLogCapturesEarlyWorkerPartialAndRuntimeOnceAndRotates)
{
	console::LineRing ring;
	const auto header = console::StartupLog::RunStart("test-version");
	EXPECT_NE(header.find(" Jamma vtest-version"), std::string::npos);
	{
		console::StartupLog log((Directory / "defaults.json").wstring());
		ASSERT_TRUE(log.Available());
		console::ConsoleCapture capture(ring, {}, [&](std::string_view line) { log.Append(line); });
		std::cout << header << '\n';
		std::cout << "early\n";
		std::thread worker([] { std::cerr << "worker failure" << std::flush; }); worker.join();
		capture.CompleteStartup();
		std::cout << "runtime\n";
	}
	EXPECT_EQ(ReadLog(), header + "\r\nearly\r\nworker failure\r\nruntime\r\n");
	{
		console::StartupLog log((Directory / "defaults.json").wstring());
		EXPECT_TRUE(std::filesystem::exists(Directory / "run.log.1"));
		log.Append("next launch");
	}
	EXPECT_EQ(ReadLog(), "next launch\r\n");
}
TEST_F(DiagnosticFiles, RunLogReportsUnavailableSinkOnce)
{
	{ std::ofstream block(Directory / "blocked"); block << "file"; }
	console::StartupLog unavailable((Directory / "blocked" / "defaults.json").wstring());
	EXPECT_FALSE(unavailable.Available());
	EXPECT_NE(unavailable.TakeFailure().find("create-directory"), std::string::npos);
	EXPECT_TRUE(unavailable.TakeFailure().empty());
}
TEST_F(DiagnosticFiles, RunLogRotatesDuringRunAndBoundsEachGeneration)
{
	console::StartupLog log((Directory / "defaults.json").wstring(), 8);
	for (unsigned i = 0; i < 5; ++i) log.Append("line-" + std::to_string(i));
	EXPECT_TRUE(log.Available());
	EXPECT_TRUE(log.TakeFailure().empty());
	EXPECT_EQ(ReadLog(), "line-4\r\n");
	for (unsigned i = 1; i <= 3; ++i)
	{
		const auto path = Directory / ("run.log." + std::to_string(i));
		EXPECT_LE(std::filesystem::file_size(path), 8u);
		std::ifstream file(path, std::ios::binary);
		EXPECT_EQ(std::string(std::istreambuf_iterator<char>(file), {}), "line-" + std::to_string(4 - i) + "\r\n");
	}
	EXPECT_FALSE(std::filesystem::exists(Directory / "run.log.4"));
}
TEST_F(DiagnosticFiles, RunLogBoundsOversizedDirectEntriesAndRetainsUtf8Tail)
{
	console::StartupLog log((Directory / "defaults.json").wstring(), 8);
	log.Append("abc\xF0\x9F\x8E\xB5" "def");
	EXPECT_TRUE(log.TakeFailure().empty());
	EXPECT_LE(std::filesystem::file_size(Directory / "run.log"), 8u);
	std::string joined;
	for (int i = 2; i >= 0; --i)
	{
		std::ifstream file(Directory / (i ? "run.log." + std::to_string(i) : "run.log"), std::ios::binary);
		auto part = std::string(std::istreambuf_iterator<char>(file), {});
		ASSERT_GE(part.size(), 2u);
		joined += part.substr(0, part.size() - 2);
	}
	EXPECT_EQ(joined, "abc\xF0\x9F\x8E\xB5" "def");
}
TEST_F(DiagnosticFiles, RunLogReportsRotationDenialWithoutDestroyingOldLog)
{
	const auto path = Directory / L"run.log";
	{ std::ofstream file(path); file << "prior run"; }
	const auto locked = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
	ASSERT_NE(locked, INVALID_HANDLE_VALUE);
	console::StartupLog log((Directory / "defaults.json").wstring());
	EXPECT_FALSE(log.Available());
	EXPECT_NE(log.TakeFailure().find("rotation-move"), std::string::npos);
	EXPECT_TRUE(log.TakeFailure().empty());
	CloseHandle(locked);
	EXPECT_EQ(ReadLog(), "prior run");
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
	console::StartupLog log((Directory / L"defaults.json").wstring());
	{
		console::ConsoleCapture capture(ring, {}, [&](std::string_view line) { log.Append(line); });
		capture.CompleteStartup();
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
	const auto retained = ReadLog();
	EXPECT_NE(retained.find("missing.jam"), std::string::npos);
	EXPECT_NE(retained.find("missing.wav"), std::string::npos);
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
		console::ConsoleCapture capture(ring, {}, [&](std::string_view line) { log.Append(line); });
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
		static bool Quitting(Scene& scene) { return scene._isSceneQuitting.load(std::memory_order_acquire); }
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
TEST(ConsoleSession, VstLifecycleAndFailuresRemainInFileAfterStartup)
{
	console::LineRing ring;
	std::vector<std::string> saved;
	console::ConsoleCapture capture(ring, {}, [&](std::string_view line) { saved.emplace_back(line); });
	capture.CompleteStartup();
	{
		console::DiagnosticThread::StartupScope runtime(false);
		std::clog << "[VST][load] stage=begin" << std::endl;
		std::cerr << "[ERROR][VST2] windows-error=126" << std::endl;
		std::cout << "ordinary runtime output" << std::endl;
	}
	EXPECT_EQ(saved, (std::vector<std::string>{"[VST][load] stage=begin", "[ERROR][VST2] windows-error=126", "ordinary runtime output"}));
}

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
class ThrowingDiagnosticReceiver : public base::ActionReceiver
{
public:
	actions::ActionResult OnAction(actions::JobAction) override { throw std::runtime_error("receiver exploded"); }
};
class JobThreadFailures : public testing::TestWithParam<int> {};
TEST_P(JobThreadFailures, PublicCallbackFailureStopsWorkerAndOwnerShutdownJoins)
{
	console::LineRing ring;
	console::ConsoleCapture capture(ring);
	auto rig = io::RigFile::FromStream(std::stringstream(io::RigFile::DefaultJson)).value();
	rig.Triggers.clear(); rig.User.Midi.Devices.clear();
	io::JamFile jam{};
	io::JamFile::Station station{}; station.Name = "Failure station"; jam.Stations.push_back(station);
	engine::SceneParams params({ "" }, {}, {640u, 480u});
	std::atomic<bool> release{false};
	const int mode = GetParam();
	params.OnJobThreadStart = [&] {
		if (mode == 1) throw std::runtime_error("initialization exploded");
		if (mode == 3) throw 7;
		if (mode == 2)
		{
			const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(3);
			while (!release.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < limit)
				std::this_thread::yield();
		}
	};
	auto created = engine::Scene::FromFile(params, jam, rig, L"", [](const auto&) { return true; });
	ASSERT_TRUE(created);
	auto scene = *created;
	std::atomic<unsigned> calls{0};
	console::CommandMailbox commands;
	console::ConsoleWindow view(commands);
	std::vector<std::string> retained;
	std::deque<std::string> pending;
	const auto drainToView = [&] {
		JobFailureNotice::Drain(ring, pending, [&](const std::string& line) {
			retained.push_back(line); view.Append(std::span<const std::string>(&line, 1));
		});
	};
	if (mode == 0 || mode == 2 || mode == 5)
	{
		ASSERT_TRUE(view.CreateHidden(GetModuleHandleW(nullptr)));
		capture.CompleteStartup();
		drainToView(); // Last ordinary UI drain precedes either failure ordering.
		retained.clear();
	}
	auto receiver = std::make_shared<ThrowingDiagnosticReceiver>();
	if (mode == 4)
	{
		actions::JobAction job; job.JobActionType = actions::JobAction::JOB_UPDATELOOPS; job.Receiver = receiver;
		job.VstPath = L"test-receiver.vst3";
		engine::SceneStartupDiagnosticTestAccess::Enqueue(*scene, std::move(job));
	}
	if (mode == 0 || mode == 2 || mode == 5)
		scene->QueueExplicitRigSave([&] { ++calls; throw std::runtime_error(mode == 5 ? std::string(10000, 'x') : "save exploded"); });
	if (mode == 2)
	{
		// Console is already gone when Shutdown runs the throwing final save.
		view.Destroy();
		for (std::size_t i = 0; i < console::LineRing::Capacity; ++i) ASSERT_TRUE(ring.Publish("saturated console"));
		std::thread signal([&] {
			const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(3);
			while (!engine::SceneStartupDiagnosticTestAccess::Quitting(*scene) && std::chrono::steady_clock::now() < limit)
				std::this_thread::yield();
			release.store(true, std::memory_order_release);
		});
		scene->Shutdown();
		signal.join();
	}
	else
	{
		const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		while (!scene->JobThreadFailed() && std::chrono::steady_clock::now() < limit) std::this_thread::yield();
		EXPECT_TRUE(scene->JobThreadFailed());
		// Same public failure notification checked by the app UI before its normal
		// Shutdown path. Work queued after failure must never run, including save.
		scene->QueueExplicitRigSave([&] { calls.fetch_add(100); });
		scene->OnJobTick(std::chrono::steady_clock::now());
		scene->Shutdown();
		if (mode == 0) drainToView(); // Runtime failure retained before view teardown.
		view.Destroy();
	}
	EXPECT_TRUE(scene->JobThreadFailed());
	scene->Shutdown(); // repeated UI/destructor teardown remains safe
	EXPECT_EQ(calls.load(), mode == 0 || mode == 2 || mode == 5 ? 1u : 0u);
	// Delivery is independent of both the destroyed console and its undrained
	// ring. The immutable detail remains readable before and after join.
	const auto detail = scene->JobThreadFailureDetail();
	ASSERT_FALSE(detail.empty());
	unsigned presented = 0, fallback = 0;
	std::string delivered;
	JobFailureNotice notice;
	const auto show = [&](std::string_view message) {
		EXPECT_FALSE(view.Created()); ++presented; delivered = message; return true;
	};
	notice.Report(detail, show, [&](auto) { ++fallback; return false; }, [](auto) {});
	notice.Report(detail, show, [&](auto) { ++fallback; return false; }, [](auto) {});
	EXPECT_EQ(presented, 1u); EXPECT_EQ(fallback, 0u);
	EXPECT_EQ(delivered, detail);
	if (mode == 5) { EXPECT_LE(detail.size(), 4095u); EXPECT_TRUE(detail.ends_with(" [detail truncated]")); }
	if (mode == 2) EXPECT_TRUE(retained.empty()); // No post-destroy transcript drain.
	unsigned errors = 0;
	while (auto line = ring.Take()) retained.push_back(std::move(*line));
	for (const auto& value : retained)
	{
		const auto* line = &value;
		if (line->find("[ERROR][job]") == std::string::npos) continue;
		++errors;
		EXPECT_NE(line->find("worker stopped; shutdown requested"), std::string::npos);
		EXPECT_NE(line->find(mode == 2 ? "stage=final-save" : (mode == 0 || mode == 5) ? "stage=pump" : mode == 4 ? "stage=receiver" : "stage=initialize"), std::string::npos);
	}
	EXPECT_EQ(errors, mode == 2 ? 0u : 1u);
	if (mode == 2) EXPECT_GT(ring.ConsumeDropped(), 0u);
}
INSTANTIATE_TEST_SUITE_P(ConsoleSession, JobThreadFailures, testing::Values(0, 1, 2, 3, 4, 5));
TEST_F(DiagnosticFiles, OwnerFailureNoticePersistsWhenModalFailsWithoutUsingCapturedStreams)
{
	console::LineRing ring;
	console::ConsoleCapture capture(ring);
	capture.CompleteStartup();
	const std::string detail = "[ERROR][job] stage=final-save exception=test; shutdown requested";
	const auto path = std::filesystem::path(JobFailureNotice::FallbackPath((Directory / L"defaults.json").wstring()));
	EXPECT_EQ(path.parent_path(), Directory); EXPECT_EQ(path.filename(), L"worker-failure.log");
	JobFailureNotice notice;
	unsigned presented = 0, debugged = 0;
	notice.Report(detail, [&](auto) -> bool { ++presented; throw std::runtime_error("modal unavailable"); },
		[&](auto text) { return JobFailureNotice::Persist(path.c_str(), text); }, [&](auto) { ++debugged; });
	std::ifstream file(path, std::ios::binary);
	EXPECT_EQ(std::string(std::istreambuf_iterator<char>(file), {}), detail);
	EXPECT_EQ(presented, 1u); EXPECT_EQ(debugged, 0u);
	EXPECT_FALSE(ring.Take()); // No recursive stdout/stderr failure report.
}
TEST_F(DiagnosticFiles, OwnerFailureNoticeUsesDirectDebugFallbackIfModalAndFileFail)
{
	console::LineRing ring;
	console::ConsoleCapture capture(ring);
	capture.CompleteStartup();
	std::string delivered;
	JobFailureNotice notice;
	notice.Report("retained worker detail", [](auto) { return false; },
		[&](auto text) { return JobFailureNotice::Persist(Directory.c_str(), text); },
		[&](auto text) { delivered = text; });
	EXPECT_EQ(delivered, "retained worker detail");
	EXPECT_FALSE(ring.Take());
}
TEST_F(DiagnosticFiles, RunLogRetainsConcurrentBurstDespiteConsoleSaturation)
{
	console::LineRing ring;
	{
		console::StartupLog log((Directory / "defaults.json").wstring());
		console::ConsoleCapture capture(ring, {}, [&](std::string_view line) { log.Append(line); });
		capture.CompleteStartup();
		std::vector<std::thread> producers;
		for (unsigned thread = 0; thread < 4; ++thread) producers.emplace_back([thread] {
			for (unsigned line = 0; line < 100; ++line)
			{
				const auto text = std::to_string(thread) + ":" + std::to_string(line) + "\n";
				(thread % 2 ? std::cerr : std::cout).write(text.data(), text.size());
			}
			std::clog << "worker tail " << thread; // Flushed at worker exit.
		});
		for (auto& producer : producers) producer.join();
	}
	std::set<std::string> lines;
	std::istringstream file(ReadLog());
	std::string line;
	while (std::getline(file, line)) { if (line.ends_with('\r')) line.pop_back(); EXPECT_TRUE(lines.insert(line).second); }
	EXPECT_EQ(lines.size(), 404u);
	for (unsigned thread = 0; thread < 4; ++thread)
	{
		EXPECT_TRUE(lines.contains("worker tail " + std::to_string(thread)));
		for (unsigned entry = 0; entry < 100; ++entry) EXPECT_TRUE(lines.contains(std::to_string(thread) + ":" + std::to_string(entry)));
	}
	EXPECT_GT(ring.ConsumeDropped(), 0u);
}
TEST_F(DiagnosticFiles, CrtAndWin32StreamsJoinContinuousSinkAndRestoreDescriptors)
{
	console::LineRing ring;
	{
		console::StartupLog log((Directory / "defaults.json").wstring());
		console::ConsoleCapture capture(ring, {}, [&](std::string_view line) { log.Append(line); });
		{
			console::CrtCapture crt(capture);
			ASSERT_TRUE(crt.Available());
			std::fputs("early CRT stdout\n", stdout);
			std::fflush(stdout);
			capture.CompleteStartup();
			std::thread worker([] {
				std::fputs("runtime CRT stderr\n", stderr);
				std::fflush(stderr);
			}); worker.join();
			std::cout << "C++ runtime\n";
			DWORD written = 0;
			ASSERT_TRUE(WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "native stdout\n", 14, &written, nullptr));
			std::fputs("CRT shutdown tail", stdout); // Stop flushes the FILE and capture tail.
			crt.Stop();
			EXPECT_FALSE(crt.Available());
			crt.Stop();
		}
		std::clog << "after CRT stop\n";
	}
	const auto text = ReadLog();
	for (const auto* expected : {"early CRT stdout", "runtime CRT stderr", "C++ runtime", "native stdout", "CRT shutdown tail", "after CRT stop"})
	{
		const auto first = text.find(expected);
		ASSERT_NE(first, std::string::npos) << expected;
		EXPECT_EQ(text.find(expected, first + 1), std::string::npos) << expected;
	}
	EXPECT_EQ(_get_osfhandle(_fileno(stdout)) == -1, false);
	EXPECT_EQ(_get_osfhandle(_fileno(stderr)) == -1, false);
}
TEST_F(DiagnosticFiles, CrtShutdownDoesNotWaitForDuplicatedNativeWriterToClose)
{
	console::LineRing ring;
	console::ConsoleCapture capture(ring);
	console::CrtCapture crt(capture);
	ASSERT_TRUE(crt.Available());
	HANDLE duplicate = nullptr;
	ASSERT_TRUE(DuplicateHandle(GetCurrentProcess(), GetStdHandle(STD_OUTPUT_HANDLE), GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS));
	std::fputs("before stop\n", stdout);
	std::fflush(stdout);
	crt.Stop(); // The duplicate remains alive; EOF-based readers would deadlock.
	CloseHandle(duplicate);
	EXPECT_EQ(ring.Take(), "before stop");
	EXPECT_FALSE(ring.Take());
}
TEST_F(DiagnosticFiles, MissingVst3RuntimeFailureRetainsContextInRunLog)
{
	console::LineRing ring;
	{
		console::StartupLog log((Directory / L"defaults.json").wstring());
		console::ConsoleCapture capture(ring, {}, [&](std::string_view line) { log.Append(line); });
		capture.CompleteStartup();
		vst::Vst3Plugin plugin;
		EXPECT_FALSE(plugin.PreInit((Directory / L"missing-runtime.vst3").wstring()));
	}
	const auto retained = ReadLog();
	EXPECT_NE(retained.find("[ERROR][VST3]"), std::string::npos);
	EXPECT_NE(retained.find("missing-runtime.vst3"), std::string::npos);
	EXPECT_NE(retained.find("windows-error="), std::string::npos);
}
