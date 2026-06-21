#pragma once

#include <memory>
#include "Actionable.h"

namespace base
{
	class ActionUndo;
	class ActionReceiver;

	class ActionSender :
		public virtual Actionable
	{
	public:
		ActionSender() :
			_receiver(std::shared_ptr<ActionReceiver>()) {}
		ActionSender(std::shared_ptr<ActionReceiver> receiver) :
			_receiver(receiver)	{}

	public:
		virtual ActionDirection Direction() const override { return ACTIONDIR_SEND;	}
		virtual bool Undo(std::shared_ptr<ActionUndo> undo) { return false; }
		virtual bool Redo(std::shared_ptr<ActionUndo> undo) { return false; }

		void SetReceiver(std::shared_ptr<ActionReceiver> receiver) { _receiver = receiver; }
		const std::shared_ptr<ActionReceiver> GetReceiver() { return _receiver; }

		std::shared_ptr<ActionSender> shared_from_this()
		{
			auto self = Actionable::shared_from_this();
			return self ? std::dynamic_pointer_cast<ActionSender>(self) : nullptr;
		}

	protected:
		std::shared_ptr<ActionReceiver> _receiver;
	};
}
