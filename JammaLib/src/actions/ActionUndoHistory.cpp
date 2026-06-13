#include "ActionUndoHistory.h"

using namespace actions;
using base::ActionUndo;

ActionUndoHistory::ActionUndoHistory()
{
}

ActionUndoHistory::~ActionUndoHistory()
{
}

void actions::ActionUndoHistory::Add(std::shared_ptr<ActionUndo> actionUndo)
{
	while (!_poppedHistory.empty())
		_poppedHistory.pop();

	_history.push(actionUndo);
}

void actions::ActionUndoHistory::Clear()
{
	while (!_poppedHistory.empty())
		_poppedHistory.pop();

	while (!_history.empty())
		_history.pop();
}

bool actions::ActionUndoHistory::Undo()
{
	auto lastOpt = Pop();

	if (!lastOpt.has_value())
		return false;

	auto last = lastOpt.value();

	if (last)
		return last->Undo();

	return false;
}

bool actions::ActionUndoHistory::Redo()
{
	auto nextOpt = UnPop();

	if (!nextOpt.has_value())
		return false;

	auto next = nextOpt.value();

	if (next)
		return next->Redo();

	return false;
}

std::optional<std::shared_ptr<ActionUndo>> actions::ActionUndoHistory::Pop()
{
	if (_history.empty())
		return std::nullopt;

	_poppedHistory.push(_history.front());
	_history.pop();

	return _poppedHistory.front();
}

std::optional<std::shared_ptr<ActionUndo>> actions::ActionUndoHistory::UnPop()
{
	if (_poppedHistory.empty())
		return std::nullopt;

	_history.push(_poppedHistory.front());
	_poppedHistory.pop();

	return _history.front();
}
