#pragma once

#include <atomic>
#include <cstdint>
#include <limits>
#if defined(_DEBUG)
#include <cassert>
#include <functional>
#include <thread>
#endif

namespace engine
{
	// Coordinates the two semantic Trigger-input producer domains without
	// involving the audio thread. The UI thread is the sole writer of open/close
	// requests and of the UI acknowledgement. The Scene job thread is the sole
	// writer of the job acknowledgement. Both producer domains read the open
	// revision before publishing an input edge; the publication coordinator reads
	// the acknowledgements before requesting audio quiescence.
	//
	// RequestCloseFromUi must be called by the UI producer after its current
	// dispatch is complete. ObserveAndAcknowledgeClose must be called by the job
	// producer before dispatching a new job-tick batch. Those sequencing rules
	// make each acknowledgement a producer barrier: once acknowledged, no later
	// edge from that producer can be accepted for the closed revision.
	//
	// Every close request also receives a unique monotonic token. Acknowledgements
	// carry that token rather than the rig revision, so a delayed acknowledgement
	// from an earlier close cannot satisfy a later close after the same revision is
	// reopened. CloseForever is called by the UI producer after its current
	// dispatch; teardown waits for the job producer to acknowledge its unique token
	// before destroying queues or Triggers.
	class RigTriggerIngressGate
	{
	public:
		static constexpr std::uint64_t NoRevision = 0u;
		static constexpr std::uint64_t CloseForeverRevision =
			(std::numeric_limits<std::uint64_t>::max)();

		RigTriggerIngressGate() noexcept = default;
		RigTriggerIngressGate(const RigTriggerIngressGate&) = delete;
		RigTriggerIngressGate& operator=(const RigTriggerIngressGate&) = delete;

		// Opens a newly published revision. Publication of the matching immutable
		// input dispatch must happen-before this call.
		bool Open(std::uint64_t revision) noexcept
		{
			return _Open(revision);
		}

		// Reopens the still-accepted revision after a rejected/cancelled edit.
		// Audio cancellation acknowledgement must happen-before this call.
		bool Reopen(std::uint64_t acceptedRevision) noexcept
		{
			return _Open(acceptedRevision);
		}

		// UI-producer operation. The caller has completed its current dispatch, so
		// closing the open revision and publishing the UI acknowledgement is the UI
		// producer barrier for this request.
		bool RequestCloseFromUi(std::uint64_t revision) noexcept
		{
			_AssertProducer(_uiProducerThread);
			if (!_IsUsableRevision(revision) ||
				_closedForever.load(std::memory_order_acquire))
				return false;
			if (_openRevision.load(std::memory_order_acquire) == NoRevision &&
				_requestedClosedRevision.load(std::memory_order_acquire) == revision &&
				_requestedCloseToken.load(std::memory_order_acquire) != NoCloseToken)
				return true;
			if (!_TryClaimTransition())
				return false;

			auto expected = revision;
			if (!_openRevision.compare_exchange_strong(expected,
				NoRevision,
				std::memory_order_acq_rel,
				std::memory_order_acquire))
			{
				// Permit the UI producer to repeat the same close request without
				// allocating a new barrier token.
				const auto repeated = expected == NoRevision &&
					_requestedClosedRevision.load(std::memory_order_acquire) == revision;
				_ReleaseTransition();
				_RepairPermanentCloseIfNeeded();
				return repeated;
			}

			const auto closeToken = _AllocateCloseToken();
			_requestedClosedRevision.store(revision, std::memory_order_relaxed);
			_requestedCloseToken.store(closeToken, std::memory_order_release);
			_uiAcknowledgedCloseToken.store(closeToken, std::memory_order_release);
			if (_closedForever.load(std::memory_order_acquire))
			{
				_PublishPermanentClose();
				_ReleaseTransition();
				return false;
			}
			_ReleaseTransition();
			_RepairPermanentCloseIfNeeded();
			return !_closedForever.load(std::memory_order_acquire);
		}

		// Job-producer test/integration seam. Separating observation from
		// acknowledgement makes the possible cross-thread delay explicit and lets
		// callers prove that an old observation cannot acknowledge a newer close.
		std::uint64_t ObserveCloseTokenFromJob() const noexcept
		{
			_AssertProducer(_jobProducerThread);
			return _requestedCloseToken.load(std::memory_order_acquire);
		}

		bool AcknowledgeObservedCloseFromJob(std::uint64_t closeToken) noexcept
		{
			_AssertProducer(_jobProducerThread);
			if (closeToken == NoCloseToken)
				return false;

			_jobAcknowledgedCloseToken.store(closeToken, std::memory_order_release);
			return true;
		}

		// Job-producer operation. Call once at the top of each job tick, before any
		// MIDI/serial Trigger dispatch. Returns the observed rig revision, or
		// NoRevision when no closure is pending.
		std::uint64_t ObserveAndAcknowledgeClose() noexcept
		{
			const auto closeToken = ObserveCloseTokenFromJob();
			if (!AcknowledgeObservedCloseFromJob(closeToken))
				return NoRevision;

			return _requestedClosedRevision.load(std::memory_order_acquire);
		}

		bool TryAcceptUi(std::uint64_t revision) const noexcept
		{
			_AssertProducer(_uiProducerThread);
			return _TryAccept(revision);
		}

		bool TryAcceptJob(std::uint64_t revision) const noexcept
		{
			_AssertProducer(_jobProducerThread);
			return _TryAccept(revision);
		}

		bool UiAcknowledged(std::uint64_t revision) const noexcept
		{
			return _ProducerAcknowledged(revision, _uiAcknowledgedCloseToken);
		}

		bool JobAcknowledged(std::uint64_t revision) const noexcept
		{
			return _ProducerAcknowledged(revision, _jobAcknowledgedCloseToken);
		}

		bool ReadyForAudioQuiescence(std::uint64_t revision) const noexcept
		{
			if (!_IsUsableRevision(revision))
				return false;
			const auto closeToken = _requestedCloseToken.load(std::memory_order_acquire);
			if (closeToken == NoCloseToken ||
				_requestedClosedRevision.load(std::memory_order_acquire) != revision)
				return false;

			return _uiAcknowledgedCloseToken.load(std::memory_order_acquire) == closeToken &&
				_jobAcknowledgedCloseToken.load(std::memory_order_acquire) == closeToken &&
				_requestedCloseToken.load(std::memory_order_acquire) == closeToken;
		}

		// UI-producer teardown operation. Permanent closure cannot be reopened.
		void CloseForever() noexcept
		{
			_AssertProducer(_uiProducerThread);
			_closedForever.store(true, std::memory_order_release);
			_openRevision.store(NoRevision, std::memory_order_release);
			if (_requestedClosedRevision.load(std::memory_order_acquire) == CloseForeverRevision)
				return;
			if (_TryClaimTransition())
			{
				_PublishPermanentClose();
				_ReleaseTransition();
			}
		}

		bool UiAcknowledgedCloseForever() const noexcept
		{
			return _ProducerAcknowledged(CloseForeverRevision, _uiAcknowledgedCloseToken);
		}

		bool JobAcknowledgedCloseForever() const noexcept
		{
			return _ProducerAcknowledged(CloseForeverRevision, _jobAcknowledgedCloseToken);
		}

		bool ReadyForShutdown() const noexcept
		{
			return _closedForever.load(std::memory_order_acquire) &&
				UiAcknowledgedCloseForever() && JobAcknowledgedCloseForever() &&
				_requestedClosedRevision.load(std::memory_order_acquire) == CloseForeverRevision;
		}

		std::uint64_t OpenRevision() const noexcept
		{
			return _openRevision.load(std::memory_order_acquire);
		}

		std::uint64_t RequestedClosedRevision() const noexcept
		{
			return _requestedClosedRevision.load(std::memory_order_acquire);
		}

		std::uint64_t RequestedCloseToken() const noexcept
		{
			return _requestedCloseToken.load(std::memory_order_acquire);
		}

		bool IsClosedForever() const noexcept
		{
			return _closedForever.load(std::memory_order_acquire);
		}

	private:
		static constexpr std::uint64_t NoCloseToken = 0u;

		static constexpr bool _IsUsableRevision(std::uint64_t revision) noexcept
		{
			return revision != NoRevision && revision != CloseForeverRevision;
		}

		bool _ProducerAcknowledged(std::uint64_t revision,
			const std::atomic<std::uint64_t>& acknowledgement) const noexcept
		{
			const auto closeToken = _requestedCloseToken.load(std::memory_order_acquire);
			if (closeToken == NoCloseToken ||
				_requestedClosedRevision.load(std::memory_order_acquire) != revision)
				return false;

			return acknowledgement.load(std::memory_order_acquire) == closeToken &&
				_requestedCloseToken.load(std::memory_order_acquire) == closeToken;
		}

		bool _Open(std::uint64_t revision) noexcept
		{
			if (!_IsUsableRevision(revision) ||
				_closedForever.load(std::memory_order_acquire))
				return false;
			if (!_TryClaimTransition())
				return false;
			if (_closedForever.load(std::memory_order_acquire))
			{
				_PublishPermanentClose();
				_ReleaseTransition();
				return false;
			}

			// Keep ingress closed while the previous request/acknowledgements are
			// cleared. The final release-store is the publication point.
			_openRevision.store(NoRevision, std::memory_order_release);
			_requestedCloseToken.store(NoCloseToken, std::memory_order_release);
			_requestedClosedRevision.store(NoRevision, std::memory_order_relaxed);
			_uiAcknowledgedCloseToken.store(NoCloseToken, std::memory_order_release);
			_jobAcknowledgedCloseToken.store(NoCloseToken, std::memory_order_release);
			_openRevision.store(revision, std::memory_order_release);
			if (_closedForever.load(std::memory_order_acquire))
			{
				_PublishPermanentClose();
				_ReleaseTransition();
				return false;
			}
			_ReleaseTransition();
			_RepairPermanentCloseIfNeeded();
			return !_closedForever.load(std::memory_order_acquire);
		}

		std::uint64_t _AllocateCloseToken() noexcept
		{
			auto token = _nextCloseToken.fetch_add(1u, std::memory_order_relaxed);
			if (token == NoCloseToken)
				token = _nextCloseToken.fetch_add(1u, std::memory_order_relaxed);
			return token;
		}

		bool _TryClaimTransition() noexcept
		{
			auto expected = false;
			return _transitionClaimed.compare_exchange_strong(expected, true,
				std::memory_order_acq_rel, std::memory_order_acquire);
		}

		void _ReleaseTransition() noexcept
		{
			_transitionClaimed.store(false, std::memory_order_release);
		}

		void _PublishPermanentClose() noexcept
		{
			_openRevision.store(NoRevision, std::memory_order_release);
			const auto closeToken = _AllocateCloseToken();
			_requestedClosedRevision.store(CloseForeverRevision, std::memory_order_relaxed);
			_requestedCloseToken.store(closeToken, std::memory_order_release);
			_uiAcknowledgedCloseToken.store(closeToken, std::memory_order_release);
		}

		void _RepairPermanentCloseIfNeeded() noexcept
		{
			if (!_closedForever.load(std::memory_order_acquire) ||
				_requestedClosedRevision.load(std::memory_order_acquire) == CloseForeverRevision ||
				!_TryClaimTransition())
				return;
			_PublishPermanentClose();
			_ReleaseTransition();
		}

		bool _TryAccept(std::uint64_t revision) const noexcept
		{
			if (!_IsUsableRevision(revision) ||
				_closedForever.load(std::memory_order_acquire))
				return false;

			return _openRevision.load(std::memory_order_acquire) == revision;
		}

		static void _AssertProducer(std::atomic<std::uint64_t>& owner) noexcept
		{
#if defined(_DEBUG)
			const auto threadToken = static_cast<std::uint64_t>(
				std::hash<std::thread::id>{}(std::this_thread::get_id())) + 1u;
			auto expected = std::uint64_t{ 0u };
			owner.compare_exchange_strong(expected, threadToken,
				std::memory_order_relaxed, std::memory_order_relaxed);
			assert(owner.load(std::memory_order_relaxed) == threadToken);
#else
			(void)owner;
#endif
		}

		std::atomic<std::uint64_t> _openRevision{ NoRevision };
		std::atomic<std::uint64_t> _requestedClosedRevision{ NoRevision };
		std::atomic<std::uint64_t> _requestedCloseToken{ NoCloseToken };
		std::atomic<std::uint64_t> _uiAcknowledgedCloseToken{ NoCloseToken };
		std::atomic<std::uint64_t> _jobAcknowledgedCloseToken{ NoCloseToken };
		std::atomic<bool> _closedForever{ false };
		std::atomic<bool> _transitionClaimed{ false };
		std::atomic<std::uint64_t> _nextCloseToken{ 1u };
		mutable std::atomic<std::uint64_t> _uiProducerThread{ 0u };
		mutable std::atomic<std::uint64_t> _jobProducerThread{ 0u };
	};
}
