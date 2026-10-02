#pragma once

#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace console
{
	// Producers only enqueue. The app thread takes and executes submissions
	// after releasing this lock, so Scene replacement remains on one owner.
	class CommandMailbox
	{
	public:
		struct Submission
		{
			std::string Text;
			std::shared_ptr<std::promise<std::string>> Completion;
		};
		std::optional<std::future<std::string>> Submit(std::string text)
		{
			std::scoped_lock lock(_mutex);
			if (!_accepting || _pending.size() == Capacity || text.size() > MaxTextBytes)
				return std::nullopt;
			auto completion = std::make_shared<std::promise<std::string>>();
			auto future = completion->get_future();
			_pending.push_back({ std::move(text), std::move(completion) });
			return future;
		}
		std::optional<Submission> Take()
		{
			std::scoped_lock lock(_mutex);
			if (_pending.empty()) return std::nullopt;
			auto submission = std::move(_pending.front());
			_pending.pop_front();
			return submission;
		}
		void Close()
		{
			std::deque<Submission> discarded;
			{
				std::scoped_lock lock(_mutex);
				_accepting = false;
				discarded.swap(_pending);
			}
			for (auto& submission : discarded)
				submission.Completion->set_value("Console shutting down");
		}
		static constexpr std::size_t Capacity = 64;
		static constexpr std::size_t MaxTextBytes = 4096;
	private:
		std::mutex _mutex;
		std::deque<Submission> _pending;
		bool _accepting = true;
	};
}
