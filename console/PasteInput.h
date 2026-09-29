#pragma once

#include "PromptEditor.h"

namespace console
{
	// Bracketed paste is committed to the prompt only after its closing marker.
	// Controls are rejected while the complete paste remains consumed.
	class PasteInput
	{
	public:
		bool Active() const noexcept { return _active; }
		void Start()
		{
			_active = true;
			_invalid = false;
			_tooLong = false;
			_text.clear();
		}
		void Character(std::string_view text)
		{
			if (!_active) return;
			if (_text.size() + text.size() > MaxInputBytes) _tooLong = true;
			else _text.append(text);
		}
		void Control() noexcept { if (_active) _invalid = true; }
		InputResult Finish(PromptEditor& prompt)
		{
			_active = false;
			const auto result = _tooLong ? InputResult::TooLong
				: _invalid ? InputResult::Multiline : prompt.Insert(_text);
			_text.clear();
			return result;
		}
	private:
		std::string _text;
		bool _active = false;
		bool _invalid = false;
		bool _tooLong = false;
	};
}
