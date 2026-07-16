#include "ExternalTransport.h"

#include <iostream>

using namespace timing;

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
	_observedIntervalLength = 0u;
	_generation = 0u;
	_snapshotSequence.store(0u, std::memory_order_relaxed);
	_generationPrimeCount.store(0u, std::memory_order_relaxed);
	_acceptedWrapCount.store(0u, std::memory_order_relaxed);
	_rejectedWrapCandidateCount.store(0u, std::memory_order_relaxed);
	_duplicateSnapshotCount.store(0u, std::memory_order_relaxed);
	_Publish("connect");
}

void ExternalTransport::Disconnect()
{
	// Discard runtime-only connected-sync state and return to free-running.
	_staging = ExternalTransportState{};
	_hasLastPos = false;
	_lastRemoteIntervalPos = 0u;
	_observedIntervalLength = 0u;
	_generation = 0u;
	_Publish("disconnect");
}

bool ExternalTransport::IsConnected() const noexcept
{
	return _staging.Mode == ExternalTransportMode::Connected;
}

std::optional<RemoteTransportWrap> ExternalTransport::IngestSnapshot(
	const ExternalTransportSnapshot& snapshot)
{
	if (_staging.Mode != ExternalTransportMode::Connected)
		return std::nullopt;

	_snapshotSequence.fetch_add(1u, std::memory_order_relaxed);
	const auto length = snapshot.IntervalLengthSamps;
	const auto normalisedPos = (length > 0u)
		? (snapshot.IntervalPositionSamps % length)
		: 0u;
	if (length == 0u)
	{
		_hasLastPos = false;
		_observedIntervalLength = 0u;
		_LogSnapshot(snapshot, normalisedPos, _lastRemoteIntervalPos, false, false, "zero-length");
		return std::nullopt;
	}

	if (length != _observedIntervalLength)
	{
		const auto previousPosition = _lastRemoteIntervalPos;
		_observedIntervalLength = length;
		++_generation;
		_generationPrimeCount.fetch_add(1u, std::memory_order_relaxed);
		_hasLastPos = true;
		_lastRemoteIntervalPos = normalisedPos;
		_staging.PendingAlignment = PendingJoinAlignment{};
		_staging.HasCommittedAlignment = false;
		_staging.LocalAnchorSamps = snapshot.LocalAnchorSamps;
		_staging.RemoteIntervalLengthSamps = length;
		_staging.RemoteIntervalPositionSamps = normalisedPos;
		_staging.MasterLoopLengthSamps = length;
		_staging.AuthoritativeIntervalStartSamps =
			_DeriveIntervalStart(snapshot.LocalAnchorSamps, normalisedPos);
		_LogSnapshot(snapshot, normalisedPos, previousPosition, false, false, "generation-prime");
		return std::nullopt;
	}

	if (_hasLastPos && normalisedPos == _lastRemoteIntervalPos)
	{
		_duplicateSnapshotCount.fetch_add(1u, std::memory_order_relaxed);
		_LogSnapshot(snapshot, normalisedPos, _lastRemoteIntervalPos, false, false, "duplicate");
		return std::nullopt;
	}

	const auto previousPosition = _lastRemoteIntervalPos;
	const auto wrapCandidate = _hasLastPos && normalisedPos < _lastRemoteIntervalPos;
	bool wrapped = false;
	if (wrapCandidate)
	{
		const auto endWindowStart = length - (length / 4u);
		const auto startWindowEnd = length / 4u;
		if (_lastRemoteIntervalPos >= endWindowStart && normalisedPos <= startWindowEnd)
		{
			++_staging.RemoteWrapCount;
			_acceptedWrapCount.fetch_add(1u, std::memory_order_relaxed);
			wrapped = true;
		}
		else
			_rejectedWrapCandidateCount.fetch_add(1u, std::memory_order_relaxed);
	}
	_lastRemoteIntervalPos = normalisedPos;
	_hasLastPos = true;

	// Update the staging (back) state from the fresh snapshot.  Between wraps this
	// tracks continuous remote phase but is not yet published.
	_staging.LocalAnchorSamps = snapshot.LocalAnchorSamps;
	_staging.RemoteIntervalLengthSamps = length;
	_staging.RemoteIntervalPositionSamps = normalisedPos;
	if (length > 0u)
		_staging.MasterLoopLengthSamps = length;
	_staging.MasterLoopCount = _staging.RemoteWrapCount;
	_staging.AuthoritativeIntervalStartSamps =
		_DeriveIntervalStart(snapshot.LocalAnchorSamps, normalisedPos);

	if (!wrapped)
	{
		_LogSnapshot(snapshot,
			normalisedPos,
			previousPosition,
			wrapCandidate,
			false,
			wrapCandidate ? "backward-away-boundary" : "advance");
		return std::nullopt;
	}

	// Wrap boundary: commit any pending join alignment and zero the master loop.
	const auto isJoin = _staging.PendingAlignment.HasPending
		&& !_staging.PendingAlignment.Committed
		&& _staging.PendingAlignment.Generation == _generation;
	const auto joinDelta = isJoin ? _staging.PendingAlignment.AlignmentDeltaSamps : 0;
	if (isJoin)
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
	_LogSnapshot(snapshot, normalisedPos, previousPosition, true, true, "wrap");
	return RemoteTransportWrap{
		_generation,
		_staging.RemoteWrapCount,
		length,
		normalisedPos,
		joinDelta,
		isJoin
	};
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
	alignment.AlignmentDeltaSamps = SignedCircularDifference(
		static_cast<unsigned int>(localPhase), remotePhase, length);
	alignment.Generation = _generation;

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

ExternalTransportDiagnostics ExternalTransport::Diagnostics() const noexcept
{
	return {
		_snapshotSequence.load(std::memory_order_relaxed),
		_generationPrimeCount.load(std::memory_order_relaxed),
		_acceptedWrapCount.load(std::memory_order_relaxed),
		_rejectedWrapCandidateCount.load(std::memory_order_relaxed),
		_duplicateSnapshotCount.load(std::memory_order_relaxed)
	};
}

void ExternalTransport::_LogSnapshot(const ExternalTransportSnapshot& snapshot,
	unsigned int normalisedPos,
	unsigned int previousPosition,
	bool wrapCandidate,
	bool wrapAccepted,
	const char* reason) const
{
	if (!_diagnostics.load(std::memory_order_relaxed))
		return;
	const auto localPhase = snapshot.IntervalLengthSamps > 0u
		? snapshot.LocalAnchorSamps % snapshot.IntervalLengthSamps
		: 0ul;
	std::cout << "[XTransportSnapshot] generation=" << _generation
		<< " sequence=" << _snapshotSequence.load(std::memory_order_relaxed)
		<< " intervalLength=" << snapshot.IntervalLengthSamps
		<< " remotePosition=" << normalisedPos
		<< " localAbsoluteSample=" << snapshot.LocalAnchorSamps
		<< " localPhase=" << localPhase
		<< " previousRemotePosition=" << previousPosition
		<< " wrapCandidate=" << (wrapCandidate ? 1 : 0)
		<< " wrapAccepted=" << (wrapAccepted ? 1 : 0)
		<< " reason=" << reason
		<< std::endl;
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

long long ExternalTransport::SignedCircularDifference(unsigned int currentOffset,
	unsigned int targetOffset,
	unsigned int intervalLength) noexcept
{
	if (intervalLength == 0u)
		return 0;
	const auto current = currentOffset % intervalLength;
	const auto target = targetOffset % intervalLength;
	const auto length = static_cast<long long>(intervalLength);
	auto delta = static_cast<long long>(target) - static_cast<long long>(current);
	if (delta > length / 2)
		delta -= length;
	else if (delta < -(length / 2))
		delta += length;
	else if ((intervalLength % 2u) == 0u && delta == -(length / 2))
		delta = length / 2;
	return delta;
}

unsigned long ExternalTransport::_DeriveIntervalStart(unsigned long localAnchorSamps,
	unsigned int normalisedPos) noexcept
{
	if (static_cast<unsigned long>(normalisedPos) >= localAnchorSamps)
		return 0ul;
	return localAnchorSamps - static_cast<unsigned long>(normalisedPos);
}
