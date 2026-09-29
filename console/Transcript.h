#pragma once

#include "TextCoordinates.h"
#include <deque>
#include <string>
#include <vector>

namespace console
{
	struct TranscriptRow
	{
		TextPosition Start;
		std::string Text;
	};

	// Owned by the UI thread. The anchor identifies text, so reflow does not
	// change the user's place when the terminal width changes.
	class Transcript
	{
	public:
		static constexpr std::size_t MaxBytes = 64ull * 1024 * 1024;
		static constexpr std::size_t MaxEntries = 100000;
		std::size_t Size() const noexcept { return _entries.size(); }
		std::size_t Bytes() const noexcept { return _bytes; }
		std::uint64_t FirstId() const noexcept { return _entries.empty() ? 0 : _entries.front().Id; }
		std::uint64_t LastId() const noexcept { return _entries.empty() ? 0 : _entries.back().Id; }
		const std::string* TextFor(std::uint64_t id) const noexcept
		{
			if (_entries.empty() || id < FirstId() || id > LastId()) return nullptr;
			return &_entries[static_cast<std::size_t>(id - FirstId())].Text;
		}
		bool Following() const noexcept { return _following; }
		TextPosition Anchor() const noexcept { return _anchor; }

		void Append(std::string_view text)
		{
			// Each protocol message is bounded, but split explicit line breaks so
			// every retained entry remains one logical copy/selection line.
			for (std::size_t begin = 0; begin <= text.size();)
			{
				const auto end = text.find('\n', begin);
				const auto length = (end == std::string_view::npos ? text.size() : end) - begin;
				_entries.push_back({ _nextId++, std::string(text.substr(begin, length)) });
				_bytes += length;
				while (_entries.size() > MaxEntries || _bytes > MaxBytes)
				{
					_bytes -= _entries.front().Text.size();
					_entries.pop_front();
				}
				if (!_entries.empty() && _anchor.EntryId < _entries.front().Id)
					_anchor = { _entries.front().Id, 0 };
				if (end == std::string_view::npos) break;
				begin = end + 1;
			}
		}

		std::vector<TranscriptRow> Visible(int width, int height)
		{
			std::vector<TranscriptRow> result;
			if (_entries.empty() || width <= 0 || height <= 0) return result;
			if (_following)
			{
				for (auto index = _entries.size(); index > 0 && result.size() < static_cast<std::size_t>(height); --index)
				{
					const auto& starts = Wrap(_entries[index - 1], width);
					for (auto at = starts.size(); at > 0 && result.size() < static_cast<std::size_t>(height); --at)
						result.push_back(Row(_entries[index - 1], starts, at - 1));
				}
				std::reverse(result.begin(), result.end());
				_anchor = result.front().Start;
				return result;
			}
			const auto first = _entries.front().Id;
			for (auto index = static_cast<std::size_t>(std::max(_anchor.EntryId, first) - first);
				index < _entries.size() && result.size() < static_cast<std::size_t>(height); ++index)
			{
				const auto& starts = Wrap(_entries[index], width);
				for (std::size_t at = 0; at < starts.size(); ++at)
				{
					if (index == static_cast<std::size_t>(std::max(_anchor.EntryId, first) - first)
						&& at + 1 < starts.size() && starts[at + 1] <= _anchor.Byte)
						continue;
					if (result.size() == static_cast<std::size_t>(height)) break;
					result.push_back(Row(_entries[index], starts, at));
				}
			}
			if (!result.empty()) _anchor = result.front().Start;
			return result;
		}

		void ScrollUp(int width, int rows)
		{
			if (_entries.empty() || width <= 0 || rows <= 0) return;
			if (!_anchor.EntryId) Visible(width, 1);
			if (_following) _following = false;
			for (; rows > 0; --rows)
			{
				const auto index = static_cast<std::size_t>(_anchor.EntryId - _entries.front().Id);
				if (index >= _entries.size()) break;
				const auto& starts = Wrap(_entries[index], width);
				auto it = std::upper_bound(starts.begin(), starts.end(), _anchor.Byte);
				if (it != starts.begin()) --it;
				if (it != starts.begin()) { _anchor.Byte = *(it - 1); continue; }
				if (!index) break;
				_anchor = { _entries[index - 1].Id, Wrap(_entries[index - 1], width).back() };
			}
		}

		void ScrollDown(int width, int rows)
		{
			if (_entries.empty() || width <= 0 || rows <= 0 || _following) return;
			if (!_anchor.EntryId) _anchor = { _entries.front().Id, 0 };
			for (; rows > 0; --rows)
			{
				const auto index = static_cast<std::size_t>(_anchor.EntryId - _entries.front().Id);
				if (index >= _entries.size()) break;
				const auto& starts = Wrap(_entries[index], width);
				auto it = std::upper_bound(starts.begin(), starts.end(), _anchor.Byte);
				if (it != starts.begin()) --it;
				if (it != starts.end() && it + 1 != starts.end()) { _anchor.Byte = *(it + 1); continue; }
				if (index + 1 == _entries.size()) break;
				_anchor = { _entries[index + 1].Id, 0 };
			}
		}
		void FollowTail() noexcept { _following = true; }
		void Pause() noexcept { _following = false; }
		std::vector<LogicalTextEntry> LogicalEntries() const
		{
			std::vector<LogicalTextEntry> result;
			result.reserve(_entries.size());
			for (const auto& entry : _entries) result.push_back({ entry.Id, entry.Text });
			return result;
		}

	private:
		struct Entry { std::uint64_t Id; std::string Text; };
		const std::vector<std::size_t>& Wrap(const Entry& entry, int width)
		{
			if (_cachedId == entry.Id && _cachedWidth == width) return _cachedStarts;
			_cachedId = entry.Id;
			_cachedWidth = width;
			_cachedStarts.clear();
			_cachedStarts.push_back(0);
			int columns = 0;
			for (const auto& cluster : Graphemes(entry.Text))
			{
				if (columns > 0 && columns + cluster.Columns > width)
				{
					_cachedStarts.push_back(cluster.Start);
					columns = 0;
				}
				columns += cluster.Columns;
			}
			return _cachedStarts;
		}
		static TranscriptRow Row(const Entry& entry, const std::vector<std::size_t>& starts,
			std::size_t index)
		{
			const auto begin = starts[index];
			const auto end = index + 1 < starts.size() ? starts[index + 1] : entry.Text.size();
			return { { entry.Id, begin }, entry.Text.substr(begin, end - begin) };
		}
		std::deque<Entry> _entries;
		std::size_t _bytes = 0;
		std::uint64_t _nextId = 1;
		TextPosition _anchor{};
		bool _following = true;
		std::uint64_t _cachedId = 0;
		int _cachedWidth = 0;
		std::vector<std::size_t> _cachedStarts;
	};
}
