#include "../../../../console/TextCoordinates.h"
#include "../../../../console/PromptEditor.h"
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
