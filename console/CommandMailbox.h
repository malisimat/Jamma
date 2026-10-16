#pragma once
#include <deque>
#include <optional>
#include <string>
#include <utility>
namespace console
{
	// UI-thread owned: window procedures enqueue, app loop executes using current Scene.
	class CommandMailbox
	{
	public:
		struct Submission { std::string Text; };
		bool Submit(std::string text)
		{
			if (!_accepting || _pending.size() == Capacity || text.size() > MaxTextBytes) return false;
			_pending.push_back({ std::move(text) }); return true;
		}
		std::optional<Submission> Take()
		{
			if (_pending.empty()) return std::nullopt;
			auto submission = std::move(_pending.front()); _pending.pop_front(); return submission;
		}
		void Close() { _accepting = false; _pending.clear(); }
		static constexpr std::size_t Capacity = 64;
		static constexpr std::size_t MaxTextBytes = 4096;
	private:
		std::deque<Submission> _pending;
		bool _accepting = true;
	};
}
