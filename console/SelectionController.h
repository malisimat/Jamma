#pragma once

#include "PromptEditor.h"
#include <optional>

namespace console
{
	inline constexpr std::string_view NoMouseHelp =
		"[CONSOLE] Mouse unavailable? Use host selection (Shift+drag) and keyboard editing.";
	enum class SelectionRegion { None, Transcript, Prompt };

	// One UI owner coordinates the two editable regions. Release retains the
	// selected bytes for Ctrl+C while ending pointer capture.
	class SelectionController
	{
	public:
		TextSelection Transcript;
		SelectionRegion Dragging = SelectionRegion::None;
		std::optional<std::size_t> PromptOrigin;

		void PressTranscript(TextPosition position, int x, int y, PromptEditor& prompt)
		{
			prompt.ClickByte(prompt.Caret());
			PromptOrigin.reset();
			Transcript.Press(position);
			_pressX = x;
			_pressY = y;
			Dragging = SelectionRegion::Transcript;
		}
		void PressPrompt(std::size_t byte, std::size_t origin, PromptEditor& prompt)
		{
			Transcript.Clear();
			prompt.ClickByte(byte);
			PromptOrigin = origin;
			Dragging = SelectionRegion::Prompt;
		}
		void DragTranscriptRow(TextPosition start, std::string_view text,
			int x, int y)
		{
			if (Dragging != SelectionRegion::Transcript) return;
			if (x == _pressX && y == _pressY)
			{
				Transcript.Drag(Transcript.Anchor);
				return;
			}
			const auto leading = RowPosition(start, text, x);
			if (leading > Transcript.Anchor)
			{
				Transcript.Drag({ start.EntryId, start.Byte + HoverEndByte(text, x) });
			}
			else Transcript.Drag(leading);
		}
		void DragPrompt(std::size_t byte, PromptEditor& prompt)
		{
			if (Dragging == SelectionRegion::Prompt) prompt.ClickByte(byte, true);
		}
		void Release() noexcept
		{
			Dragging = SelectionRegion::None;
			PromptOrigin.reset();
		}
	private:
		int _pressX = 0;
		int _pressY = 0;
	};
}
