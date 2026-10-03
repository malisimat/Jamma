
#include "gtest/gtest.h"
#include "resources/ResourceLib.h"
#include "actions/ActionUndoHistory.h"

using base::ActionUndo;
using base::ActionSender;
using base::ActionReceiver;
using actions::ActionUndoHistory;
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

TEST(ActionUndoHistory, CannotUndoWhenEmpty) {
	auto sender = std::make_shared<MockedActionSender>(nullptr);
	auto actionUndo = std::make_shared<ActionUndo>(sender);

	auto undoHistory = ActionUndoHistory();
	undoHistory.Add(actionUndo);

	ASSERT_TRUE(undoHistory.Undo());
	ASSERT_FALSE(undoHistory.Undo());
}

TEST(ActionUndoHistory, SkipsAddingNull) {
	auto undoHistory = ActionUndoHistory();
	undoHistory.Add(nullptr);

	ASSERT_FALSE(undoHistory.Undo());
}

TEST(ActionUndoHistory, CanRedoTwice) {
	auto sender = std::make_shared<MockedActionSender>(nullptr);
	auto actionUndo = std::make_shared<ActionUndo>(sender);

	auto undoHistory = ActionUndoHistory();
	undoHistory.Add(actionUndo);
	undoHistory.Add(actionUndo);

	ASSERT_TRUE(undoHistory.Undo());
	ASSERT_TRUE(undoHistory.Undo());

	ASSERT_TRUE(undoHistory.Redo());
	ASSERT_TRUE(undoHistory.Redo());

	ASSERT_FALSE(undoHistory.Redo());
}

TEST(ActionUndoHistory, CanRedoAfterEmpty) {
	auto sender = std::make_shared<MockedActionSender>(nullptr);
	auto actionUndo = std::make_shared<ActionUndo>(sender);

	auto undoHistory = ActionUndoHistory();
	undoHistory.Add(actionUndo);

	ASSERT_TRUE(undoHistory.Undo());
	ASSERT_FALSE(undoHistory.Undo());

	ASSERT_TRUE(undoHistory.Redo());
	ASSERT_FALSE(undoHistory.Redo());
}

TEST(ActionUndoHistory, CannotRedoAfterAdding) {
	auto sender = std::make_shared<MockedActionSender>(nullptr);
	auto actionUndo = std::make_shared<ActionUndo>(sender);

	auto undoHistory = ActionUndoHistory();
	undoHistory.Add(actionUndo);

	ASSERT_TRUE(undoHistory.Undo());

	undoHistory.Add(actionUndo);

	ASSERT_FALSE(undoHistory.Redo());
}

class TrackingUndo final : public ActionUndo
{
public:
	TrackingUndo(int id, std::vector<int>& order) : ActionUndo({}), Id(id), Order(order) {}
	bool Undo() override { Order.push_back(-Id); return Succeeds; }
	bool Redo() override { Order.push_back(Id); return Succeeds; }
	int Id;
	std::vector<int>& Order;
	bool Succeeds = true;
};

TEST(ActionUndoHistory, LastActionFirstAndFailureRetainsAction)
{
	std::vector<int> order;
	ActionUndoHistory history;
	auto first = std::make_shared<TrackingUndo>(1, order);
	auto second = std::make_shared<TrackingUndo>(2, order);
	history.Add(first);
	history.Add(second);
	second->Succeeds = false;
	EXPECT_FALSE(history.Undo());
	second->Succeeds = true;
	EXPECT_TRUE(history.Undo());
	EXPECT_TRUE(history.Undo());
	EXPECT_TRUE(history.Redo());
	EXPECT_TRUE(history.Redo());
	EXPECT_EQ((std::vector<int>{ -2, -2, -1, 1, 2 }), order);
}
