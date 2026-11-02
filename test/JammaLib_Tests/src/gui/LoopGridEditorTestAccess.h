#pragma once
#include "midi/LoopGridEditor.h"
namespace midi
{
	class LoopGridEditorTestAccess
	{
	public:
		static unsigned int DisplayedChannel(const LoopGridEditor& editor) { return static_cast<unsigned int>(editor._channelInput->Value()); }
		static utils::Position2d ButtonPoint(const LoopGridEditor& editor) { return editor._button->GlobalPosition() + utils::Position2d{10, 10}; }
		static bool HasCancelledButtonRelease(const LoopGridEditor& editor) { return editor._consumeButtonRelease; }
		static bool ButtonVisible(const LoopGridEditor& editor) { return editor._button->IsVisible(); }
		static const std::string& Feedback(const LoopGridEditor& editor) { return editor._feedbackText; }
		static std::size_t SelectionHistorySize(const LoopGridEditor& editor) { return editor._selectionHistory.size(); }
	};
}
