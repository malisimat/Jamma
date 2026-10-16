#include "../../../../console/CommandMailbox.h"
#include "../../../../console/CommandParsing.h"
#include "../../../../console/GlobalKeyGate.h"
#include "../../../../console/LineRing.h"
#include "../../../../Jamma/src/ConsoleCapture.h"
#include <gtest/gtest.h>
#include <array>
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
