#include "../../../../console/CommandMailbox.h"
#include "../../../../console/CommandParsing.h"
#include "../../../../console/Help.h"
#include "../../../../console/GlobalKeyGate.h"
#include "../../../../console/LineRing.h"
#include "../../../../Jamma/src/ConsoleCapture.h"
#include <gtest/gtest.h>
#include <array>
#include <thread>
#include <sstream>
#include <vector>
TEST(ConsoleSession, CaptureDefaultsToWorkersAndFlushesThreadExitAndEachStream)
{
	console::LineRing ring;
	{
		console::ConsoleCapture capture(ring);
		console::ConsoleCapture::EnableForCurrentThread(true);
		std::cout << "out partial";
		std::cerr << "error line\n";
		std::clog << "clog line\n";
		std::cout << std::flush;
		std::thread worker([] { std::cout << "worker tail"; });
		worker.join();
		std::cout << "shutdown tail";
	}
	std::vector<std::string> captured;
	while (auto line = ring.Take()) captured.push_back(*line);
	EXPECT_NE(std::find(captured.begin(), captured.end(), "error line"), captured.end());
	EXPECT_NE(std::find(captured.begin(), captured.end(), "clog line"), captured.end());
	EXPECT_NE(std::find(captured.begin(), captured.end(), "out partial"), captured.end());
	EXPECT_NE(std::find(captured.begin(), captured.end(), "worker tail"), captured.end());
	EXPECT_NE(std::find(captured.begin(), captured.end(), "shutdown tail"), captured.end());
	EXPECT_EQ(captured.size(), 5u);
}

TEST(ConsoleSession, CaptureSplitsLargeUtf8LinesWithoutTruncation)
{
	console::LineRing ring;
	std::string text(console::LineRing::MaxLineBytes - 1, 'x');
	text += "\xF0\x9F\x8E\xB5";
	text += std::string(5000, 'y');
	{
		console::ConsoleCapture capture(ring);
		std::cout << text << '\n';
	}
	std::string result;
	while (auto line = ring.Take())
	{
		ASSERT_LE(line->size(), console::LineRing::MaxLineBytes);
		ASSERT_FALSE(line->empty());
		EXPECT_NE(static_cast<unsigned char>(line->front()) & 0xc0, 0x80);
		result += *line;
	}
	EXPECT_EQ(result, text);
	EXPECT_EQ(ring.ConsumeDropped(), 0u);
}

TEST(ConsoleSession, CaptureStartupTeeSurvivesSaturationAndStopsBeforeRuntime)
{
	console::LineRing ring;
	std::vector<std::string> startup;
	{
		console::ConsoleCapture capture(ring, [&](std::string_view line) { startup.emplace_back(line); });
		std::thread worker([] { for (int index = 0; index < 300; ++index) std::cout << "startup worker\n"; });
		worker.join();
		std::cout << "startup partial";
		capture.CompleteStartup();
		while (ring.Take()) {}
		std::cout << "runtime\n";
		capture.CompleteStartup(); // Repeated completion cannot flush runtime fragments.
	}
	EXPECT_EQ(startup.size(), 301u);
	EXPECT_EQ(startup.back(), "startup partial");
	EXPECT_GT(ring.ConsumeDropped(), 0u);
	EXPECT_EQ(ring.Take(), "runtime");
	EXPECT_FALSE(ring.Take());
}

TEST(ConsoleSession, CaptureSupportsConcurrentProducersAndReopenWithLivingWorker)
{
	console::LineRing first, second;
	std::atomic<bool> wrote{false}, exit{false};
	std::thread worker;
	{
		console::ConsoleCapture capture(first);
		worker = std::thread([&] {
			std::cout << "first partial";
			wrote.store(true);
			while (!exit.load()) std::this_thread::yield();
		});
		while (!wrote.load()) std::this_thread::yield();
	}
	EXPECT_EQ(first.Take(), "first partial");
	{
		console::ConsoleCapture capture(second);
		std::vector<std::thread> producers;
		for (int i = 0; i < 4; ++i) producers.emplace_back([i] {
			for (int j = 0; j < 20; ++j) { const auto line = std::to_string(i) + ":" + std::to_string(j) + "\n"; std::cout.write(line.data(), line.size()); }
		});
		for (auto& producer : producers) producer.join();
		exit.store(true); worker.join();
	}
	std::size_t count = 0;
	while (second.Take()) ++count;
	EXPECT_EQ(count + second.ConsumeDropped(), 80u);
}

TEST(ConsoleSession, RealtimeCaptureSuppressesBeforeThreadRegistration)
{
	console::LineRing ring;
	{
		console::ConsoleCapture capture(ring);
		std::thread callback([] {
			console::DiagnosticThread::RealtimeScope scope;
			std::cout << "unexpected callback output\n";
		});
		callback.join();
		std::cout << "owner output\n";
	}
	EXPECT_GT(console::DiagnosticThread::ConsumeSuppressed(), 0u);
	EXPECT_EQ(ring.Take(), "owner output");
	EXPECT_FALSE(ring.Take());
}

TEST(ConsoleSession, CaptureRingBoundsLinesAndReportsOverflow)
{
	console::LineRing ring;
	for (std::size_t index = 0; index < console::LineRing::Capacity; ++index)
		ASSERT_TRUE(ring.Publish("line " + std::to_string(index)));
	EXPECT_FALSE(ring.Publish("overflow"));
	for (std::size_t index = 0; index < console::LineRing::Capacity; ++index)
	{
		auto line = ring.Take();
		ASSERT_TRUE(line);
		EXPECT_EQ(*line, "line " + std::to_string(index));
	}
	EXPECT_FALSE(ring.Take());
	EXPECT_EQ(ring.ConsumeDropped(), 1u);
	EXPECT_EQ(ring.ConsumeDropped(), 0u);
	EXPECT_TRUE(ring.Publish("after"));
	EXPECT_EQ(ring.Take(), "after");
}

TEST(ConsoleSession, CaptureRingOrdersConcurrentProducerLines)
{
	console::LineRing ring;
	std::vector<std::thread> producers;
	for (int producer = 0; producer < 4; ++producer)
		producers.emplace_back([&ring, producer] {
			for (int index = 0; index < 50; ++index)
				ring.Publish(std::to_string(producer) + ":" + std::to_string(index));
		});
	for (auto& producer : producers) producer.join();
	std::array<int, 4> last{ -1, -1, -1, -1 };
	std::size_t received = 0;
	while (auto line = ring.Take())
	{
		const auto colon = line->find(':');
		ASSERT_NE(colon, std::string::npos);
		const int producer = std::stoi(line->substr(0, colon));
		const int index = std::stoi(line->substr(colon + 1));
		ASSERT_GE(producer, 0);
		ASSERT_LT(producer, 4);
		EXPECT_GT(index, last[producer]);
		last[producer] = index;
		++received;
	}
	EXPECT_EQ(received + ring.ConsumeDropped(), 200u);
}

TEST(ConsoleCommand, PreservesHelpConnectDisconnectAndChatMeanings)
{
	for (const auto* text : { "/", "/?", "/help", "/HELP" })
		EXPECT_EQ(console::ParseCommand(text).Kind, console::CommandKind::Help);
	for (const auto* text : { "/n", "/ninjam", "/NINJAM" })
		EXPECT_EQ(console::ParseCommand(text).Kind, console::CommandKind::Ninjam);
	for (const auto* text : { "/d", "/q", "/quit", "/exit", "/disconnect" })
		EXPECT_EQ(console::ParseCommand(text).Kind, console::CommandKind::Disconnect);
	const auto connect = console::ParseCommand("/CONNECT   2more");
	EXPECT_EQ(connect.Kind, console::CommandKind::Connect);
	EXPECT_EQ(connect.ServerNumber, 2); // Existing stoi accepts a numeric prefix.
	EXPECT_EQ(console::ParseCommand("/c bad").ServerNumber, 0);
	EXPECT_EQ(console::ParseCommand("/c").Arguments, "");
	EXPECT_EQ(console::ParseCommand("/mute").Kind, console::CommandKind::Unknown);
	EXPECT_EQ(console::ParseCommand("hello").Kind, console::CommandKind::Chat);
	EXPECT_EQ(console::ParseCommand("").Kind, console::CommandKind::Chat);
}

TEST(ConsoleCommand, GeneralHelpAliasesWriteTheSameCompleteOfflineReference)
{
	for (const auto* text : { "/", "/help", "/?", "/HELP" })
	{
		std::ostringstream output;
		// No Scene, NINJAM directory or device exists on this path.
		ASSERT_TRUE(console::TryWriteGeneralHelp(console::ParseCommand(text), output));
		EXPECT_EQ(output.str(), console::GeneralHelpText);
	}
	for (const auto* command : { "/n", "/ninjam", "/c 1", "/disconnect", "hello", "/unknown" })
	{
		std::ostringstream output;
		EXPECT_FALSE(console::TryWriteGeneralHelp(console::ParseCommand(command), output));
		EXPECT_TRUE(output.str().empty());
	}
	const auto help = console::GeneralHelpText;
	for (const auto* binding : { "/ninjam", "/connect", "/disconnect", "/quit", "/exit",
		"Ctrl+`", "F1-F9", "Shift+F1-F9", "Alt+F1-F9", "Middle-click", "Alt+middle-click",
		"Double-left-click", "unity (0 dB)", "16 channel override toggles", "unchanged/pass-through",
		"PageUp", "PageDown", "Hold Insert", "Ctrl+Shift+[", "Shift+right-drag", "CONFIGURABLE" })
		EXPECT_NE(help.find(binding), std::string_view::npos) << binding;
}

TEST(ConsoleCommand, GeneralHelpFitsCaptureWithoutDroppingLines)
{
	console::LineRing ring;
	{
		console::ConsoleCapture capture(ring);
		ASSERT_TRUE(console::TryWriteGeneralHelp(console::ParseCommand("/help"), std::cout));
		std::cout << std::flush;
	}
	std::string text;
	while (auto line = ring.Take()) text += *line + '\n';
	EXPECT_EQ(text, console::GeneralHelpText);
	EXPECT_EQ(ring.ConsumeDropped(), 0u);
}
TEST(ConsoleSession, OwnerQueuePreservesOrderAndClosesPendingWork)
{
	console::CommandMailbox mailbox;
	ASSERT_TRUE(mailbox.Submit("/help"));
	ASSERT_TRUE(mailbox.Submit("chat"));
	auto first = mailbox.Take();
	ASSERT_TRUE(first);
	EXPECT_EQ(first->Text, "/help");
	auto second = mailbox.Take();
	ASSERT_TRUE(second);
	EXPECT_EQ(second->Text, "chat");
	EXPECT_FALSE(mailbox.Take());
	ASSERT_TRUE(mailbox.Submit("pending"));
	mailbox.Close();
	EXPECT_FALSE(mailbox.Submit("late"));
	EXPECT_FALSE(mailbox.Take());
}
TEST(ConsoleSession, OwnerQueueBoundsTextAndCount)
{
	console::CommandMailbox mailbox;
	EXPECT_FALSE(mailbox.Submit(std::string(console::CommandMailbox::MaxTextBytes + 1, 'x')));
	for (std::size_t index = 0; index < console::CommandMailbox::Capacity; ++index)
		ASSERT_TRUE(mailbox.Submit("chat"));
	EXPECT_FALSE(mailbox.Submit("overflow"));
}
TEST(ConsoleCommand, QueuedSubmissionUsesReplacementOwner)
{
	console::CommandMailbox mailbox;
	struct FakeScene { std::vector<std::string> Chats; } oldScene, replacement;
	ASSERT_TRUE(mailbox.Submit("hello"));
	FakeScene* current = &oldScene;
	current = &replacement;
	auto submission = mailbox.Take();
	ASSERT_TRUE(submission);
	if (console::ParseCommand(submission->Text).Kind == console::CommandKind::Chat)
		current->Chats.push_back(submission->Text);
	EXPECT_TRUE(oldScene.Chats.empty());
	EXPECT_EQ(replacement.Chats, (std::vector<std::string>{ "hello" }));
}
TEST(ConsoleCommand, ConsoleFocusSwallowsPressesButReleasesForwardedHolds)
{
	for (const unsigned key : { 0x2d, 0x21, 0x22 })
	{
		console::GlobalKeyGate gate;
		EXPECT_TRUE(gate.Forward(key, true, false));
		EXPECT_TRUE(gate.Forward(key, false, true));
		EXPECT_FALSE(gate.Forward(key, true, true));
		EXPECT_FALSE(gate.Forward(key, false, true));
		EXPECT_FALSE(gate.Forward(key, true, true));
		EXPECT_FALSE(gate.Forward(key, false, false));
		EXPECT_TRUE(gate.Forward(key, true, false));
		EXPECT_TRUE(gate.Forward(key, false, false));
	}
}

TEST(ConsoleCommand, NativeMainPressGetsReleaseWhenConsoleActivatesBeforeGlobalPump)
{
	for (const unsigned key : { 0x2d, 0x21, 0x22 })
	{
		console::GlobalKeyGate gate;
		EXPECT_TRUE(gate.Observe(key, true, false, true, false));
		EXPECT_FALSE(gate.Forward(key, true, true));
		EXPECT_TRUE(gate.Observe(key, false, true, false, false));
		EXPECT_TRUE(gate.Forward(key, false, true));
		EXPECT_TRUE(gate.Forward(key, true, false));
		EXPECT_TRUE(gate.Forward(key, false, false));
	}
}
TEST(ConsoleCommand, ConsolePressCannotLeakNativeOrGlobalActionsAfterFocusTransfer)
{
	for (const unsigned key : { 0x2d, 0x21, 0x22 })
	{
		console::GlobalKeyGate gate;
		EXPECT_TRUE(gate.Observe(key, true, true, false, false));
		EXPECT_FALSE(gate.Forward(key, true, false));
		EXPECT_FALSE(gate.Observe(key, true, false, true, true));
		EXPECT_FALSE(gate.Observe(key, false, false, true, false));
		EXPECT_FALSE(gate.Forward(key, false, false));
		EXPECT_TRUE(gate.Observe(key, true, false, true, false));
		EXPECT_TRUE(gate.Forward(key, true, false));
		EXPECT_TRUE(gate.Observe(key, false, false, true, false));
		EXPECT_FALSE(gate.Forward(key, false, false));
	}
}
TEST(ConsoleCommand, ConsoleReleaseBeforeGlobalPumpClearsSuppressionForNextPress)
{
	console::GlobalKeyGate gate;
	EXPECT_TRUE(gate.Observe(0x2d, true, true, false, false));
	EXPECT_TRUE(gate.Observe(0x2d, false, true, false, false));
	// Fast down/up may coalesce to no global transition. The next physical
	// press can arrive in another app and must keep the existing global behavior.
	EXPECT_TRUE(gate.Forward(0x2d, true, false));
	EXPECT_TRUE(gate.Forward(0x2d, false, false));
}

TEST(ConsoleCommand, CompletedNativeHoldCannotCreateLaterConsoleReleaseAction)
{
	console::GlobalKeyGate gate;
	EXPECT_TRUE(gate.Observe(0x2d, true, false, true, false));
	EXPECT_TRUE(gate.Observe(0x2d, false, false, true, false));
	// Native down/up already dispatched before any global pump transition.
	EXPECT_TRUE(gate.Observe(0x2d, true, true, false, false));
	EXPECT_FALSE(gate.Forward(0x2d, true, true));
	EXPECT_TRUE(gate.Observe(0x2d, false, true, false, false));
	EXPECT_FALSE(gate.Forward(0x2d, false, true));
}

TEST(ConsoleCommand, NativeMainHoldStillReleasesAfterConsoleAndMainFocusRoundTrip)
{
	for (const unsigned key : { 0x2d, 0x21, 0x22 })
	{
		console::GlobalKeyGate gate;
		EXPECT_TRUE(gate.Observe(key, true, false, true, false));
		EXPECT_FALSE(gate.Forward(key, true, true));
		// Main already received the initial press. Console activation suppressed
		// its duplicate global down, but a later native main release is required.
		EXPECT_TRUE(gate.Observe(key, false, false, true, false));
		EXPECT_FALSE(gate.Forward(key, false, false));
	}
}
