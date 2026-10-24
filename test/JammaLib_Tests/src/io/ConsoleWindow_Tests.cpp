#include "../../../../Jamma/src/ConsoleWindow.h"
#include "../../../../console/CommandMailbox.h"
#include "../../../../console/Help.h"
#include <Richedit.h>
#include <gtest/gtest.h>
#include <array>
#include <vector>
#include <objbase.h>
#include <sstream>

namespace console
{
	struct ConsoleWindowTestAccess
	{
		static HWND Window(const ConsoleWindow& view) { return view._window; }
		static HWND Transcript(const ConsoleWindow& view) { return view._transcript; }
		static HWND Input(const ConsoleWindow& view) { return view._input; }
		static HWND Status(const ConsoleWindow& view) { return view._status; }
		static LONG Units(const ConsoleWindow& view) { return view._units; }
		static std::size_t Entries(const ConsoleWindow& view) { return view._entries.size(); }
		static void Paste(ConsoleWindow& view, std::wstring_view text) { view.ApplyPaste(text); }
		static bool Command(std::wstring_view wide, std::string& text) { return ConsoleWindow::CommandText(wide, text); }
	};
}

class NativeConsoleWindow : public testing::Test
{
protected:
	console::CommandMailbox Commands;
	console::ConsoleWindow View{Commands};
	using Access = console::ConsoleWindowTestAccess;
	bool ComInitialized = false;
	void SetUp() override
	{
		ComInitialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
		ASSERT_TRUE(View.CreateHidden(GetModuleHandleW(nullptr)));
	}
	void TearDown() override
	{
		View.Destroy();
		if (ComInitialized) CoUninitialize();
	}
	void Append(std::string line)
	{
		const std::array<std::string, 1> lines{std::move(line)};
		View.Append(lines);
	}
	void Fill(std::size_t count, const std::string& line = "line with text")
	{
		for (std::size_t index = 0; index < count;)
		{
			std::vector<std::string> batch;
			for (std::size_t item = 0; item < console::ConsoleWindow::MaxAppendEntries && index < count; ++item, ++index)
				batch.push_back(line);
			View.Append(batch);
		}
	}
	static std::wstring Text(HWND control)
	{
		const int length = GetWindowTextLengthW(control);
		std::wstring text(length + 1, L'\0');
		text.resize(GetWindowTextW(control, text.data(), static_cast<int>(text.size())));
		return text;
	}
	static void Select(HWND control, LONG start, LONG end)
	{
		CHARRANGE range{start, end};
		SendMessageW(control, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&range));
	}
	static CHARRANGE Selection(HWND control)
	{
		CHARRANGE range{};
		SendMessageW(control, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&range));
		return range;
	}
	static std::wstring Selected(HWND control)
	{
		const auto range = Selection(control);
		std::wstring text(range.cpMax - range.cpMin + 1, L'\0');
		const auto length = SendMessageW(control, EM_GETSELTEXT, 0, reinterpret_cast<LPARAM>(text.data()));
		text.resize(length); return text;
	}
	static LONG FirstCharacter(HWND control)
	{
		return static_cast<LONG>(SendMessageW(control, EM_LINEINDEX,
			SendMessageW(control, EM_GETFIRSTVISIBLELINE, 0, 0), 0));
	}
	void Enter() { SendMessageW(Access::Input(View), WM_CHAR, L'\r', 0); }
};

TEST_F(NativeConsoleWindow, UnicodeSelectionAndViewportStayStableWhileOutputArrives)
{
	const auto transcript = Access::Transcript(View);
	const std::string line = "prefix \xF0\x9F\x8E\xB5 \xE4\xB8\xAD \x65\xCC\x81 suffix";
	Fill(100, line);
	Select(transcript, 7, 9);
	EXPECT_EQ(Selected(transcript), L"\xd83c\xdfb5");
	SendMessageW(transcript, WM_VSCROLL, SB_TOP, 0);
	SendMessageW(transcript, EM_LINESCROLL, 0, 15);
	const auto before = FirstCharacter(transcript);
	const auto selected = Selected(transcript);
	Append("new output");
	EXPECT_EQ(FirstCharacter(transcript), before);
	EXPECT_EQ(Selected(transcript), selected);
	EXPECT_EQ(Selection(transcript).cpMin, 7);
}

TEST_F(NativeConsoleWindow, WrappedUnicodeAnchorAndSelectionSurvivePrefixEviction)
{
	const auto transcript = Access::Transcript(View);
	const std::string line = "\xF0\x9F\x8E\xB5 " + std::string(250, 'w');
	SetWindowPos(Access::Window(View), nullptr, 0, 0, 340, 240, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
	Fill(1100, line); // UTF-16 cap trims well before the logical entry cap.
	const LONG unitsBefore = Access::Units(View);
	const LONG entryUnits = 254; // surrogate pair + space + 250 letters + paragraph.
	Select(transcript, entryUnits * 30, entryUnits * 30 + 2);
	const auto selected = Selected(transcript);
	SendMessageW(transcript, WM_VSCROLL, SB_TOP, 0);
	SendMessageW(transcript, EM_LINESCROLL, 0, 100);
	const LONG anchorBefore = FirstCharacter(transcript);
	Append(line);
	const LONG removed = unitsBefore + entryUnits - Access::Units(View);
	EXPECT_GT(removed, 0);
	EXPECT_EQ(Selection(transcript).cpMin, entryUnits * 30 - removed);
	EXPECT_EQ(Selected(transcript), selected);
	EXPECT_EQ(FirstCharacter(transcript), anchorBefore - removed);
	EXPECT_LE(Access::Units(View), console::ConsoleWindow::MaxHistoryUnits);
}

TEST_F(NativeConsoleWindow, LargeWrappedHistoryViewportAvoidsSixteenBitPixelTruncation)
{
	const auto transcript = Access::Transcript(View);
	SetWindowPos(Access::Window(View), nullptr, 0, 0, 280, 220, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
	Fill(700, std::string(200, 'w'));
	Select(transcript, 100, 110);
	SendMessageW(transcript, WM_VSCROLL, SB_TOP, 0);
	SendMessageW(transcript, EM_LINESCROLL, 0, 4000);
	const LONG anchor = FirstCharacter(transcript);
	EXPECT_GT(anchor, 50000);
	Append("later");
	EXPECT_EQ(FirstCharacter(transcript), anchor);
	EXPECT_EQ(Selection(transcript).cpMin, 100);
}

TEST_F(NativeConsoleWindow, EntryAndUnitBoundsApplyToProgrammaticAppendAndEvictedSelection)
{
	const auto transcript = Access::Transcript(View);
	Fill(console::ConsoleWindow::MaxHistoryEntries, "x");
	Select(transcript, 0, 1);
	Append("after");
	EXPECT_EQ(Access::Entries(View), console::ConsoleWindow::MaxHistoryEntries);
	EXPECT_EQ(Selection(transcript).cpMin, 0);
	EXPECT_EQ(Selection(transcript).cpMax, 0);
	Fill(100, std::string(4096, 'x'));
	EXPECT_LE(Access::Units(View), console::ConsoleWindow::MaxHistoryUnits);
	GETTEXTLENGTHEX query{GTL_NUMCHARS | GTL_PRECISE, 1200};
	EXPECT_EQ(SendMessageW(transcript, EM_GETTEXTLENGTHEX, reinterpret_cast<WPARAM>(&query), 0), Access::Units(View));
}

TEST_F(NativeConsoleWindow, TailFollowsWithoutSelectionButSelectionStopsIt)
{
	const auto transcript = Access::Transcript(View);
	Fill(150);
	const auto first = FirstCharacter(transcript);
	EXPECT_GT(first, 0);
	Append("followed");
	EXPECT_GT(FirstCharacter(transcript), first);
	Select(transcript, 20, 25);
	const auto held = FirstCharacter(transcript);
	Append("selected");
	EXPECT_EQ(FirstCharacter(transcript), held);
	EXPECT_EQ(Selection(transcript).cpMin, 20);
	SendMessageW(transcript, WM_CHAR, L'x', 0);
	EXPECT_EQ(Selection(transcript).cpMin, 20);
}

TEST_F(NativeConsoleWindow, InputUsesUtf8ByteLimitAndRetainsInvalidOrFullQueueDraft)
{
	const auto input = Access::Input(View);
	SetWindowTextW(input, L"hello \xd83c\xdfb5"); Enter();
	auto command = Commands.Take(); ASSERT_TRUE(command);
	EXPECT_EQ(command->Text, "hello \xF0\x9F\x8E\xB5");
	EXPECT_TRUE(Text(input).empty());
	const std::wstring tooManyBytes(2049, L'\x00e9');
	SetWindowTextW(input, tooManyBytes.c_str()); Enter();
	EXPECT_EQ(Text(input), tooManyBytes); EXPECT_FALSE(Commands.Take());
	SetWindowTextW(input, L"one\rtwo"); Enter();
	EXPECT_FALSE(Commands.Take());
	EXPECT_EQ(Text(input), L"one\rtwo");
	for (std::size_t index = 0; index < console::CommandMailbox::Capacity; ++index)
		ASSERT_TRUE(Commands.Submit("pending"));
	SetWindowTextW(input, L"draft"); Enter();
	EXPECT_EQ(Text(input), L"draft");
	Commands.Close(); Enter(); EXPECT_EQ(Text(input), L"draft");
}

TEST_F(NativeConsoleWindow, CommandValidationRejectsControlsMalformedUnicodeAndMultilinePaste)
{
	std::string text;
	for (const auto* invalid : { L"line\nline", L"line\rline", L"line\tline", L"line\x2028line", L"line\x2029line", L"\xd800", L"\xdc00", L"\x007f" })
		EXPECT_FALSE(Access::Command(invalid, text));
	EXPECT_FALSE(Access::Command(std::wstring_view(L"a\0b", 3), text));
	EXPECT_TRUE(Access::Command(std::wstring(1024, L'\x4e2d'), text));
	EXPECT_EQ(text.size(), 3072);
	EXPECT_FALSE(Access::Command(std::wstring(1366, L'\x4e2d'), text));
	EXPECT_TRUE(Access::Command(std::wstring(4096, L'x'), text));
}

TEST_F(NativeConsoleWindow, ResizeDpiHelpAndStatusUseNativeControls)
{
	const auto window = Access::Window(View), input = Access::Input(View);
	EXPECT_NE(Text(window).find(L"/help or F1"), std::wstring::npos);
	RECT old{}; GetWindowRect(input, &old);
	SetWindowPos(window, nullptr, 0, 0, 1000, 600, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
	RECT resized{}; GetWindowRect(input, &resized);
	EXPECT_GT(resized.right - resized.left, old.right - old.left);
	RECT dpiBounds{0, 0, 800, 500};
	SendMessageW(window, WM_DPICHANGED, MAKEWPARAM(144, 144), reinterpret_cast<LPARAM>(&dpiBounds));
	RECT scaled{}; GetWindowRect(input, &scaled);
	EXPECT_GT(scaled.bottom - scaled.top, resized.bottom - resized.top);
	SetWindowTextW(input, L"unfinished chat");
	for (const auto control : { input, Access::Transcript(View) })
	{
		SendMessageW(control, WM_KEYDOWN, VK_F1, 0);
		auto command = Commands.Take(); ASSERT_TRUE(command); EXPECT_EQ(command->Text, "/help");
		std::ostringstream output;
		EXPECT_TRUE(console::TryWriteGeneralHelp(console::ParseCommand(command->Text), output));
		EXPECT_EQ(output.str(), console::GeneralHelpText);
		EXPECT_EQ(Text(input), L"unfinished chat");
	}
	Append("[NINJAM] latest event"); View.UpdateStatus(true);
	EXPECT_NE(Text(Access::Status(View)).find(L"connected | [NINJAM] latest event"), std::wstring::npos);
	const auto transcript = Access::Transcript(View);
	Select(transcript, 0, 1);
	CHARFORMAT2W format{sizeof(format)};
	SendMessageW(transcript, EM_GETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&format));
	EXPECT_EQ(format.crTextColor, RGB(123, 213, 224));
}

TEST_F(NativeConsoleWindow, NativeInputPasteReplacesSelectionAndRejectsMultilineOrOversizeDraft)
{
	const auto input = Access::Input(View);
	SetWindowTextW(input, L"hello world");
	SendMessageW(input, EM_SETSEL, 6, 11);
	Access::Paste(View, L"\xd83c\xdfb5");
	EXPECT_EQ(Text(input), L"hello \xd83c\xdfb5");
	const auto draft = Text(input);
	for (const auto* invalid : { L"one\r\ntwo", L"one\ntwo", L"one\ttwo" })
	{
		Access::Paste(View, invalid);
		EXPECT_EQ(Text(input), draft);
		EXPECT_FALSE(Commands.Take());
	}
	const std::wstring nearLimit(4095, L'x');
	SetWindowTextW(input, nearLimit.c_str());
	SendMessageW(input, EM_SETSEL, 4095, 4095);
	Access::Paste(View, L"\x00e9");
	EXPECT_EQ(Text(input), nearLimit);
	Access::Paste(View, L"x");
	EXPECT_EQ(Text(input).size(), 4096);
	Enter();
	auto command = Commands.Take(); ASSERT_TRUE(command);
	EXPECT_EQ(command->Text.size(), 4096);
}
