#include "LocalTiming.h"
#include "Quantiser.h"
#include "Station.h"
#include "../audio/AudioHost.h"

using namespace engine;

void LocalTiming::Advance(Quantiser& quantisation, audio::AudioHost& audio,
	const std::vector<std::shared_ptr<Station>>& callerLockedStations)
{
	if (quantisation.LocalMasterTimingPending())
	{
		const auto* target = quantisation.PendingLocalMasterTake();
		bool present = false;
		for (const auto& station : callerLockedStations)
			if (station && !station->IsRemote())
				for (const auto& take : station->GetLoopTakes())
					if (take.get() == target && !station->MembershipRemoving()) present = true;
		if (!present && audio.LastActivatedLocalMasterTimingVersion() <
			quantisation.PendingLocalMasterTimingVersion())
		{
			const auto version = quantisation.PendingLocalMasterTimingVersion();
			audio.CancelLocalMasterTiming(version);
			for (const auto& station : callerLockedStations)
				if (station && !station->IsRemote())
					for (const auto& take : station->GetLoopTakes())
						if (take) take->CancelLocalMasterQuantisationOnOwner();
			quantisation.CancelLocalMasterTiming(version);
		}
	}
	if (const auto receipt = audio.LastPreparedLocalMasterTiming();
		receipt && quantisation.LocalMasterTimingPending() &&
		receipt->Version == quantisation.PendingLocalMasterTimingVersion() &&
		(receipt->Version > _lastCommittedVersion ||
		(audio.LocalMasterPreparationRetryVersion() == receipt->Version &&
			audio.LastActivatedLocalMasterTimingVersion() < receipt->Version)))
	{
		const auto length = static_cast<std::uint64_t>(receipt->NewMasterLengthSamps);
		if (_preparingVersion != receipt->Version)
		{
			_preparingVersion = receipt->Version;
			_preparingDelta =
				(static_cast<std::uint64_t>(receipt->NewPhaseSamps) + length
					- receipt->OldAbsoluteSamplePos % length) % length;
		}
		bool prepared = true;
		for (const auto& station : callerLockedStations)
		{
			if (!station || station->IsRemote()) continue;
			for (const auto& take : station->GetLoopTakes())
				if (take)
					prepared &= take->PrepareLocalMasterQuantisation(
						quantisation.EffectiveSamps(),
						(take->MidiQuantisationTransportStartSamps() % length +
							_preparingDelta) % length);
		}
		if (prepared)
		{
			_lastCommittedVersion = receipt->Version;
			audio.CommitLocalMasterTiming(receipt->Version);
		}
	}
	if (quantisation.LocalMasterTimingPending() &&
		audio.LastActivatedLocalMasterTimingVersion() >=
			quantisation.PendingLocalMasterTimingVersion() &&
		quantisation.PendingLocalMasterTimingVersion() != 0u)
	{
		for (const auto& station : callerLockedStations)
			if (station && !station->IsRemote())
				for (const auto& take : station->GetLoopTakes())
					if (take)
						take->CompleteLocalMasterQuantisationOnOwner();
		quantisation.CompleteLocalMasterTiming();
	}
}
