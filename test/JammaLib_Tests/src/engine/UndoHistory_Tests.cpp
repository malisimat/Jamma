
#include "gtest/gtest.h"
#include "resources/ResourceLib.h"
#include "engine/UndoHistory.h"

using base::ActionUndo;
using base::ActionSender;
using base::ActionReceiver;
using engine::UndoHistory;
using actions::TouchAction;
using actions::TouchMoveAction;

class MockedActionSender :
	public ActionSender
{
public:
	MockedActionSender() :
		ActionSender() {}
	MockedActionSender(std::shared_ptr<ActionReceiver> receiver) :
		ActionSender(receiver) {}
public:
	virtual bool Undo(std::shared_ptr<ActionUndo> undo) override { return true; }
	virtual bool Redo(std::shared_ptr<ActionUndo> undo) override { return true; }
};

TEST(UndoHistory, CannotUndoWhenEmpty) {
	auto sender = std::make_shared<MockedActionSender>(nullptr);
	auto actionUndo = std::make_shared<ActionUndo>(sender);

	auto undoHistory = UndoHistory();
	undoHistory.Add(actionUndo);

	ASSERT_TRUE(undoHistory.Undo());
	ASSERT_FALSE(undoHistory.Undo());
}

TEST(UndoHistory, SkipsAddingNull) {
	auto undoHistory = UndoHistory();
	undoHistory.Add(nullptr);

	ASSERT_FALSE(undoHistory.Undo());
}

TEST(UndoHistory, CanRedoTwice) {
	auto sender = std::make_shared<MockedActionSender>(nullptr);
	auto actionUndo = std::make_shared<ActionUndo>(sender);

	auto undoHistory = UndoHistory();
	undoHistory.Add(actionUndo);
	undoHistory.Add(actionUndo);

	ASSERT_TRUE(undoHistory.Undo());
	ASSERT_TRUE(undoHistory.Undo());

	ASSERT_TRUE(undoHistory.Redo());
	ASSERT_TRUE(undoHistory.Redo());

	ASSERT_FALSE(undoHistory.Redo());
}

TEST(UndoHistory, CanRedoAfterEmpty) {
	auto sender = std::make_shared<MockedActionSender>(nullptr);
	auto actionUndo = std::make_shared<ActionUndo>(sender);

	auto undoHistory = UndoHistory();
	undoHistory.Add(actionUndo);

	ASSERT_TRUE(undoHistory.Undo());
	ASSERT_FALSE(undoHistory.Undo());

	ASSERT_TRUE(undoHistory.Redo());
	ASSERT_FALSE(undoHistory.Redo());
}

TEST(UndoHistory, CannotRedoAfterAdding) {
	auto sender = std::make_shared<MockedActionSender>(nullptr);
	auto actionUndo = std::make_shared<ActionUndo>(sender);

	auto undoHistory = UndoHistory();
	undoHistory.Add(actionUndo);

	ASSERT_TRUE(undoHistory.Undo());

	undoHistory.Add(actionUndo);

	ASSERT_FALSE(undoHistory.Redo());
}
