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
	if (!actionUndo)
		return;
	_poppedHistory.clear();
	_history.push_back(std::move(actionUndo));
}

void actions::ActionUndoHistory::Clear()
{
	_poppedHistory.clear();
	_history.clear();
}

bool actions::ActionUndoHistory::Undo()
{
	if (_history.empty())
		return false;
	auto last = _history.back();
	if (!last || !last->Undo())
		return false;
	_poppedHistory.push_back(last);
	_history.pop_back();
	return true;
}

bool actions::ActionUndoHistory::Redo()
{
	if (_poppedHistory.empty())
		return false;
	auto next = _poppedHistory.back();
	if (!next || !next->Redo())
		return false;
	_history.push_back(next);
	_poppedHistory.pop_back();
	return true;
}

std::optional<std::shared_ptr<ActionUndo>> actions::ActionUndoHistory::Pop()
{
	if (_history.empty())
		return std::nullopt;

	auto last = _history.back();
	_poppedHistory.push_back(last);
	_history.pop_back();
	return last;
}

std::optional<std::shared_ptr<ActionUndo>> actions::ActionUndoHistory::UnPop()
{
	if (_poppedHistory.empty())
		return std::nullopt;

	auto next = _poppedHistory.back();
	_history.push_back(next);
	_poppedHistory.pop_back();
	return next;
}
