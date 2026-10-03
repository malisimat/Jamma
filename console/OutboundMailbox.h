#pragma once

#include "Protocol.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <string>

namespace console
{
	// Non-audio producers enqueue bounded UTF-8 messages. One pipe writer drains
	// the queue; replaceable status never displaces ordered events or results.
	class OutboundMailbox
	{
	public:
		bool Publish(Message message)
		{
			if (message.Type != MessageType::Event) return false;
			if (!ValidMessage(message.Type, message.RequestId, message.Text)
				|| !ValidUtf8(message.Text)) return false;
			std::scoped_lock lock(_mutex);
			if (_closed) return false;
			if (!Fits(message.Text.size())) { ++_dropped; return false; }
			_latestEvent = message.Text;
			Push(std::move(message));
			_ready.notify_one();
			return true;
		}
		template<class Stopping>
		bool PublishResult(Message message, Stopping&& stopping)
		{
			if (message.Type != MessageType::CommandResult
				|| !ValidMessage(message.Type, message.RequestId, message.Text)
				|| !ValidUtf8(message.Text)) return false;
			std::unique_lock lock(_mutex);
			while (!_closed && !stopping())
			{
				while (!Fits(message.Text.size()))
				{
					auto event = std::find_if(_pending.begin(), _pending.end(),
						[](const Message& pending) { return pending.Type == MessageType::Event; });
					if (event == _pending.end()) break;
					_bytes -= event->Text.size();
					_pending.erase(event);
					++_dropped;
				}
				if (Fits(message.Text.size()))
				{
					_latestEvent = message.Text;
					Push(std::move(message));
					_ready.notify_one();
					return true;
				}
				_ready.wait_for(lock, std::chrono::milliseconds(50));
			}
			return false;
		}
		void SetStatus(std::string status)
		{
			if (status.size() > MaxInputBytes || !ValidUtf8(status)) return;
			std::scoped_lock lock(_mutex);
			if (_closed) return;
			_status = Message{ MessageType::StatusSnapshot, 0, std::move(status) };
			_ready.notify_one();
		}
		bool ReportLoss(std::size_t count)
		{
			std::scoped_lock lock(_mutex);
			if (_closed) return false;
			_dropped += count;
			_ready.notify_one();
			return true;
		}
		std::string LatestEvent() const
		{
			std::scoped_lock lock(_mutex);
			return _latestEvent;
		}
		std::optional<Message> Take(const std::atomic<bool>& stopping)
		{
			std::unique_lock lock(_mutex);
			_ready.wait_for(lock, std::chrono::milliseconds(100), [&] {
				return _closed || stopping.load(std::memory_order_acquire)
					|| _status.has_value() || !_pending.empty() || _dropped;
			});
			if (_closed || stopping.load(std::memory_order_acquire)) return std::nullopt;
			if (_status)
			{
				auto value = std::move(_status);
				_status.reset();
				return value;
			}
			if (_pending.empty())
			{
				if (!_dropped) return std::nullopt;
				auto notice = Message{ MessageType::Event, 0,
					"[CONSOLE] Lost " + std::to_string(_dropped) + " events" };
				_dropped = 0;
				return notice;
			}
			auto value = std::move(_pending.front());
			_bytes -= value.Text.size();
			_pending.pop_front();
			if (_dropped)
			{
				auto notice = Message{ MessageType::Event, 0,
					"[CONSOLE] Lost " + std::to_string(_dropped) + " events" };
				if (Fits(notice.Text.size()))
				{
					Push(std::move(notice));
					_dropped = 0;
				}
			}
			_ready.notify_all();
			return value;
		}
		void Close()
		{
			std::scoped_lock lock(_mutex);
			_closed = true;
			_pending.clear();
			_status.reset();
			_bytes = 0;
			_ready.notify_all();
		}
		static constexpr std::size_t Capacity = 512;
		static constexpr std::size_t MaxBytes = 1024 * 1024;
	private:
		bool Fits(std::size_t size, std::size_t count = 1) const noexcept
		{
			return _pending.size() + count <= Capacity && _bytes + size <= MaxBytes;
		}
		void Push(Message message)
		{
			_bytes += message.Text.size();
			_pending.push_back(std::move(message));
		}
		mutable std::mutex _mutex;
		std::condition_variable _ready;
		std::deque<Message> _pending;
		std::optional<Message> _status;
		std::string _latestEvent = "Ready";
		std::size_t _bytes = 0;
		std::size_t _dropped = 0;
		bool _closed = false;
	};
}
