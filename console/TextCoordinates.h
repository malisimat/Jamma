#pragma once

#include <utf8proc.h>
#include <ftxui/screen/string.hpp>
#include <algorithm>
#include <compare>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace console
{
	struct Grapheme
	{
		std::size_t Start;
		std::size_t End;
		int Columns;
	};

	// Called only for already-validated UTF-8. utf8proc supplies Unicode
	// grapheme breaks; one terminal cell cluster is never split by the editor.
	inline std::vector<Grapheme> Graphemes(std::string_view text)
	{
		std::vector<Grapheme> result;
		utf8proc_int32_t previous = -1;
		utf8proc_int32_t state = 0;
		for (std::size_t at = 0; at < text.size();)
		{
			utf8proc_int32_t codepoint = 0;
			const auto count = utf8proc_iterate(
				reinterpret_cast<const utf8proc_uint8_t*>(text.data() + at),
				static_cast<utf8proc_ssize_t>(text.size() - at), &codepoint);
			if (count <= 0) return {};
			const bool starts = previous < 0
				|| utf8proc_grapheme_break_stateful(previous, codepoint, &state);
			if (starts) result.push_back({ at, at + static_cast<std::size_t>(count), 0 });
			else
			{
				result.back().End = at + static_cast<std::size_t>(count);
			}
			previous = codepoint;
			at += static_cast<std::size_t>(count);
		}
		// Match the pinned FTXUI text renderer's cell count, including its
		// treatment of ZWJ emoji and regional indicators. Phase 3 wraps only
		// at these cluster boundaries before handing each row to FTXUI.
		for (auto& cluster : result)
			cluster.Columns = ftxui::string_width(
				std::string(text.substr(cluster.Start, cluster.End - cluster.Start)));
		return result;
	}

	inline std::size_t ColumnToByte(std::string_view text, int column)
	{
		const auto clusters = Graphemes(text);
		int x = 0;
		for (const auto& cluster : clusters)
		{
			const auto width = cluster.Columns;
			if (width == 0) continue;
			if (column < x + width)
				return column - x < (width + 1) / 2 ? cluster.Start : cluster.End;
			x += width;
		}
		return text.size();
	}

	inline std::string ClipColumns(std::string_view text, int columns)
	{
		if (columns <= 0) return {};
		std::size_t end = 0;
		int used = 0;
		for (const auto& cluster : Graphemes(text))
		{
			if (used + cluster.Columns > columns) break;
			used += cluster.Columns;
			end = cluster.End;
		}
		return std::string(text.substr(0, end));
	}

	inline std::string StatusForWidth(std::string_view status, int columns)
	{
		if (columns <= 0) return {};
		if (ftxui::string_width(std::string(status)) <= columns)
			return std::string(status);
		constexpr std::string_view divider = " | last: ";
		const auto split = status.find(divider);
		if (split == std::string_view::npos) return ClipColumns(status, columns);
		const bool connected = status.substr(0, split) == "NINJAM connected";
		const std::string_view state = connected ? "C" : "D";
		if (columns == 1) return std::string(state);
		return std::string(state) + "|" + ClipColumns(status.substr(split + divider.size()), columns - 2);
	}

	inline std::size_t PromptWindowStart(std::string_view text, std::size_t caret, int columns)
	{
		std::size_t begin = caret;
		int used = 0;
		const auto clusters = Graphemes(text.substr(0, caret));
		for (auto it = clusters.rbegin(); it != clusters.rend(); ++it)
		{
			if (used + it->Columns > columns) break;
			used += it->Columns;
			begin = it->Start;
		}
		return begin;
	}

	inline std::size_t NextGrapheme(std::string_view text, std::size_t byte);

	inline std::size_t PromptColumnToByte(std::string_view text, std::size_t caret,
		int width, int column)
	{
		const int label = width >= 3 ? 2 : width == 2 ? 1 : 0;
		const int available = std::max(0, width - label);
		const auto next = NextGrapheme(text, caret);
		const auto atCaret = caret < text.size() ? text.substr(caret, next - caret) : " ";
		const int actualCaretColumns = std::max(1, ftxui::string_width(std::string(atCaret)));
		const int caretColumns = actualCaretColumns > available ? 1 : actualCaretColumns;
		const auto begin = PromptWindowStart(text, caret, std::max(0, available - caretColumns));
		const auto prefix = text.substr(begin, caret - begin);
		const auto prefixColumns = ftxui::string_width(std::string(prefix));
		const auto local = std::max(0, column - label);
		if (local < prefixColumns) return begin + ColumnToByte(prefix, local);
		if (local < prefixColumns + caretColumns)
		{
			if (actualCaretColumns > available) return caret;
			return caret + ColumnToByte(atCaret, local - prefixColumns);
		}
		const auto suffix = ClipColumns(text.substr(next),
			std::max(0, available - prefixColumns - caretColumns));
		return next + ColumnToByte(suffix, local - prefixColumns - caretColumns);
	}

	inline std::size_t PromptColumnToByteAtOrigin(std::string_view text,
		std::size_t origin, int width, int column)
	{
		const int label = width >= 3 ? 2 : width == 2 ? 1 : 0;
		const auto visible = ClipColumns(text.substr(origin), std::max(0, width - label));
		return origin + ColumnToByte(visible, std::max(0, column - label));
	}

	inline std::size_t PreviousGrapheme(std::string_view text, std::size_t byte)
	{
		std::size_t previous = 0;
		for (const auto& cluster : Graphemes(text))
		{
			if (cluster.End >= byte) return cluster.Start;
			previous = cluster.End;
		}
		return previous;
	}

	inline std::size_t NextGrapheme(std::string_view text, std::size_t byte)
	{
		for (const auto& cluster : Graphemes(text))
			if (cluster.End > byte) return cluster.End;
		return text.size();
	}

	inline int EdgeScrollDelta(int row, int viewportRows) noexcept
	{
		if (viewportRows <= 0) return 0;
		if (row < 0) return -1;
		if (row >= viewportRows) return 1;
		return 0;
	}

	inline bool IsGraphemeBoundary(std::string_view text, std::size_t byte)
	{
		if (byte == 0 || byte == text.size()) return true;
		for (const auto& cluster : Graphemes(text))
			if (cluster.Start == byte) return true;
		return false;
	}

	struct LogicalTextEntry
	{
		// IDs increase with transcript order and are never reused. Text is
		// immutable while an entry is retained.
		std::uint64_t Id;
		std::string_view Text;
	};

	struct TextPosition
	{
		std::uint64_t EntryId = 0;
		std::size_t Byte = 0;
		auto operator<=>(const TextPosition&) const = default;
	};

	inline TextPosition RowPosition(TextPosition start, std::string_view text, int column)
	{
		return { start.EntryId, start.Byte + ColumnToByte(text, column) };
	}

	inline std::size_t HoverEndByte(std::string_view text, int column)
	{
		int at = 0;
		for (const auto& cluster : Graphemes(text))
		{
			at += cluster.Columns;
			if (column < at) return cluster.End;
		}
		return text.size();
	}

	struct TextSelection
	{
		TextPosition Anchor;
		TextPosition Focus;
		bool Active = false;

		void Press(TextPosition position) noexcept
		{
			Anchor = Focus = position;
			Active = true;
		}
		void Drag(TextPosition position) noexcept { if (Active) Focus = position; }
		void Clear() noexcept { Active = false; }
		void ReconcileBounds(std::uint64_t firstId, std::uint64_t lastId) noexcept
		{
			if (Active && (Anchor.EntryId < firstId || Anchor.EntryId > lastId
				|| Focus.EntryId < firstId || Focus.EntryId > lastId)) Clear();
		}
		std::optional<std::pair<std::size_t, std::size_t>> RowRange(
			TextPosition start, std::size_t bytes) const noexcept
		{
			if (!Active) return std::nullopt;
			const auto first = std::min(Anchor, Focus);
			const auto last = std::max(Anchor, Focus);
			const TextPosition end{ start.EntryId, start.Byte + bytes };
			if (last <= start || first >= end) return std::nullopt;
			const auto beginByte = std::max(first, start).Byte - start.Byte;
			const auto endByte = std::min(last, end).Byte - start.Byte;
			if (beginByte >= endByte) return std::nullopt;
			return std::pair{ beginByte, endByte };
		}

		void Reconcile(std::span<const LogicalTextEntry> entries)
		{
			if (!Active) return;
			const auto exists = [&](std::uint64_t id) {
				return std::any_of(entries.begin(), entries.end(),
					[id](const auto& entry) { return entry.Id == id; });
			};
			if (!exists(Anchor.EntryId) || !exists(Focus.EntryId)) Clear();
		}

		std::string Copy(std::span<const LogicalTextEntry> entries) const
		{
			if (!Active || entries.empty()) return {};
			const auto begin = std::min(Anchor, Focus);
			const auto end = std::max(Anchor, Focus);
			const auto indexOf = [&](std::uint64_t id) {
				for (std::size_t i = 0; i < entries.size(); ++i)
					if (entries[i].Id == id) return i;
				return entries.size();
			};
			const auto firstIndex = indexOf(begin.EntryId);
			const auto lastIndex = indexOf(end.EntryId);
			if (firstIndex >= entries.size() || lastIndex >= entries.size()
				|| firstIndex > lastIndex
				|| !IsGraphemeBoundary(entries[firstIndex].Text, begin.Byte)
				|| !IsGraphemeBoundary(entries[lastIndex].Text, end.Byte)) return {};
			std::string copy;
			for (auto index = firstIndex; index <= lastIndex; ++index)
			{
				if (index > firstIndex) copy += '\n';
				const auto first = index == firstIndex ? begin.Byte : 0;
				const auto last = index == lastIndex ? end.Byte : entries[index].Text.size();
				if (first > last || last > entries[index].Text.size()) return {};
				copy.append(entries[index].Text.substr(first, last - first));
			}
			return copy;
		}

		template<class Writer>
		bool CopyToClipboard(std::span<const LogicalTextEntry> entries, Writer&& writer) const
		{
			const auto value = Copy(entries);
			return !value.empty() && writer(value);
		}
	};
}
