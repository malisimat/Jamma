#include "../../../../console/TextCoordinates.h"
#include "../../../../console/PromptEditor.h"
#include "../../../../console/Transcript.h"
#include "../../../../console/PasteInput.h"
#include <gtest/gtest.h>
#include <array>

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
