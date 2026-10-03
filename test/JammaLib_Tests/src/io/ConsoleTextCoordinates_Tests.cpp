#include "../../../../console/TextCoordinates.h"
#include "../../../../console/PromptEditor.h"
#include "../../../../console/Transcript.h"
#include "../../../../console/PasteInput.h"
#include "../../../../console/SelectionController.h"
#include "../../../../console/ClipboardText.h"
#include <gtest/gtest.h>
#include <array>

TEST(ConsoleTranscript, WrappedRowsRetainLogicalTextForSemanticRendering)
{
	console::Transcript transcript;
	transcript.Append("[NINJAM] <you> a long line");
	const auto rows = transcript.Visible(8, 16);
	ASSERT_GT(rows.size(), 1u);
	for (const auto& row : rows)
	{
		const auto* logical = transcript.TextFor(row.Start.EntryId);
		ASSERT_NE(logical, nullptr);
		EXPECT_EQ(*logical, "[NINJAM] <you> a long line");
	}
	EXPECT_EQ(transcript.TextFor(rows.front().Start.EntryId + 1), nullptr);
}

TEST(ConsoleTextCoordinates, GraphemeMovementAndFullWidthClick)
{
	const std::string text = "a\xCC\x84\xE6\xB5\x8B\xF0\x9F\xAA\x90"; // ā测🪐
	ASSERT_TRUE(console::ValidUtf8(text));
	const auto clusters = console::Graphemes(text);
	ASSERT_EQ(clusters.size(), 3u);
	EXPECT_EQ(clusters[0].End, 3u);
	EXPECT_EQ(clusters[1].Columns, 2);
	EXPECT_EQ(clusters[2].Columns, 2);
	EXPECT_EQ(console::NextGrapheme(text, 0), 3u);
	EXPECT_EQ(console::PreviousGrapheme(text, 6), 3u);
	EXPECT_EQ(console::ColumnToByte(text, 0), 0u);
	EXPECT_EQ(console::ColumnToByte(text, 1), 3u);
	EXPECT_EQ(console::ColumnToByte(text, 2), 6u);
	EXPECT_EQ(console::ColumnToByte(text, 3), 6u);
}

TEST(ConsoleTextCoordinates, EmojiJoinerIsOneCluster)
{
	const std::string text = "\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x92\xBB"; // woman technologist
	ASSERT_TRUE(console::ValidUtf8(text));
	const auto clusters = console::Graphemes(text);
	ASSERT_EQ(clusters.size(), 1u);
	EXPECT_EQ(clusters[0].Columns, 5); // Pinned FTXUI also counts the ZWJ cell.
	EXPECT_EQ(console::NextGrapheme(text, 0), text.size());
}

TEST(ConsoleTextCoordinates, FlagEmojiMatchesFtxuiCells)
{
	const std::string text = "\xF0\x9F\x87\xAC\xF0\x9F\x87\xA7"; // GB flag
	const auto clusters = console::Graphemes(text);
	ASSERT_EQ(clusters.size(), 1u);
	EXPECT_EQ(clusters.front().Columns, ftxui::string_width(text));
	EXPECT_EQ(console::ColumnToByte(text, 0), 0u);
	EXPECT_EQ(console::ColumnToByte(text, clusters.front().Columns - 1), text.size());
}

TEST(ConsoleTextCoordinates, ZeroWidthClusterDoesNotConsumeCell)
{
	const std::string text = "\xCC\x84" "a";
	const auto clusters = console::Graphemes(text);
	ASSERT_EQ(clusters.size(), 2u);
	EXPECT_EQ(clusters[0].Columns, 0);
	EXPECT_EQ(console::ColumnToByte(text, 0), 2u);
	EXPECT_EQ(console::ColumnToByte(text, 1), text.size());
}

TEST(ConsoleTextCoordinates, SelectionExpandsContractsAndCopiesLogicalLines)
{
	const std::array<console::LogicalTextEntry, 2> entries{{ { 10, "first long logical line" }, { 11, "second" } }};
	console::TextSelection selection;
	selection.Press({ 10, 6 });
	selection.Drag({ 11, 6 });
	EXPECT_EQ(selection.Copy(entries), "long logical line\nsecond");
	selection.Drag({ 10, 10 });
	EXPECT_EQ(selection.Copy(entries), "long");
	selection.Drag({ 10, 2 });
	EXPECT_EQ(selection.Copy(entries), "rst ");
	selection.Clear();
	EXPECT_TRUE(selection.Copy(entries).empty());
}

TEST(ConsoleTextCoordinates, SelectionRejectsEvictedOrSplitGraphemes)
{
	const std::array<console::LogicalTextEntry, 2> entries{{ { 7, "a\xCC\x84" }, { 9, "\xE6\xB5\x8B" } }};
	console::TextSelection selection;
	selection.Press({ 7, 1 });
	selection.Drag({ 9, 3 });
	EXPECT_TRUE(selection.Copy(entries).empty());
	selection.Press({ 7, 0 });
	selection.Drag({ 9, 3 });
	EXPECT_EQ(selection.Copy(entries), "a\xCC\x84\n\xE6\xB5\x8B");
	const std::array<console::LogicalTextEntry, 1> afterEviction{{ { 9, "\xE6\xB5\x8B" } }};
	selection.Reconcile(afterEviction);
	EXPECT_FALSE(selection.Active);
	EXPECT_TRUE(selection.Copy(afterEviction).empty());
}

TEST(ConsoleTextCoordinates, EdgeScrollAndClipboardFailureAreBounded)
{
	EXPECT_EQ(console::EdgeScrollDelta(-100, 8), -1);
	EXPECT_EQ(console::EdgeScrollDelta(0, 8), 0);
	EXPECT_EQ(console::EdgeScrollDelta(7, 8), 0);
	EXPECT_EQ(console::EdgeScrollDelta(100, 8), 1);
	EXPECT_EQ(console::EdgeScrollDelta(1, 0), 0);
	const std::array<console::LogicalTextEntry, 1> entries{{ { 2, "hello" } }};
	console::TextSelection selection;
	selection.Press({ 2, 0 });
	selection.Drag({ 2, 5 });
	std::string clipboard;
	EXPECT_TRUE(selection.CopyToClipboard(entries, [&](std::string_view text) {
		clipboard = text; return true;
	}));
	EXPECT_EQ(clipboard, "hello");
	EXPECT_FALSE(selection.CopyToClipboard(entries, [](std::string_view) { return false; }));
}

TEST(ConsolePromptEditor, UnicodeCaretSelectionAndInputLimits)
{
	console::PromptEditor editor;
	EXPECT_EQ(editor.Insert("a\xCC\x84\xE6\xB5\x8B"), console::InputResult::Accepted);
	EXPECT_EQ(editor.Caret(), editor.Text().size());
	editor.MoveLeft();
	EXPECT_EQ(editor.Caret(), 3u);
	editor.MoveLeft(true);
	EXPECT_EQ(editor.SelectedText(), "a\xCC\x84");
	editor.MoveRight(true);
	EXPECT_FALSE(editor.HasSelection());
	editor.ClickColumn(2);
	EXPECT_EQ(editor.Caret(), editor.Text().size());
	editor.Backspace();
	EXPECT_EQ(editor.Text(), "a\xCC\x84");
	EXPECT_EQ(editor.Insert("\xED\xA0\x80"), console::InputResult::InvalidUtf8);
	EXPECT_EQ(editor.Insert("a\nb"), console::InputResult::Multiline);
	EXPECT_EQ(editor.Insert(std::string(console::MaxInputBytes + 1, 'x')),
		console::InputResult::TooLong);
}

TEST(ConsolePromptEditor, InsertionKeepsCaretOutsideMergedGrapheme)
{
	console::PromptEditor editor;
	ASSERT_EQ(editor.Insert("\xCC\x84"), console::InputResult::Accepted);
	editor.MoveHome();
	ASSERT_EQ(editor.Insert("a"), console::InputResult::Accepted);
	EXPECT_EQ(editor.Caret(), editor.Text().size());
	editor.Backspace();
	EXPECT_TRUE(editor.Text().empty());
}

TEST(ConsolePromptEditor, WordNavigationAndSubmit)
{
	console::PromptEditor editor;
	ASSERT_EQ(editor.Insert("hello \xE6\xB5\x8B world"), console::InputResult::Accepted);
	editor.MoveWordLeft();
	EXPECT_EQ(editor.Caret(), editor.Text().find("world"));
	editor.DeleteWordLeft();
	EXPECT_EQ(editor.Text(), "hello world");
	EXPECT_EQ(editor.Take(), "hello world");
	EXPECT_TRUE(editor.Text().empty());
	EXPECT_EQ(editor.Caret(), 0u);
}

TEST(ConsolePromptEditor, WordMotionAtInputLimit)
{
	console::PromptEditor editor;
	ASSERT_EQ(editor.Insert(std::string(console::MaxInputBytes, 'x')),
		console::InputResult::Accepted);
	editor.MoveWordLeft();
	EXPECT_EQ(editor.Caret(), 0u);
	editor.MoveWordRight();
	EXPECT_EQ(editor.Caret(), console::MaxInputBytes);
}

TEST(ConsolePromptEditor, BracketedPasteRejectsControlsAndOversizeAtomically)
{
	console::PromptEditor editor;
	console::PasteInput paste;
	paste.Start();
	paste.Character("first");
	paste.Control(); // CR, LF, Escape, and other special input use this path.
	paste.Character("second");
	EXPECT_EQ(paste.Finish(editor), console::InputResult::Multiline);
	EXPECT_TRUE(editor.Text().empty());
	paste.Start();
	paste.Character(std::string(console::MaxInputBytes, 'x'));
	paste.Character("z");
	EXPECT_EQ(paste.Finish(editor), console::InputResult::TooLong);
	EXPECT_TRUE(editor.Text().empty());
	paste.Start();
	paste.Character("hello");
	EXPECT_EQ(paste.Finish(editor), console::InputResult::Accepted);
	EXPECT_EQ(editor.Text(), "hello");
}

TEST(ConsoleTranscript, RetentionAndStableIds)
{
	console::Transcript transcript;
	for (std::size_t index = 0; index < console::Transcript::MaxEntries + 2; ++index)
		transcript.Append("x");
	EXPECT_EQ(transcript.Size(), console::Transcript::MaxEntries);
	EXPECT_EQ(transcript.Bytes(), console::Transcript::MaxEntries);
	const auto visible = transcript.Visible(10, 2);
	ASSERT_EQ(visible.size(), 2u);
	EXPECT_EQ(visible.front().Start.EntryId, 100001u);
}

TEST(ConsoleTranscript, ByteRetentionEvictsOldestEntry)
{
	console::Transcript transcript;
	const std::string entry(4096, 'x');
	for (std::size_t index = 0; index < console::Transcript::MaxBytes / entry.size() + 1; ++index)
		transcript.Append(entry);
	EXPECT_EQ(transcript.Bytes(), console::Transcript::MaxBytes);
	EXPECT_EQ(transcript.Size(), console::Transcript::MaxBytes / entry.size());
	EXPECT_EQ(transcript.FirstId(), 2u);
}

TEST(ConsoleTranscript, WrapReflowAndPausedAnchor)
{
	console::Transcript transcript;
	transcript.Append("a\xCC\x84\xE6\xB5\x8B" "bc\nlast");
	auto visible = transcript.Visible(2, 5);
	ASSERT_EQ(visible.size(), 5u);
	EXPECT_EQ(visible[0].Text, "a\xCC\x84");
	EXPECT_EQ(visible[1].Text, "\xE6\xB5\x8B");
	transcript.ScrollUp(2, 2);
	EXPECT_FALSE(transcript.Following());
	const auto anchor = transcript.Anchor();
	transcript.Append("new");
	visible = transcript.Visible(4, 3);
	EXPECT_EQ(visible.front().Start.EntryId, anchor.EntryId);
	transcript.FollowTail();
	visible = transcript.Visible(4, 1);
	ASSERT_EQ(visible.size(), 1u);
	EXPECT_EQ(visible.front().Text, "new");
}

TEST(ConsoleTranscript, LargeEntryRendersOnlyViewportRows)
{
	console::Transcript transcript;
	transcript.Append(std::string(16000, 'x'));
	const auto visible = transcript.Visible(1, 3);
	ASSERT_EQ(visible.size(), 3u);
	for (const auto& row : visible) EXPECT_EQ(row.Text, "x");
	EXPECT_EQ(visible.front().Start.Byte, 15997u);
}

TEST(ConsoleTranscript, ScrollAfterResizeUsesContainingRow)
{
	console::Transcript transcript;
	transcript.Append("abcdef");
	const auto tail = transcript.Visible(2, 1);
	ASSERT_EQ(tail.front().Start.Byte, 4u);
	transcript.ScrollUp(3, 1);
	EXPECT_EQ(transcript.Anchor().Byte, 0u);
}

TEST(ConsoleTextCoordinates, NarrowStatusAndPromptWindow)
{
	EXPECT_EQ(console::StatusForWidth("NINJAM connected | last: hello", 8), "C|hello");
	EXPECT_EQ(console::StatusForWidth("NINJAM disconnected | last: hello", 1), "D");
	const std::string prompt = "abcdef\xE6\xB5\x8B";
	EXPECT_EQ(console::PromptWindowStart(prompt, prompt.size(), 3), 5u);
	EXPECT_EQ(console::ClipColumns(prompt.substr(5), 3), "f\xE6\xB5\x8B");
}

TEST(ConsoleTextCoordinates, WrappedRowHitTestAndSelectionRange)
{
	console::Transcript transcript;
	transcript.Append("a\xCC\x84\xE6\xB5\x8B" "bc\nnext");
	const auto rows = transcript.Visible(2, 5);
	ASSERT_EQ(rows.size(), 5u);
	EXPECT_EQ(console::RowPosition(rows[0].Start, rows[0].Text, 1).Byte, 3u);
	EXPECT_EQ(console::RowPosition(rows[1].Start, rows[1].Text, 0).Byte, 3u);
	EXPECT_EQ(console::RowPosition(rows[1].Start, rows[1].Text, 1).Byte, 6u);
	console::TextSelection selection;
	selection.Press(console::RowPosition(rows[0].Start, rows[0].Text, 1));
	selection.Drag(console::RowPosition(rows[3].Start, rows[3].Text, 2));
	const auto middle = selection.RowRange(rows[1].Start, rows[1].Text.size());
	ASSERT_TRUE(middle);
	EXPECT_EQ(*middle, (std::pair<std::size_t, std::size_t>{ 0, 3 }));
	EXPECT_EQ(selection.Copy(transcript.LogicalEntries()), "\xE6\xB5\x8B" "bc\nne");
}

TEST(ConsoleTextCoordinates, PromptClickUsesWindowAndGraphemeBoundaries)
{
	const std::string text = "abcdef\xE6\xB5\x8B";
	const auto caret = text.size();
	EXPECT_EQ(console::PromptColumnToByte(text, caret, 6, 2), 5u);
	EXPECT_EQ(console::PromptColumnToByte(text, caret, 6, 3), 6u);
	EXPECT_EQ(console::PromptColumnToByte(text, caret, 6, 4), caret);
	console::PromptEditor prompt;
	ASSERT_EQ(prompt.Insert(text), console::InputResult::Accepted);
	prompt.ClickByte(console::PromptColumnToByte(prompt.Text(), prompt.Caret(), 6, 3));
	prompt.ClickByte(console::PromptColumnToByte(prompt.Text(), prompt.Caret(), 6, 0), true);
	EXPECT_TRUE(prompt.HasSelection());
	EXPECT_TRUE(console::IsGraphemeBoundary(prompt.Text(), prompt.Caret()));
}

TEST(ConsoleTextCoordinates, PromptDragKeepsWindowUntilRelease)
{
	console::PromptEditor prompt;
	ASSERT_EQ(prompt.Insert("abcdefghij"), console::InputResult::Accepted);
	const auto origin = console::PromptWindowStart(prompt.Text(), prompt.Caret(), 3);
	ASSERT_EQ(origin, 7u);
	prompt.ClickByte(console::PromptColumnToByte(prompt.Text(), prompt.Caret(), 6, 2));
	prompt.ClickByte(console::PromptColumnToByteAtOrigin(prompt.Text(), origin, 6, 2), true);
	EXPECT_FALSE(prompt.HasSelection());
	prompt.ClickByte(console::PromptColumnToByteAtOrigin(prompt.Text(), origin, 6, 5), true);
	EXPECT_EQ(prompt.SelectedText(), "hij");
	prompt.ClickByte(console::PromptColumnToByteAtOrigin(prompt.Text(), origin, 6, 3), true);
	EXPECT_EQ(prompt.SelectedText(), "h");
}

TEST(ConsoleTextCoordinates, FullWidthCaretClickUsesSecondCell)
{
	const std::string text = "a\xE6\xB5\x8B" "b";
	EXPECT_EQ(console::PromptColumnToByte(text, 1, 6, 3), 1u);
	EXPECT_EQ(console::PromptColumnToByte(text, 1, 6, 4), 4u);
	const std::string emoji = "\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x92\xBB" "x";
	EXPECT_EQ(console::PromptColumnToByte(emoji, 0, 4, 2), 0u);
	EXPECT_EQ(console::PromptColumnToByte(emoji, 0, 4, 3), emoji.size() - 1);
}

TEST(ConsoleTextCoordinates, SelectionSurvivesReflowAndClearsOnEviction)
{
	console::Transcript transcript;
	transcript.Append("long logical text");
	const auto first = transcript.Visible(4, 3);
	console::TextSelection selection;
	selection.Press({ first.front().Start.EntryId, 1 });
	selection.Drag({ first.front().Start.EntryId, 12 });
	transcript.Visible(7, 3);
	transcript.Append("new output");
	EXPECT_EQ(selection.Copy(transcript.LogicalEntries()), "ong logical");
	for (std::size_t i = 0; i < console::Transcript::MaxEntries; ++i)
		transcript.Append("x");
	selection.ReconcileBounds(transcript.FirstId(), transcript.LastId());
	EXPECT_FALSE(selection.Active);
}

TEST(ConsoleTextCoordinates, PointerSelectionExpandsContractsAndReleases)
{
	console::PromptEditor prompt;
	console::SelectionController selection;
	selection.PressTranscript({ 1, 0 }, 0, 2, prompt);
	selection.DragTranscriptRow({ 1, 0 }, "ab", 0, 2);
	EXPECT_FALSE(selection.Transcript.RowRange({ 1, 0 }, 2));
	selection.DragTranscriptRow({ 1, 0 }, "ab", 1, 2);
	EXPECT_EQ(selection.Transcript.RowRange({ 1, 0 }, 2),
		(std::optional<std::pair<std::size_t, std::size_t>>{ { 0, 2 } }));
	selection.DragTranscriptRow({ 1, 0 }, "ab", 0, 2);
	EXPECT_FALSE(selection.Transcript.RowRange({ 1, 0 }, 2));
	selection.DragTranscriptRow({ 1, 0 }, "ab", 1, 2);
	selection.Release();
	EXPECT_EQ(selection.Dragging, console::SelectionRegion::None);
	EXPECT_TRUE(selection.Transcript.Active);
	selection.DragTranscriptRow({ 1, 0 }, "ab", 0, 2);
	EXPECT_EQ(selection.Transcript.Focus.Byte, 2u);
}

TEST(ConsoleTextCoordinates, RegionSwitchClearsOtherSelection)
{
	console::PromptEditor prompt;
	ASSERT_EQ(prompt.Insert("hello"), console::InputResult::Accepted);
	console::SelectionController selection;
	selection.PressTranscript({ 1, 0 }, 0, 2, prompt);
	selection.DragTranscriptRow({ 1, 0 }, "abc", 1, 2);
	ASSERT_TRUE(selection.Transcript.Active);
	selection.PressPrompt(1, 0, prompt);
	EXPECT_FALSE(selection.Transcript.Active);
	selection.DragPrompt(3, prompt);
	EXPECT_EQ(prompt.SelectedText(), "el");
	selection.PressTranscript({ 2, 0 }, 0, 2, prompt);
	EXPECT_FALSE(prompt.HasSelection());
}

TEST(ConsoleTextCoordinates, EdgeScrollFocusUsesScrolledRow)
{
	console::PromptEditor prompt;
	console::Transcript transcript;
	transcript.Append("first\nsecond\nthird");
	const auto before = transcript.Visible(6, 2);
	console::SelectionController selection;
	selection.PressTranscript(before.front().Start, 0, 2, prompt);
	transcript.ScrollUp(6, 1);
	const auto after = transcript.Visible(6, 2);
	selection.DragTranscriptRow(after.front().Start, after.front().Text, 0, -1);
	EXPECT_EQ(selection.Transcript.Focus.EntryId, after.front().Start.EntryId);
	selection.Release();
}

TEST(ConsoleTextCoordinates, BackspaceClearsEmptySelectionAnchor)
{
	console::PromptEditor prompt;
	ASSERT_EQ(prompt.Insert("abc"), console::InputResult::Accepted);
	prompt.ClickByte(prompt.Caret(), true); // A drag that did not move.
	prompt.Backspace();
	EXPECT_EQ(prompt.Text(), "ab");
	EXPECT_FALSE(prompt.HasSelection());
	prompt.Backspace();
	EXPECT_EQ(prompt.Text(), "a");
}

TEST(ConsoleTextCoordinates, DeletingSeparatorKeepsCaretAtGraphemeBoundary)
{
	const std::string left = "\xF0\x9F\x87\xA6";
	const std::string right = "\xF0\x9F\x87\xA7";
	for (int operation = 0; operation < 3; ++operation)
	{
		console::PromptEditor prompt;
		ASSERT_EQ(prompt.Insert(left + "x" + right), console::InputResult::Accepted);
		prompt.ClickByte(left.size());
		if (operation == 0) prompt.Delete();
		else if (operation == 1)
		{
			prompt.MoveRight();
			prompt.Backspace();
		}
		else
		{
			prompt.MoveRight(true);
			prompt.Backspace();
		}
		EXPECT_EQ(prompt.Text(), left + right);
		EXPECT_TRUE(console::IsGraphemeBoundary(prompt.Text(), prompt.Caret()));
		prompt.Backspace();
		EXPECT_TRUE(prompt.Text().empty());
	}
}

TEST(ConsoleTextCoordinates, ClipboardUtf16RejectsUnpairedSurrogates)
{
	EXPECT_EQ(console::Utf16ToUtf8(L""), std::optional<std::string>(""));
	EXPECT_EQ(console::Utf16ToUtf8(L"hello"), std::optional<std::string>("hello"));
	const std::wstring high(1, static_cast<wchar_t>(0xD800));
	const std::wstring low(1, static_cast<wchar_t>(0xDC00));
	EXPECT_FALSE(console::Utf16ToUtf8(high));
	EXPECT_FALSE(console::Utf16ToUtf8(low));
	EXPECT_FALSE(console::Utf16ToUtf8(high + L"a"));
	EXPECT_EQ(console::Utf16ToUtf8(high + low), std::optional<std::string>("\xF0\x90\x80\x80"));
}

TEST(ConsoleTextCoordinates, ClipboardEncodingAndFailureLeaveSelectionIntact)
{
	const auto wide = console::Utf8ToUtf16("a\xE6\xB5\x8B\xF0\x9F\xAA\x90");
	ASSERT_TRUE(wide);
	EXPECT_EQ(*wide, L"a\u6D4B\U0001FA90");
	EXPECT_FALSE(console::Utf8ToUtf16("\xED\xA0\x80"));
	EXPECT_EQ(console::ClipboardUtf8ToUtf16("one\ntwo\rthree\r\nfour"),
		std::optional<std::wstring>(L"one\r\ntwo\r\nthree\r\nfour"));
	EXPECT_FALSE(console::ClipboardUtf8ToUtf16(std::string_view("a\0b", 3)));
	const std::array<console::LogicalTextEntry, 1> entries{{ { 1, "a\xE6\xB5\x8B" } }};
	console::TextSelection selection;
	selection.Press({ 1, 0 });
	selection.Drag({ 1, entries[0].Text.size() });
	EXPECT_FALSE(selection.CopyToClipboard(entries, [](std::string_view) { return false; }));
	EXPECT_TRUE(selection.Active);
	EXPECT_EQ(selection.Copy(entries), entries[0].Text);
}

TEST(ConsoleTextCoordinates, KeyboardAndHelpRemainWithoutMouseEvents)
{
	console::SelectionController selection;
	console::PromptEditor prompt;
	ASSERT_EQ(prompt.Insert("chat"), console::InputResult::Accepted);
	prompt.MoveLeft();
	prompt.Backspace();
	EXPECT_EQ(prompt.Text(), "cht");
	EXPECT_EQ(selection.Dragging, console::SelectionRegion::None);
}
