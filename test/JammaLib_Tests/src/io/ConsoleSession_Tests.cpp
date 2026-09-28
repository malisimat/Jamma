#include "../../../../console/SessionGate.h"
#include "../../../../Jamma/src/ConsoleLaunch.h"
#include <gtest/gtest.h>
#include <vector>

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
