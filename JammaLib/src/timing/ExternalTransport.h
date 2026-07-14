#pragma once

#include <atomic>
#include <memory>

namespace timing
{
	// Authoritative NINJAM interval snapshot as it enters the transport layer.
	// Carries just enough remote data to project into local sample space.  All
	// fields are captured on the job thread when the snapshot becomes authoritative.
	struct ExternalTransportSnapshot
	{
		// NINJAM interval duration in samples (remote master cycle length).
		unsigned int IntervalLengthSamps = 0u;
		// Current position within the remote interval, in samples.
		unsigned int IntervalPositionSamps = 0u;
		// Remote session sample rate (e.g. 44100).
		unsigned int SampleRate = 0u;
		// Local audio-thread sample counter where this snapshot became authoritative.
		unsigned long LocalAnchorSamps = 0ul;
	};

	enum class ExternalTransportMode
	{
		Disconnected,
		Connected
	};

	// Mid-cycle join alignment.  Computed immediately on join but not applied to
	// the master loop until the next authoritative remote interval wrap.
	struct PendingJoinAlignment
	{
		bool HasPending = false;
		bool Committed = false;
		// Local master-loop play position observed at the moment of join.
		unsigned long LocalMasterPositionAtJoin = 0ul;
		// Remote interval position observed at the moment of join.
		unsigned int RemoteIntervalPositionAtJoin = 0u;
		// Signed phase delta (remote interval phase - local master phase) in samples.
		// Positive means the local master is behind the remote interval.
		long long AlignmentDeltaSamps = 0;
	};

	// Immutable, double-buffered transport state POCO.  One instance is the
	// published (front) state read by consumers; a second staging (back) instance
	// receives snapshot-derived updates during the current remote interval and is
	// published atomically at the next authoritative remote wrap.
	struct ExternalTransportState
	{
		ExternalTransportMode Mode = ExternalTransportMode::Disconnected;

		// Local audio-sample anchor for the authoritative remote snapshot.
		unsigned long LocalAnchorSamps = 0ul;

		// Authoritative remote interval length/position.
		unsigned int RemoteIntervalLengthSamps = 0u;
		unsigned int RemoteIntervalPositionSamps = 0u;

		// Unbounded remote interval count (increments once per authoritative wrap).
		unsigned long RemoteWrapCount = 0ul;

		// Authoritative master-loop shape derived from the remote interval.
		unsigned long MasterLoopLengthSamps = 0ul;
		unsigned long MasterLoopCount = 0ul;
		unsigned long MasterLoopPlayPositionSamps = 0ul;

		// Derived local sample where the current remote interval started
		// (LocalAnchor - normalised remote interval position).
		unsigned long AuthoritativeIntervalStartSamps = 0ul;

		// Pending/committed join-alignment bookkeeping.
		PendingJoinAlignment PendingAlignment;
		unsigned long LastCommittedRemoteWrap = 0ul;
		bool HasCommittedAlignment = false;
	};

	// ExternalTransport is a thin adaptor that promotes the connected NINJAM
	// interval to an authoritative external transport.  It owns runtime-only
	// connected-sync state (never persisted) and publishes an immutable
	// ExternalTransportState snapshot at each authoritative remote interval wrap.
	//
	// Threading contract:
	//  - IngestSnapshot / BeginJoinAlignment / Connect / Disconnect run on the
	//    job thread and mutate only the staging (back) state.
	//  - Published() is a lock-free read of the front state, safe from the audio
	//    thread.  Publication is an atomic shared-pointer flip (the "equivalent
	//    pointer flip" the plan permits) which keeps readers wait-free.
	class ExternalTransport
	{
	public:
		ExternalTransport();

		// Enter connected mode.  masterLoopLengthSamps seeds the master-loop shape
		// before the first snapshot arrives; it is overwritten once the remote
		// interval length is known.  Publishes the initial connected state.
		void Connect(unsigned long masterLoopLengthSamps = 0ul);

		// Return to free-running/disconnected mode and publish that fact.  Runtime
		// connected-sync state is discarded (never persisted).
		void Disconnect();

		bool IsConnected() const noexcept;

		// Ingest a fresh authoritative remote snapshot into the staging state.
		// Detects the authoritative interval wrap; on wrap it commits any pending
		// join alignment, advances the master-loop count, and publishes.
		void IngestSnapshot(const ExternalTransportSnapshot& snapshot);

		// Record a mid-cycle join alignment immediately (against the current remote
		// interval phase) without touching the master loop.  The alignment is
		// committed at the next authoritative remote wrap.
		void BeginJoinAlignment(unsigned long localMasterPlayPositionSamps);

		// Lock-free read of the currently published transport state.
		std::shared_ptr<const ExternalTransportState> Published() const noexcept;

		void SetDiagnosticsEnabled(bool enabled) noexcept;

		// --- Master-relative re-anchoring -----------------------------------------
		// A loop take never stores an absolute position against the master timeline;
		// it stores a master-relative anchor so that when the external transport
		// wraps (or is otherwise re-based) its play position can be re-derived to the
		// point it would have naturally reached, rather than snapping to zero.

		// Absolute position on the unbounded master timeline.
		static unsigned long AbsoluteMasterSample(unsigned long masterLoopCount,
			unsigned long masterLoopLengthSamps,
			unsigned long masterLoopOffsetSamps) noexcept;

		// Absolute master-timeline sample derived from the authoritative remote phase
		// carried by a transport state (wrapCount * intervalLen + intervalPos).
		static unsigned long AbsoluteMasterSample(const ExternalTransportState& state) noexcept;

		// Master-relative anchor for a take observed playing at takePlayPosSamps while
		// the master timeline is at absoluteMasterSample.  The anchor is the absolute
		// master sample at which the take sits at loop-relative position 0.
		static unsigned long TakeAnchorSample(unsigned long absoluteMasterSample,
			unsigned long takePlayPosSamps,
			unsigned long takeLengthSamps) noexcept;

		// Re-derive a take's loop-relative play position at absoluteMasterSample from
		// its stored anchor and its own loop length.
		static unsigned long TakePositionFromAnchor(unsigned long absoluteMasterSample,
			unsigned long takeAnchorSample,
			unsigned long takeLengthSamps) noexcept;

		// Move a take cursor toward its master-derived target through the shortest
		// modular path, capped at maxAdjustmentSamps for seamless phase discipline.
		static unsigned long ApproachTakePosition(unsigned long currentPositionSamps,
			unsigned long targetPositionSamps,
			unsigned long takeLengthSamps,
			unsigned long maxAdjustmentSamps) noexcept;

	private:
		void _Publish(const char* reason);

		// Staging (back) state.  Mutated only on the ingestion thread.
		ExternalTransportState _staging;
		// Published (front) state.  Atomic shared-pointer flip on wrap.
		std::atomic<std::shared_ptr<const ExternalTransportState>> _published;

		bool _hasLastPos = false;
		unsigned int _lastRemoteIntervalPos = 0u;
		std::atomic_bool _diagnostics{ false };
	};
}
