#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace base
{
	// Publishes immutable snapshots to audio readers without making them
	// touch a shared_ptr reference count. Writers must serialize store/exchange/collect.
	// load/store/exchange/collect are off-callback operations. The owner must stop
	// the audio reader before destroying this object.
	template<typename T>
	class PublishedAudioSnapshot
	{
	public:
		using Shared = std::shared_ptr<T>;

		class Lease
		{
		public:
			Lease() noexcept = default;
			Lease(const Lease&) = delete;
			Lease& operator=(const Lease&) = delete;

			Lease(Lease&& other) noexcept
				: _snapshot(other._snapshot), _value(other._value), _slot(other._slot),
				_readSucceeded(other._readSucceeded)
			{
				other._snapshot = nullptr;
				other._value = nullptr;
				other._readSucceeded = false;
			}

			Lease& operator=(Lease&& other) noexcept
			{
				if (this != &other)
				{
					reset();
					_snapshot = other._snapshot;
					_value = other._value;
					_slot = other._slot;
					_readSucceeded = other._readSucceeded;
					other._snapshot = nullptr;
					other._value = nullptr;
					other._readSucceeded = false;
				}
				return *this;
			}

			~Lease() { reset(); }

			explicit operator bool() const noexcept { return _value != nullptr; }
			// A successful null publication is empty; false indicates a bounded read failure.
			bool ReadSucceeded() const noexcept { return _readSucceeded; }
			T* get() const noexcept { return _value; }
			T* operator->() const noexcept { return _value; }
			T& operator*() const noexcept { return *_value; }
			void reset() noexcept
			{
				if (_snapshot)
					_snapshot->Release(_slot);
				_snapshot = nullptr;
				_value = nullptr;
				_readSucceeded = false;
			}

		private:
			friend class PublishedAudioSnapshot<T>;
			Lease(const PublishedAudioSnapshot* snapshot, T* value, unsigned int slot,
				bool readSucceeded) noexcept
				: _snapshot(snapshot), _value(value), _slot(slot), _readSucceeded(readSucceeded) {}

			const PublishedAudioSnapshot* _snapshot = nullptr;
			T* _value = nullptr;
			unsigned int _slot = 0u;
			bool _readSucceeded = false;
		};

		PublishedAudioSnapshot() = default;
		PublishedAudioSnapshot(const PublishedAudioSnapshot&) = delete;
		PublishedAudioSnapshot& operator=(const PublishedAudioSnapshot&) = delete;

		Shared load(std::memory_order order = std::memory_order_acquire) const
		{
			return _owner.load(order);
		}

		void store(Shared value, std::memory_order order = std::memory_order_release)
		{
			if (_writerOwner && (_writerOwner.get() != value.get()
				|| _writerOwner.owner_before(value) || value.owner_before(_writerOwner)))
				_retired.push_back(_writerOwner);
			_writerOwner = value;
			_owner.store(std::move(value), order);
			_published.store(_writerOwner.get(), std::memory_order_seq_cst);
			collect();
		}

		Shared exchange(Shared value, std::memory_order order = std::memory_order_acq_rel)
		{
			if (_writerOwner && (_writerOwner.get() != value.get()
				|| _writerOwner.owner_before(value) || value.owner_before(_writerOwner)))
				_retired.push_back(_writerOwner);
			_writerOwner = value;
			auto previous = _owner.exchange(std::move(value), order);
			_published.store(_writerOwner.get(), std::memory_order_seq_cst);
			collect();
			return previous;
		}

		// Call off audio, preferably from the writer or its job-thread pump. This
		// reclaims snapshots whose last audio borrow has ended even without a publish.
		// Returns outstanding retirements so a non-callback pump can keep running
		// after a cancelled publication even when no new value will be stored.
		std::size_t collect()
		{
			for (auto retired = _retired.begin(); retired != _retired.end();)
			{
				if (!IsProtected(retired->get()))
					retired = _retired.erase(retired);
				else
					++retired;
			}
			return _retired.size();
		}

		Lease borrow() const noexcept
		{
			// This SC order closes both races: if validation accepts the old
			// pointer, its hazard store precedes the writer's publish and scan;
			// if the writer's publish precedes validation, validation retries
			// without dereferencing the old pointer.
			for (unsigned int attempt = 0u; attempt < _MaxReadAttempts; ++attempt)
			{
				auto* value = _published.load(std::memory_order_seq_cst);
				if (!value)
				{
					if (!_published.load(std::memory_order_seq_cst))
						return Lease(nullptr, nullptr, 0u, true);
					continue;
				}

				unsigned int slot = 0u;
				bool expected = false;
				while (slot < _HazardSlots
					&& !_slotInUse[slot].compare_exchange_strong(expected, true,
						std::memory_order_acquire, std::memory_order_relaxed))
				{
					++slot;
					expected = false;
				}
				if (slot == _HazardSlots)
					return {};

				_hazards[slot].store(value, std::memory_order_seq_cst);
				if (value == _published.load(std::memory_order_seq_cst))
					return Lease(this, value, slot, true);
				Release(slot);
			}

			return {};
		}

	private:
		static constexpr unsigned int _HazardSlots = 8u;
		static constexpr unsigned int _MaxReadAttempts = 4u;
		static_assert(std::atomic<T*>::is_always_lock_free,
			"Audio snapshot pointer publication must be lock free");
		static_assert(std::atomic<bool>::is_always_lock_free,
			"Audio snapshot slot acquisition must be lock free");

		void Release(unsigned int slot) const noexcept
		{
			_hazards[slot].store(nullptr, std::memory_order_seq_cst);
			_slotInUse[slot].store(false, std::memory_order_release);
		}

		bool IsProtected(T* value) const noexcept
		{
			for (const auto& hazard : _hazards)
				if (hazard.load(std::memory_order_seq_cst) == value)
					return true;
			return false;
		}

		mutable std::array<std::atomic<T*>, _HazardSlots> _hazards{};
		mutable std::array<std::atomic<bool>, _HazardSlots> _slotInUse{};
		std::atomic<T*> _published{ nullptr };
		std::atomic<Shared> _owner;
		Shared _writerOwner;
		std::vector<Shared> _retired;
	};
}
