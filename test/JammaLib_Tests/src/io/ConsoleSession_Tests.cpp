#include "../../../../console/SessionGate.h"
#include "../../../../Jamma/src/ConsoleLaunch.h"
#include "../../../../console/CommandMailbox.h"
#include "../../../../console/OutboundMailbox.h"
#include "../../../../console/CommandParsing.h"
#include "../../../../console/LineRing.h"
#include "../../../../Jamma/src/ConsoleCapture.h"
#include "../../../../Jamma/src/ConsoleStatus.h"
#include <gtest/gtest.h>
#include <array>
#include <chrono>
#include <thread>
#include <vector>

TEST(ConsoleSession, CaptureOnlySinkKeepsOptedInLinesAndBoundsOverflow)
{
	console::LineRing ring;
	{
		console::ConsoleCapture capture(ring);
		console::ConsoleCapture::EnableForCurrentThread(true);
		std::cout << "visible\n";
		console::ConsoleCapture::EnableForCurrentThread(false);
		std::cout << "unopted\n";
		console::ConsoleCapture::EnableForCurrentThread(true);
		std::cout << std::string(console::LineRing::MaxLineBytes + 1, 'x') << '\n';
		console::ConsoleCapture::EnableForCurrentThread(false);
		capture.Stop();
	}
	const auto captured = ring.Take();
	ASSERT_TRUE(captured);
	EXPECT_EQ(*captured, "visible");
	EXPECT_FALSE(ring.Take());
	EXPECT_EQ(ring.ConsumeDropped(), 1u);
}

TEST(ConsoleSession, StatusCadenceUsesMonotonicOneSecondBoundary)
{
	using Clock = std::chrono::steady_clock;
	const auto last = Clock::time_point{};
	EXPECT_FALSE(console::StatusUpdateDue(last, last));
	EXPECT_FALSE(console::StatusUpdateDue(last, last + std::chrono::milliseconds(999)));
	EXPECT_TRUE(console::StatusUpdateDue(last, last + std::chrono::seconds(1)));
	EXPECT_TRUE(console::StatusUpdateDue(last, last + std::chrono::seconds(2)));
}

TEST(ConsoleSession, AuthenticatesOneClientAndIncreasingRequests)
{
	console::SessionGate gate;
	gate.Begin("one-time-token");
	EXPECT_FALSE(gate.AcceptRequest(1));
	EXPECT_FALSE(gate.AcceptHello("wrong", true));
	EXPECT_FALSE(gate.AcceptHello("one-time-token", false));
	ASSERT_TRUE(gate.AcceptHello("one-time-token", true));
	EXPECT_FALSE(gate.AcceptHello("one-time-token", true));
	EXPECT_TRUE(gate.AcceptRequest(1));
	EXPECT_FALSE(gate.AcceptRequest(1));
	EXPECT_FALSE(gate.AcceptRequest(0));
	EXPECT_TRUE(gate.AcceptRequest(3));
	EXPECT_FALSE(gate.AcceptRequest(2));
}

TEST(ConsoleSession, DisconnectAndFreshGenerationRejectStaleClient)
{
	console::SessionGate gate;
	gate.Begin("old");
	ASSERT_TRUE(gate.AcceptHello("old", true));
	ASSERT_TRUE(gate.AcceptRequest(7));
	gate.Disconnect();
	EXPECT_FALSE(gate.AcceptRequest(8));
	EXPECT_FALSE(gate.AcceptHello("old", true));
	gate.Begin("fresh");
	EXPECT_FALSE(gate.AcceptHello("old", true));
	ASSERT_TRUE(gate.AcceptHello("fresh", true));
	EXPECT_TRUE(gate.AcceptRequest(1));
}

TEST(ConsoleSession, ShutdownRejectsPendingAndFutureRequests)
{
	console::SessionGate gate;
	gate.Begin("token");
	ASSERT_TRUE(gate.AcceptHello("token", true));
	ASSERT_TRUE(gate.AcceptRequest(1));
	gate.StopAccepting();
	EXPECT_FALSE(gate.AcceptRequest(2));
	gate.Disconnect();
	EXPECT_FALSE(gate.AcceptHello("token", true));
}

TEST(ConsoleSession, BufferedRequestAfterStopCannotPassGate)
{
	console::SessionGate gate;
	gate.Begin("token");
	ASSERT_TRUE(gate.AcceptHello("token", true));
	EXPECT_FALSE(gate.AcceptRequest(1, true));
	EXPECT_FALSE(gate.AcceptRequest(2));
}

TEST(ConsoleSession, ShutdownOrdersPipeAndChildCleanup)
{
	console::SessionGate gate;
	gate.Begin("token");
	ASSERT_TRUE(gate.AcceptHello("token", true));
	std::vector<std::string> operations;
	console::FinishSession(gate, true, true,
		[&] {
			EXPECT_FALSE(gate.AcceptRequest(1));
			operations.push_back("shutdown frame");
		},
		[&] {
			operations.push_back("wait child");
			return false;
		},
		[&] { operations.push_back("terminate child"); });
	EXPECT_EQ(operations,
		(std::vector<std::string>{ "shutdown frame", "wait child", "terminate child" }));
	EXPECT_FALSE(gate.Authenticated());
	EXPECT_FALSE(gate.AcceptHello("token", true));
	operations.clear();
	console::FinishSession(gate, false, false,
		[&] { operations.push_back("shutdown frame"); },
		[&] { operations.push_back("wait child"); return true; },
		[&] { operations.push_back("terminate child"); });
	EXPECT_TRUE(operations.empty());
}

TEST(ConsoleSession, MailboxOrdersSubmissionsAndClosesPendingWork)
{
	console::CommandMailbox mailbox;
	auto first = mailbox.Submit("/help");
	auto second = mailbox.Submit("chat");
	ASSERT_TRUE(first);
	ASSERT_TRUE(second);
	auto taken = mailbox.Take();
	ASSERT_TRUE(taken);
	EXPECT_EQ(taken->Text, "/help");
	taken->Completion->set_value("help shown");
	EXPECT_EQ(first->get(), "help shown");
	mailbox.Close();
	EXPECT_EQ(second->get(), "Console shutting down");
	EXPECT_FALSE(mailbox.Submit("later"));
	EXPECT_FALSE(mailbox.Take());
}

TEST(ConsoleSession, MailboxBoundsTextAndPendingCount)
{
	console::CommandMailbox mailbox;
	EXPECT_FALSE(mailbox.Submit(std::string(console::CommandMailbox::MaxTextBytes + 1, 'x')));
	for (std::size_t index = 0; index < console::CommandMailbox::Capacity; ++index)
		ASSERT_TRUE(mailbox.Submit("chat"));
	EXPECT_FALSE(mailbox.Submit("overflow"));
	mailbox.Close();
}

TEST(ConsoleSession, InFlightOwnerCommandFinishesAfterIntakeCloses)
{
	console::CommandMailbox mailbox;
	auto pending = mailbox.Submit("/connect 2");
	ASSERT_TRUE(pending);
	auto inFlight = mailbox.Take();
	ASSERT_TRUE(inFlight);
	mailbox.Close();
	EXPECT_EQ(pending->wait_for(std::chrono::milliseconds(0)), std::future_status::timeout);
	EXPECT_FALSE(mailbox.Submit("new"));
	inFlight->Completion->set_value("Connection requested");
	EXPECT_EQ(pending->get(), "Connection requested");
}

TEST(ConsoleSession, OutboundOrdersEventsReportsLossAndCoalescesStatus)
{
	console::OutboundMailbox mailbox;
	mailbox.SetStatus("connecting");
	mailbox.SetStatus("connected");
	for (std::size_t index = 0; index < console::OutboundMailbox::Capacity; ++index)
		ASSERT_TRUE(mailbox.Publish({ console::MessageType::Event, 0,
			std::to_string(index) }));
	EXPECT_FALSE(mailbox.Publish({ console::MessageType::Event, 0, "dropped" }));
	std::atomic<bool> stopping{ false };
	auto status = mailbox.Take(stopping);
	ASSERT_TRUE(status);
	EXPECT_EQ(status->Type, console::MessageType::StatusSnapshot);
	EXPECT_EQ(status->Text, "connected");
	for (std::size_t index = 0; index < console::OutboundMailbox::Capacity; ++index)
	{
		auto event = mailbox.Take(stopping);
		ASSERT_TRUE(event);
		EXPECT_EQ(event->Text, std::to_string(index));
	}
	auto notice = mailbox.Take(stopping);
	ASSERT_TRUE(notice);
	EXPECT_NE(notice->Text.find("Lost 1 events"), std::string::npos);
	ASSERT_TRUE(mailbox.Publish({ console::MessageType::Event, 0, "after" }));
	const auto last = mailbox.Take(stopping);
	ASSERT_TRUE(last);
	EXPECT_EQ(last->Text, "after");
	mailbox.Close();
	EXPECT_FALSE(mailbox.Publish({ console::MessageType::Event, 0, "late" }));
}

TEST(ConsoleSession, OutboundPreservesCommandResultUnderOverflow)
{
	console::OutboundMailbox mailbox;
	std::atomic<bool> stopping{ false };
	ASSERT_TRUE(mailbox.PublishResult({ console::MessageType::CommandResult, 7, "first" },
		[&] { return stopping.load(); }));
	for (std::size_t index = 1; index < console::OutboundMailbox::Capacity; ++index)
		ASSERT_TRUE(mailbox.Publish({ console::MessageType::Event, 0, "old" }));
	ASSERT_TRUE(mailbox.PublishResult({ console::MessageType::CommandResult, 8, "second" },
		[&] { return stopping.load(); }));
	std::vector<std::uint64_t> resultIds;
	for (std::size_t index = 0; index < console::OutboundMailbox::Capacity + 1; ++index)
	{
		auto message = mailbox.Take(stopping);
		ASSERT_TRUE(message);
		if (message->Type == console::MessageType::CommandResult)
			resultIds.push_back(message->RequestId);
	}
	EXPECT_EQ(resultIds, (std::vector<std::uint64_t>{ 7, 8 }));
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

TEST(ConsoleSession, CaptureLossSurvivesFullOutboundQueueWithoutNewEvents)
{
	console::OutboundMailbox mailbox;
	for (std::size_t index = 0; index < console::OutboundMailbox::Capacity; ++index)
		ASSERT_TRUE(mailbox.Publish({ console::MessageType::Event, 0, "queued" }));
	ASSERT_TRUE(mailbox.ReportLoss(3));
	std::atomic<bool> stopping{ false };
	for (std::size_t index = 0; index < console::OutboundMailbox::Capacity; ++index)
		ASSERT_TRUE(mailbox.Take(stopping));
	auto notice = mailbox.Take(stopping);
	ASSERT_TRUE(notice);
	EXPECT_NE(notice->Text.find("Lost 3 events"), std::string::npos);
}

TEST(ConsoleCommand, PreservesHelpConnectDisconnectAndChatMeanings)
{
	for (const auto* text : { "/", "/?", "/help", "/HELP" })
		EXPECT_EQ(console::ParseCommand(text).Kind, console::CommandKind::Help);
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

TEST(ConsoleCommand, QueuedSubmissionUsesReplacementOwner)
{
	console::CommandMailbox mailbox;
	struct FakeScene { std::vector<std::string> Chats; } oldScene, replacement;
	auto pending = mailbox.Submit("hello");
	ASSERT_TRUE(pending);
	FakeScene* current = &oldScene;
	current = &replacement; // The app replaces Scene before taking the command.
	auto submission = mailbox.Take();
	ASSERT_TRUE(submission);
	if (console::ParseCommand(submission->Text).Kind == console::CommandKind::Chat)
		current->Chats.push_back(submission->Text);
	submission->Completion->set_value("Chat sent");
	EXPECT_TRUE(oldScene.Chats.empty());
	EXPECT_EQ(replacement.Chats, (std::vector<std::string>{ "hello" }));
	EXPECT_EQ(pending->get(), "Chat sent");
}

TEST(ConsoleLaunch, QuotesSpacesQuotesAndTrailingBackslashes)
{
	EXPECT_EQ(console::QuoteWindowsArgument(L"C:\\Program Files\\JammaConsole.exe"),
		L"\"C:\\Program Files\\JammaConsole.exe\"");
	EXPECT_EQ(console::QuoteWindowsArgument(L"a\"b"), L"\"a\\\"b\"");
	EXPECT_EQ(console::QuoteWindowsArgument(L"a\\"), L"\"a\\\\\"");
}

TEST(ConsoleLaunch, FallsBackOnMissingLaunchOrHandshake)
{
	std::vector<console::LaunchHost> attempts;
	int notices = 0;
	auto tryHost = [&](console::LaunchHost host) {
		attempts.push_back(host);
		return host == console::LaunchHost::ConsoleHost;
	};
	EXPECT_EQ(console::RunLaunchPlan(true, tryHost, [] { return false; },
		[&] { ++notices; }), console::LaunchOutcome::ConsoleHost);
	ASSERT_EQ(attempts.size(), 2u);
	EXPECT_EQ(attempts[0], console::LaunchHost::WindowsTerminal);
	EXPECT_EQ(attempts[1], console::LaunchHost::ConsoleHost);
	EXPECT_EQ(notices, 1);
	attempts.clear();
	EXPECT_EQ(console::RunLaunchPlan(false, tryHost, [] { return false; },
		[&] { ++notices; }), console::LaunchOutcome::ConsoleHost);
	ASSERT_EQ(attempts.size(), 1u);
	EXPECT_EQ(attempts[0], console::LaunchHost::ConsoleHost);
}

TEST(ConsoleLaunch, NeverCreatesFallbackAfterStopOrAcceptedTerminal)
{
	int attempts = 0;
	int notices = 0;
	EXPECT_EQ(console::RunLaunchPlan(true,
		[&](console::LaunchHost) { ++attempts; return false; },
		[] { return true; }, [&] { ++notices; }), console::LaunchOutcome::Stopped);
	EXPECT_EQ(attempts, 1);
	EXPECT_EQ(notices, 0);
	EXPECT_EQ(console::RunLaunchPlan(true,
		[&](console::LaunchHost) { ++attempts; return true; },
		[] { return false; }, [&] { ++notices; }), console::LaunchOutcome::WindowsTerminal);
	EXPECT_EQ(attempts, 2);
	EXPECT_EQ(notices, 0);
}

TEST(ConsoleLaunch, ReopenKeepsLiveGenerationAndNeverStartsAfterFailedStop)
{
	int stopped = 0;
	int started = 0;
	const auto stop = [&] { ++stopped; return true; };
	const auto start = [&] { ++started; return true; };
	EXPECT_TRUE(console::RunReopenPlan(true, stop, start));
	EXPECT_TRUE(console::RunReopenPlan(true, stop, start));
	EXPECT_EQ(stopped, 0);
	EXPECT_EQ(started, 0);
	EXPECT_TRUE(console::RunReopenPlan(false, stop, start));
	EXPECT_EQ(stopped, 1);
	EXPECT_EQ(started, 1);
	EXPECT_FALSE(console::RunReopenPlan(false,
		[&] { ++stopped; return false; }, start));
	EXPECT_EQ(stopped, 2);
	EXPECT_EQ(started, 1);
}

TEST(ConsoleLaunch, BuildsTerminalAndConsoleArgumentsFromAbsoluteSibling)
{
	const auto companion = console::SiblingCompanionPath(L"C:\\Program Files\\Jamma\\Jamma.exe");
	EXPECT_EQ(companion, L"C:\\Program Files\\Jamma\\JammaConsole.exe");
	const auto terminal = console::TerminalArguments(L"Jamma-123", companion,
		L"\\\\.\\pipe\\JammaConsole-123", L"abc");
	EXPECT_EQ(terminal,
		L"-w \"Jamma-123\" --size 100,30 \"C:\\Program Files\\Jamma\\JammaConsole.exe\" --pipe \"\\\\.\\pipe\\JammaConsole-123\" --token \"abc\"");
	EXPECT_EQ(console::ConsoleArguments(L"pipe", L"abc"),
		L"--pipe \"pipe\" --token \"abc\"");
}
