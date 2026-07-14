#include "ExternalTransport.h"

#include <iostream>

using namespace timing;

namespace
{
	// Local sample corresponding to remote interval position 0, clamped so we
	// never underflow the unsigned local timeline when joining mid-interval.
	unsigned long DeriveIntervalStart(unsigned long localAnchorSamps,
		unsigned int normalisedPos) noexcept
	{
		if (static_cast<unsigned long>(normalisedPos) >= localAnchorSamps)
			return 0ul;
		return localAnchorSamps - static_cast<unsigned long>(normalisedPos);
	}
}

ExternalTransport::ExternalTransport()
{
	_published.store(std::make_shared<const ExternalTransportState>(_staging),
		std::memory_order_release);
}

void ExternalTransport::Connect(unsigned long masterLoopLengthSamps)
{
	_staging = ExternalTransportState{};
	_staging.Mode = ExternalTransportMode::Connected;
	_staging.MasterLoopLengthSamps = masterLoopLengthSamps;
	_hasLastPos = false;
	_lastRemoteIntervalPos = 0u;
	_Publish("connect");
}

void ExternalTransport::Disconnect()
{
	// Discard runtime-only connected-sync state and return to free-running.
	_staging = ExternalTransportState{};
	_hasLastPos = false;
	_lastRemoteIntervalPos = 0u;
	_Publish("disconnect");
}

bool ExternalTransport::IsConnected() const noexcept
{
	return _staging.Mode == ExternalTransportMode::Connected;
}

void ExternalTransport::IngestSnapshot(const ExternalTransportSnapshot& snapshot)
{
	if (_staging.Mode != ExternalTransportMode::Connected)
		return;

	const auto length = snapshot.IntervalLengthSamps;
	const auto normalisedPos = (length > 0u)
		? (snapshot.IntervalPositionSamps % length)
		: 0u;

	bool wrapped = false;
	if (length > 0u)
	{
		if (_hasLastPos && normalisedPos < _lastRemoteIntervalPos)
		{
			++_staging.RemoteWrapCount;
			wrapped = true;
		}
		_lastRemoteIntervalPos = normalisedPos;
		_hasLastPos = true;
	}

	// Update the staging (back) state from the fresh snapshot.  Between wraps this
	// tracks continuous remote phase but is not yet published.
	_staging.LocalAnchorSamps = snapshot.LocalAnchorSamps;
	_staging.RemoteIntervalLengthSamps = length;
	_staging.RemoteIntervalPositionSamps = normalisedPos;
	if (length > 0u)
		_staging.MasterLoopLengthSamps = length;
	_staging.MasterLoopCount = _staging.RemoteWrapCount;
	_staging.AuthoritativeIntervalStartSamps =
		DeriveIntervalStart(snapshot.LocalAnchorSamps, normalisedPos);

	if (!wrapped)
		return;

	// Wrap boundary: commit any pending join alignment and zero the master loop.
	if (_staging.PendingAlignment.HasPending && !_staging.PendingAlignment.Committed)
	{
		_staging.PendingAlignment.Committed = true;
		_staging.HasCommittedAlignment = true;
		_staging.LastCommittedRemoteWrap = _staging.RemoteWrapCount;
		_staging.MasterLoopPlayPositionSamps = 0ul;
		// Clear the pending flag now the alignment has been committed.
		_staging.PendingAlignment.HasPending = false;
	}
	else
	{
		// Master loop tracks the remote interval start at every wrap.
		_staging.MasterLoopPlayPositionSamps = normalisedPos;
	}

	_Publish("wrap");
}

void ExternalTransport::BeginJoinAlignment(unsigned long localMasterPlayPositionSamps)
{
	if (_staging.Mode != ExternalTransportMode::Connected)
		return;

	const auto length = _staging.RemoteIntervalLengthSamps;
	const auto remotePhase = _staging.RemoteIntervalPositionSamps;
	const auto localPhase = (length > 0u)
		? (localMasterPlayPositionSamps % length)
		: localMasterPlayPositionSamps;

	PendingJoinAlignment alignment;
	alignment.HasPending = true;
	alignment.Committed = false;
	alignment.LocalMasterPositionAtJoin = localMasterPlayPositionSamps;
	alignment.RemoteIntervalPositionAtJoin = remotePhase;
	alignment.AlignmentDeltaSamps =
		static_cast<long long>(remotePhase) - static_cast<long long>(localPhase);

	_staging.PendingAlignment = alignment;

	// Pending alignment is staged only; it is not published until the next wrap.
	if (_diagnostics.load(std::memory_order_relaxed))
	{
		std::cout << "[XTransport] join-alignment staged"
			<< ": localMaster=" << localMasterPlayPositionSamps
			<< " remotePhase=" << remotePhase
			<< " delta=" << alignment.AlignmentDeltaSamps
			<< std::endl;
	}
}

std::shared_ptr<const ExternalTransportState> ExternalTransport::Published() const noexcept
{
	return _published.load(std::memory_order_acquire);
}

void ExternalTransport::SetDiagnosticsEnabled(bool enabled) noexcept
{
	_diagnostics.store(enabled, std::memory_order_relaxed);
}

void ExternalTransport::_Publish(const char* reason)
{
	// Publish an immutable copy of the staging state via an atomic pointer flip.
	// The staging state already equals the new front, so no post-publish clone is
	// required before the next set of snapshot-derived edits.
	_published.store(std::make_shared<const ExternalTransportState>(_staging),
		std::memory_order_release);

	if (_diagnostics.load(std::memory_order_relaxed))
	{
		std::cout << "[XTransport] publish(" << reason << ")"
			<< ": mode=" << (IsConnected() ? "connected" : "disconnected")
			<< " wrap=" << _staging.RemoteWrapCount
			<< " intervalPos=" << _staging.RemoteIntervalPositionSamps
			<< " intervalStart=" << _staging.AuthoritativeIntervalStartSamps
			<< " masterCount=" << _staging.MasterLoopCount
			<< " masterPos=" << _staging.MasterLoopPlayPositionSamps
			<< " pending=" << (_staging.PendingAlignment.HasPending ? 1 : 0)
			<< " committedWrap=" << _staging.LastCommittedRemoteWrap
			<< std::endl;
	}
}

unsigned long ExternalTransport::AbsoluteMasterSample(unsigned long masterLoopCount,
	unsigned long masterLoopLengthSamps,
	unsigned long masterLoopOffsetSamps) noexcept
{
	const auto abs = static_cast<unsigned long long>(masterLoopCount)
			* static_cast<unsigned long long>(masterLoopLengthSamps)
		+ static_cast<unsigned long long>(masterLoopOffsetSamps);
	return static_cast<unsigned long>(abs);
}

unsigned long ExternalTransport::AbsoluteMasterSample(const ExternalTransportState& state) noexcept
{
	return AbsoluteMasterSample(state.RemoteWrapCount,
		state.RemoteIntervalLengthSamps,
		state.RemoteIntervalPositionSamps);
}

unsigned long ExternalTransport::TakeAnchorSample(unsigned long absoluteMasterSample,
	unsigned long takePlayPosSamps,
	unsigned long takeLengthSamps) noexcept
{
	if (takeLengthSamps == 0ul)
		return absoluteMasterSample;

	const auto abs = static_cast<unsigned long long>(absoluteMasterSample);
	const auto pos = static_cast<unsigned long long>(takePlayPosSamps)
		% static_cast<unsigned long long>(takeLengthSamps);

	// The anchor is the master sample at which the take is at loop position 0.
	// Guard the (unlikely) case where the timeline has not yet advanced past the
	// take's current phase so the subtraction never underflows.
	const auto anchor = (abs >= pos)
		? (abs - pos)
		: (abs + static_cast<unsigned long long>(takeLengthSamps) - pos);
	return static_cast<unsigned long>(anchor);
}

unsigned long ExternalTransport::TakePositionFromAnchor(unsigned long absoluteMasterSample,
	unsigned long takeAnchorSample,
	unsigned long takeLengthSamps) noexcept
{
	if (takeLengthSamps == 0ul)
		return 0ul;

	const auto abs = static_cast<unsigned long long>(absoluteMasterSample);
	const auto anchor = static_cast<unsigned long long>(takeAnchorSample);
	const auto diff = (abs >= anchor) ? (abs - anchor) : 0ull;
	return static_cast<unsigned long>(diff % static_cast<unsigned long long>(takeLengthSamps));
}

unsigned long ExternalTransport::ApproachTakePosition(unsigned long currentPositionSamps,
	unsigned long targetPositionSamps,
	unsigned long takeLengthSamps,
	unsigned long maxAdjustmentSamps) noexcept
{
	if (takeLengthSamps == 0ul)
		return 0ul;

	const auto current = currentPositionSamps % takeLengthSamps;
	const auto target = targetPositionSamps % takeLengthSamps;
	const auto length = static_cast<long long>(takeLengthSamps);
	auto delta = static_cast<long long>(target) - static_cast<long long>(current);
	if (delta > length / 2)
		delta -= length;
	else if (delta < -(length / 2))
		delta += length;

	const auto maxAdjustment = static_cast<long long>(maxAdjustmentSamps);
	if (delta > maxAdjustment)
		delta = maxAdjustment;
	else if (delta < -maxAdjustment)
		delta = -maxAdjustment;

	const auto next = static_cast<long long>(current) + delta;
	return static_cast<unsigned long>((next + length) % length);
}

