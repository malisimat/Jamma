#pragma once

#include "TextCoordinates.h"
#include "Protocol.h"
#include <algorithm>
#include <optional>

namespace console
{
	enum class InputResult { Accepted, InvalidUtf8, Multiline, TooLong };

	// Single-owner prompt state. All offsets are UTF-8 grapheme boundaries.
	class PromptEditor
	{
	public:
		const std::string& Text() const noexcept { return _text; }
		std::size_t Caret() const noexcept { return _caret; }
		bool HasSelection() const noexcept { return _anchor && *_anchor != _caret; }

		InputResult Insert(std::string_view text)
		{
			if (!ValidUtf8(text)) return InputResult::InvalidUtf8;
			if (text.find_first_of("\r\n") != std::string_view::npos) return InputResult::Multiline;
			const auto [first, last] = SelectionRange();
			if (_text.size() - (last - first) + text.size() > MaxInputBytes)
				return InputResult::TooLong;
			_text.replace(first, last - first, text);
			_caret = first + text.size();
			if (!IsGraphemeBoundary(_text, _caret))
				_caret = NextGrapheme(_text, _caret);
			_anchor.reset();
			return InputResult::Accepted;
		}

		void MoveLeft(bool selecting = false)
		{
			BeginMove(selecting);
			_caret = PreviousGrapheme(_text, _caret);
		}
		void MoveRight(bool selecting = false)
		{
			BeginMove(selecting);
			_caret = NextGrapheme(_text, _caret);
		}
		void MoveHome(bool selecting = false)
		{
			BeginMove(selecting);
			_caret = 0;
		}
		void MoveEnd(bool selecting = false)
		{
			BeginMove(selecting);
			_caret = _text.size();
		}
		void ClickColumn(int column, bool selecting = false)
		{
			BeginMove(selecting);
			_caret = ColumnToByte(_text, column);
		}

		void Backspace()
		{
			if (HasSelection()) { EraseSelection(); return; }
			const auto first = PreviousGrapheme(_text, _caret);
			_text.erase(first, _caret - first);
			_caret = first;
		}
		void Delete()
		{
			if (HasSelection()) { EraseSelection(); return; }
			_text.erase(_caret, NextGrapheme(_text, _caret) - _caret);
		}
		void MoveWordLeft(bool selecting = false)
		{
			BeginMove(selecting);
			const auto clusters = Graphemes(_text);
			auto at = std::lower_bound(clusters.begin(), clusters.end(), _caret,
				[](const Grapheme& cluster, std::size_t byte) { return cluster.Start < byte; });
			while (at != clusters.begin() && _text[(at - 1)->Start] == ' ') --at;
			while (at != clusters.begin() && _text[(at - 1)->Start] != ' ') --at;
			_caret = at == clusters.end() ? _text.size() : at->Start;
		}
		void MoveWordRight(bool selecting = false)
		{
			BeginMove(selecting);
			const auto clusters = Graphemes(_text);
			auto at = std::lower_bound(clusters.begin(), clusters.end(), _caret,
				[](const Grapheme& cluster, std::size_t byte) { return cluster.Start < byte; });
			while (at != clusters.end() && _text[at->Start] != ' ') ++at;
			while (at != clusters.end() && _text[at->Start] == ' ') ++at;
			_caret = at == clusters.end() ? _text.size() : at->Start;
		}
		void DeleteWordLeft()
		{
			if (HasSelection()) { EraseSelection(); return; }
			const auto last = _caret;
			MoveWordLeft();
			_text.erase(_caret, last - _caret);
		}
		std::string Take()
		{
			auto result = std::move(_text);
			_text.clear();
			_caret = 0;
			_anchor.reset();
			return result;
		}

		std::string SelectedText() const
		{
			if (!HasSelection()) return {};
			const auto [first, last] = SelectionRange();
			return _text.substr(first, last - first);
		}

	private:
		void BeginMove(bool selecting)
		{
			if (selecting) { if (!_anchor) _anchor = _caret; }
			else _anchor.reset();
		}
		std::pair<std::size_t, std::size_t> SelectionRange() const noexcept
		{
			if (!_anchor) return { _caret, _caret };
			return { std::min(*_anchor, _caret), std::max(*_anchor, _caret) };
		}
		void EraseSelection()
		{
			const auto [first, last] = SelectionRange();
			_text.erase(first, last - first);
			_caret = first;
			_anchor.reset();
		}
		std::string _text;
		std::size_t _caret = 0;
		std::optional<std::size_t> _anchor;
	};
}
