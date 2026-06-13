#pragma once

#include <memory>
#include <functional>
#include <optional>
#include <queue>
#include "../base/ActionUndo.h"
#include "../base/ActionSender.h"
#include "../base/ActionReceiver.h"

namespace actions
{
	class ActionUndoHistory
	{
	public:
		ActionUndoHistory();
		~ActionUndoHistory();

	public:
		void Add(std::shared_ptr<base::ActionUndo> action);
		void Clear();

		bool Undo();
		bool Redo();

	protected:
		virtual std::optional<std::shared_ptr<base::ActionUndo>> Pop();
		virtual std::optional<std::shared_ptr<base::ActionUndo>> UnPop();

	protected:
		std::queue<std::shared_ptr<base::ActionUndo>> _history;
		std::queue< std::shared_ptr<base::ActionUndo>> _poppedHistory;
	};
}
