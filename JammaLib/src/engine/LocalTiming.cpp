#include "LocalTiming.h"
#include "Quantiser.h"
#include "Station.h"
#include "../audio/AudioHost.h"

using namespace engine;

LocalTiming::Result LocalTiming::Advance(Quantiser& quantisation, audio::AudioHost& audio,
	const std::vector<std::shared_ptr<Station>>& callerLockedStations,std::chrono::steady_clock::time_point now)
{
    quantisation.ReconcileMasterMembership(callerLockedStations);
	if (quantisation.LocalMasterTimingPending())
	{
        const auto version=quantisation.PendingLocalMasterTimingVersion();
        const bool valid=quantisation.PendingMasterTargetValid(callerLockedStations);
        const bool expired=now-quantisation.PendingMasterStartedAt()>=PreparationDeadline;
        if ((!valid || expired) && audio.LastActivatedLocalMasterTimingVersion()<version)
        {
            // Lost cancellation means audio already claimed irreversible activation.
            // Poll its terminal receipt on a later pump; never restore over it.
            if (!audio.CancelLocalMasterTiming(version)) return {};
            for (const auto& station:callerLockedStations)
                if(station && !station->IsRemote()) for(const auto& take:station->GetLoopTakes())
                    if(take) take->CancelLocalMasterQuantisationOnOwner();
            quantisation.CancelLocalMasterTiming(version);
            quantisation.SetMasterFailure(expired ? "Master change timed out; previous timing restored" : "Master target unavailable; previous timing restored");
            return {Outcome::Cancelled,version};
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
							_preparingDelta) % length,
						quantisation.PendingMasterTapEdit() && take.get() == quantisation.PendingLocalMasterTake()
							? receipt->NewMasterLengthSamps : 0u,
						quantisation.ActiveGridDivisions());
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
		const auto version=quantisation.PendingLocalMasterTimingVersion();
        quantisation.CompleteLocalMasterTiming();
        quantisation.ReconcileMasterMembership(callerLockedStations);
        return {Outcome::Activated,version};
	}
    return {};
}
