#include "Quantisation.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include "Loop.h"
#include "LoopTake.h"
#include "Station.h"
#include "../ninjam/NinjamConnection.h"
#include "../ninjam/NinjamSession.h"
#include "../io/UserConfig.h"
#include "../midi/MidiQuantisation.h"

using namespace engine;

unsigned int Quantisation::_ClampToUInt(unsigned long value)
{
	return value > std::numeric_limits<unsigned int>::max() ?
		std::numeric_limits<unsigned int>::max() :
		static_cast<unsigned int>(value);
}

unsigned int Quantisation::_RoundedToUInt(double value)
{
	if (value <= 0.0)
		return 0u;

	if (value >= static_cast<double>(std::numeric_limits<unsigned int>::max()))
		return std::numeric_limits<unsigned int>::max();

	return static_cast<unsigned int>(value + 0.5);
}

unsigned long Quantisation::_SnapSeedToMasterDivisor(unsigned long requestedSeedSamps,
	unsigned long masterLoopSamps)
{
	if (masterLoopSamps == 0ul)
		return requestedSeedSamps;

	if (requestedSeedSamps == 0ul)
		requestedSeedSamps = masterLoopSamps;

	unsigned long bestSeed = 0ul;
	unsigned long bestDistance = std::numeric_limits<unsigned long>::max();

	for (unsigned long d = 1ul; d * d <= masterLoopSamps; ++d)
	{
		if ((masterLoopSamps % d) != 0ul)
			continue;

		const unsigned long candidates[2] = { masterLoopSamps / d, d };
		for (auto seed : candidates)
		{
			const auto distance = seed > requestedSeedSamps ?
				seed - requestedSeedSamps :
				requestedSeedSamps - seed;

			if (distance < bestDistance)
			{
				bestDistance = distance;
				bestSeed = seed;
			}
		}
	}

	if (bestSeed == 0ul)
		bestSeed = requestedSeedSamps;

	return bestSeed;
}

std::optional<QuantisationTiming> Quantisation::_TimingFromSeed(unsigned int seedSamps,
	unsigned long masterLoopSamps,
	unsigned int sampleRate)
{
	if ((seedSamps == 0u) || (sampleRate == 0u))
		return std::nullopt;

	if (masterLoopSamps == 0ul)
		masterLoopSamps = seedSamps;

	const auto seedCount = std::max(1ul, masterLoopSamps / seedSamps);
	if (seedCount > std::numeric_limits<unsigned int>::max())
		return std::nullopt;

	QuantisationTiming timing;
	timing.SeedSamps = seedSamps;
	timing.MasterLoopSamps = _ClampToUInt(masterLoopSamps);
	timing.SeedCount = static_cast<unsigned int>(seedCount);
	timing.Bpm = (60.0f * static_cast<float>(sampleRate)) / static_cast<float>(seedSamps);
	timing.Bpi = timing.SeedCount;
	return timing;
}

void Quantisation::SetClock(std::shared_ptr<Timer> clock)
{
	_clock = std::move(clock);
}

void Quantisation::SetSeedUsesPowers(bool seedUsesPowers) noexcept
{
	_seedUsesPowers = seedUsesPowers;
}

void Quantisation::Set(unsigned int samps, Timer::QuantisationType type)
{
	if (_clock)
		_clock->SetQuantisation(samps, type);
	_effectiveQuantiseSamps.store(samps, std::memory_order_release);
}

void Quantisation::Clear(bool clearTapTempo)
{
	if (_clock)
		_clock->Clear();
	_masterLoop.reset();
	_masterLoopLengthSamps.store(0ul, std::memory_order_release);
	_effectiveQuantiseSamps.store(0u, std::memory_order_release);
	_hasPendingTempo.store(false, std::memory_order_release);
	_armReclock.store(false, std::memory_order_release);
	_remoteMasterLoopSamps = 0u;
	_remoteSampleRate = 0u;
	_lastRemoteIntervalPos = 0u;

	if (clearTapTempo)
	{
		std::scoped_lock tapTempoLock(_tapTempoMutex);
		_tapTempo.Clear();
	}
}

void Quantisation::ArmReclock()
{
	if (_clock)
		_clock->Clear();
	_masterLoop.reset();
	_masterLoopLengthSamps.store(0ul, std::memory_order_release);
	{
		std::scoped_lock tapTempoLock(_tapTempoMutex);
		_tapTempo.Clear();
	}
	_armReclock.store(true, std::memory_order_release);
	_effectiveQuantiseSamps.store(0u, std::memory_order_release);
	_hasPendingTempo.store(false, std::memory_order_release);
}

void Quantisation::ApplyTiming(const QuantisationTiming& timing, const char* source)
{
	if (!_clock || (timing.SeedSamps == 0u))
		return;

	_clock->SetQuantisation(timing.SeedSamps,
		_seedUsesPowers ? Timer::QUANTISE_POWER : Timer::QUANTISE_MULTIPLE);
	_clock->SetSeedSourceLength(timing.MasterLoopSamps);

	_effectiveQuantiseSamps.store(timing.SeedSamps, std::memory_order_release);
	_hasPendingTempo.store((timing.Bpm > 0.0f) && (timing.Bpi > 0u), std::memory_order_release);
	_armReclock.store(false, std::memory_order_release);

	std::cout << "Quantisation " << source
		<< ": seed=" << timing.SeedSamps
		<< " master=" << timing.MasterLoopSamps
		<< " seeds=" << timing.SeedCount
		<< " bpm=" << timing.Bpm
		<< " bpi=" << timing.Bpi << std::endl;
}

void Quantisation::SetMidiGrain(unsigned int grainSamps,
	const char* source,
	const std::vector<std::shared_ptr<Station>>& stations)
{
	unsigned int takeCount = 0u;
	for (const auto& station : stations)
	{
		if (!station)
			continue;

		for (const auto& take : station->GetLoopTakes())
		{
			if (!take)
				continue;

			midi::MidiQuantisationSettings settings = take->MidiQuantisation();
			if (settings.GrainSamps != grainSamps)
			{
				settings.GrainSamps = grainSamps;
				take->SetMidiQuantisation(settings);
			}
			++takeCount;
		}
	}

	(void)source;
	(void)takeCount;
}

bool Quantisation::HandleTapTempo(std::uint64_t estimatedSampleAt,
	unsigned int sampleRate,
	const std::vector<std::shared_ptr<Station>>& stations,
	const io::UserConfig& cfg)
{
	std::optional<QuantisationTiming> timing;
	{
		std::scoped_lock tapTempoLock(_tapTempoMutex);
		const auto masterLoopLengthSamps = _masterLoopLengthSamps.load(std::memory_order_acquire);
		if (masterLoopLengthSamps == 0ul)
		{
			std::cout << "Tap tempo: no master loop, tap ignored" << std::endl;
			return true;
		}

		timing = _tapTempo.TapAtSample(estimatedSampleAt,
			sampleRate,
			masterLoopLengthSamps,
			Policy(cfg));
	}

	if (!timing.has_value())
	{
		std::cout << "Tap tempo: first tap" << std::endl;
		return true;
	}

	const auto quantisation = cfg.Loop.SeedUsesPowers ? Timer::QUANTISE_POWER : Timer::QUANTISE_MULTIPLE;
	if (_clock)
		_clock->SetQuantisation(timing->SeedSamps, quantisation);
	SetSeedUsesPowers(cfg.Loop.SeedUsesPowers);
	ApplyTiming(timing.value(), "tap tempo");
	SetMidiGrain(timing->SeedSamps, "tap tempo", stations);
	return true;
}

void Quantisation::PulseOverlay()
{
	auto expected = _overlayState.load(std::memory_order_relaxed);
	do {
		if (expected == StateHeld)
			return;
	} while (!_overlayState.compare_exchange_weak(
		expected,
		Timer::GetTime().time_since_epoch().count(),
		std::memory_order_release,
		std::memory_order_relaxed));
}

void Quantisation::SetOverlayHeld(bool held)
{
	_overlayState.store(
		held ? StateHeld : Timer::GetTime().time_since_epoch().count(),
		std::memory_order_release);
}

void Quantisation::ClearOverlay() noexcept
{
	_overlayState.store(StateInactive, std::memory_order_release);
}

float Quantisation::OverlayAlpha(Time now) const
{
	const auto state = _overlayState.load(std::memory_order_acquire);
	if (state == StateHeld)
		return 1.0f;
	if (state == StateInactive)
		return 0.0f;

	const auto lastActive = Time(Time::duration(state));
	const auto elapsed = Timer::GetElapsedSeconds(lastActive, now);
	if (elapsed >= OverlayFadeSeconds)
		return 0.0f;

	return static_cast<float>(1.0 - (elapsed / OverlayFadeSeconds));
}

void Quantisation::ApplyOverlayAlpha(float alpha,
	const std::vector<std::shared_ptr<Station>>& stations)
{
	for (const auto& station : stations)
	{
		if (station)
			station->SetQuantisationOverlayAlpha(alpha);
	}
}

bool Quantisation::TrySetMasterFromHover(const std::shared_ptr<base::GuiElement>& hovering,
	unsigned int depth,
	const std::vector<std::shared_ptr<Station>>& stations,
	unsigned int sampleRate,
	const io::UserConfig& cfg,
	bool confirm)
{
	if (!hovering)
		return false;

	const auto target = _ResolveInteractionTarget(hovering, depth, stations);
	const auto masterLength = target ? target->MasterLengthSamps : 0ul;
	if (masterLength == 0ul)
		return false;

	auto timing = DeduceSeedTiming(masterLength, sampleRate, Policy(cfg));
	if (!timing.has_value())
		return false;

	_masterLoop = target->RepresentativeLoopRef;
	{
		std::scoped_lock tapTempoLock(_tapTempoMutex);
		_masterLoopLengthSamps.store(masterLength, std::memory_order_release);
		_tapTempo.Clear();
	}

	SetSeedUsesPowers(cfg.Loop.SeedUsesPowers);
	if (_clock && _masterLoop)
		_clock->SetMasterLoopIndexFrac(_masterLoop->LoopIndexFrac());
	ApplyTiming(timing.value(), "master loop");
	SetMidiGrain(timing->SeedSamps, "master loop", stations);
	UpdateStationHints(hovering, depth, confirm, stations);

	std::cout << "Master quantisation target set: depth=" << static_cast<int>(depth)
		<< " length=" << masterLength << std::endl;
	return true;
}

void Quantisation::UpdateStationHints(const std::shared_ptr<base::GuiElement>& candidate,
	unsigned int depth,
	bool confirmCandidate,
	const std::vector<std::shared_ptr<Station>>& stations)
{
	ClearStationHints(stations);

	if (!_clock || !_clock->IsQuantisable())
		return;

	const auto seed = _clock->QuantiseSamps();
	const auto masterLengthSamps = _masterLoopLengthSamps.load(std::memory_order_acquire);
	const auto master = masterLengthSamps > 0ul ? static_cast<unsigned int>(masterLengthSamps) : seed;
	if (_masterLoop)
	{
		auto masterTarget = _ResolveInteractionTarget(_masterLoop, base::DEPTH_LOOP, stations);
		if (masterTarget && masterTarget->StationRef)
		{
			masterTarget->StationRef->SetQuantisationParams(QuantisationParams{
				seed,
				master
			}, false);
		}
	}

	if (!candidate)
		return;

	const auto candidateTarget = _ResolveInteractionTarget(candidate, depth, stations);
	if (!candidateTarget || candidateTarget->MasterLengthSamps == 0ul)
		return;

	if (candidateTarget->StationRef)
	{
		candidateTarget->StationRef->SetQuantisationParams(QuantisationParams{
			seed,
			static_cast<unsigned int>(candidateTarget->MasterLengthSamps)
		}, confirmCandidate);
	}
}

void Quantisation::ClearStationHints(const std::vector<std::shared_ptr<Station>>& stations)
{
	for (const auto& station : stations)
		station->ClearQuantisationParams();
}

std::optional<Quantisation::InteractionTarget> Quantisation::_ResolveInteractionTarget(
	const std::shared_ptr<base::GuiElement>& target,
	unsigned int depth,
	const std::vector<std::shared_ptr<Station>>& stations) const
{
	if (!target)
		return std::nullopt;

	InteractionTarget resolved;
	resolved.StationRef = std::dynamic_pointer_cast<Station>(target);
	resolved.TakeRef = std::dynamic_pointer_cast<LoopTake>(target);
	resolved.LoopRef = std::dynamic_pointer_cast<Loop>(target);

	switch (depth)
	{
	case base::DEPTH_STATION:
		if (!resolved.StationRef)
			return std::nullopt;
		return resolved;
	case base::DEPTH_LOOPTAKE:
		if (!resolved.TakeRef)
			return std::nullopt;

		for (const auto& station : stations)
		{
			const auto& takes = station->GetLoopTakes();
			if (std::find(takes.begin(), takes.end(), resolved.TakeRef) != takes.end())
			{
				resolved.StationRef = station;
				break;
			}
		}

		for (const auto& loop : resolved.TakeRef->GetLoops())
		{
			if (!loop)
				continue;

			if (!resolved.RepresentativeLoopRef || (loop->LoopLength() > resolved.RepresentativeLoopRef->LoopLength()))
				resolved.RepresentativeLoopRef = loop;
			resolved.MasterLengthSamps = std::max(resolved.MasterLengthSamps, loop->LoopLength());
		}

		return resolved;
	case base::DEPTH_LOOP:
		if (!resolved.LoopRef)
			return std::nullopt;

		for (const auto& station : stations)
		{
			for (const auto& take : station->GetLoopTakes())
			{
				const auto& loops = take->GetLoops();
				if (std::find(loops.begin(), loops.end(), resolved.LoopRef) != loops.end())
				{
					resolved.StationRef = station;
					resolved.TakeRef = take;
					resolved.RepresentativeLoopRef = resolved.LoopRef;
					resolved.MasterLengthSamps = resolved.LoopRef->LoopLength();
					return resolved;
				}
			}
		}

		return std::nullopt;
	default:
		return std::nullopt;
	}
}

void Quantisation::ApplyRemoteTempo(const ninjam::NinjamRemoteSnapshot& snapshot,
	const std::vector<std::shared_ptr<Station>>& stations,
	const io::UserConfig& cfg)
{
	if (!_clock || !snapshot.HasTiming)
		return;

	if (_armReclock.load(std::memory_order_acquire))
		return;

	if (_hasPendingTempo.load(std::memory_order_acquire))
		return;

	auto intervalLengthSamps = snapshot.IntervalLengthSamps;
	if (intervalLengthSamps == 0u)
	{
		intervalLengthSamps = IntervalSampsFromTempo(snapshot.Bpm,
			static_cast<unsigned int>(snapshot.Bpi),
			snapshot.SampleRate);
	}

	const auto tempoChanged = (intervalLengthSamps != _remoteMasterLoopSamps)
		|| (snapshot.SampleRate != _remoteSampleRate);

	if (!tempoChanged && (_effectiveQuantiseSamps.load(std::memory_order_acquire) != 0u) && _clock->IsQuantisable())
		return;

	const auto timing = cfg.DeduceLoopTiming(intervalLengthSamps, snapshot.SampleRate);
	if (!timing.has_value() || (timing->GrainSamps == 0u))
		return;

	_remoteMasterLoopSamps = intervalLengthSamps;
	_remoteSampleRate = snapshot.SampleRate;
	_effectiveQuantiseSamps.store(timing->GrainSamps, std::memory_order_release);
	_masterLoopLengthSamps.store(static_cast<unsigned long>(intervalLengthSamps), std::memory_order_release);
	{
		std::scoped_lock tapTempoLock(_tapTempoMutex);
		_tapTempo.Clear();
	}

	const auto quantisation = cfg.Loop.SeedUsesPowers ? Timer::QUANTISE_POWER : Timer::QUANTISE_MULTIPLE;
	_clock->SetQuantisation(timing->GrainSamps, quantisation);
	_clock->SetSeedSourceLength(intervalLengthSamps);
	if (intervalLengthSamps > 0u)
	{
		auto loopIndexFrac = 1.0;
		if (snapshot.IntervalPositionSamps > 0u)
		{
			const auto intervalPos = snapshot.IntervalPositionSamps % intervalLengthSamps;
			loopIndexFrac = 1.0 - (static_cast<double>(intervalPos) / static_cast<double>(intervalLengthSamps));
		}
		_clock->SetMasterLoopIndexFrac(loopIndexFrac);
	}

	SetMidiGrain(timing->GrainSamps, "remote tempo", stations);

	std::cout << "[NINJAM] Tempo policy applied: bpm=" << snapshot.Bpm
		<< " bpi=" << snapshot.Bpi
		<< " sr=" << snapshot.SampleRate
		<< " intervalSamps=" << intervalLengthSamps
		<< " mode=" << (cfg.Loop.SeedUsesPowers ? "power" : "multiple")
		<< " grain=" << timing->GrainSamps << std::endl;
}

void Quantisation::QueueLocalTempo(unsigned int remoteSampleRate,
	unsigned int audioDeviceSampleRate,
	const io::UserConfig& cfg)
{
	if (!_clock || !_clock->IsQuantisable())
		return;

	const auto quantiseSamps = _clock->QuantiseSamps();
	const auto previousQuantiseSamps = _effectiveQuantiseSamps.load(std::memory_order_acquire);
	if ((0u == quantiseSamps) || (quantiseSamps == previousQuantiseSamps))
		return;

	const auto shouldPulseOverlay = (0u == previousQuantiseSamps);

	const auto seedLoopLengthSamps = _clock->SeedSourceLength();
	if (seedLoopLengthSamps == 0u)
	{
		_effectiveQuantiseSamps.store(quantiseSamps, std::memory_order_release);
		_armReclock.store(false, std::memory_order_release);
		if (shouldPulseOverlay)
			PulseOverlay();
		return;
	}

	auto sampleRate = remoteSampleRate;
	if (sampleRate == 0u)
		sampleRate = audioDeviceSampleRate;
	if (sampleRate == 0u)
		sampleRate = cfg.Audio.SampleRate;
	if (sampleRate == 0u)
		return;

	const auto timing = cfg.DeduceLoopTiming(seedLoopLengthSamps, sampleRate);
	if (!timing.has_value())
		return;

	_effectiveQuantiseSamps.store(timing->GrainSamps, std::memory_order_release);
	_masterLoopLengthSamps.store(seedLoopLengthSamps, std::memory_order_release);
	_hasPendingTempo.store(true, std::memory_order_release);
	_armReclock.store(false, std::memory_order_release);
	if (shouldPulseOverlay)
		PulseOverlay();

	std::cout << "[NINJAM] Local tempo queued: bpm=" << timing->Bpm
		<< " bpi=" << timing->Bpi
		<< " grain=" << timing->GrainSamps
		<< " seedLoopLength=" << seedLoopLengthSamps
		<< " (queued for next interval boundary)" << std::endl;
}

void Quantisation::SendQueuedTempo(const ninjam::NinjamRemoteSnapshot& snapshot,
	ninjam::NinjamSession* ninjam,
	unsigned int remoteSampleRate,
	unsigned int audioDeviceSampleRate)
{
	const auto pos = snapshot.IntervalPositionSamps;
	const bool wrapped = (pos < _lastRemoteIntervalPos);
	_lastRemoteIntervalPos = pos;

	if (!_hasPendingTempo.load(std::memory_order_acquire))
		return;

	if (!ninjam || !ninjam->IsConnected())
		return;

	if (!wrapped)
		return;

	auto sampleRate = remoteSampleRate;
	if (sampleRate == 0u)
		sampleRate = audioDeviceSampleRate;
	if (sampleRate == 0u)
		return;

	const auto qtOpt = Quantisation::TimingFromSeedAndMaster(
		_effectiveQuantiseSamps.load(std::memory_order_acquire),
		_masterLoopLengthSamps.load(std::memory_order_acquire),
		sampleRate);
	if (!qtOpt.has_value() || (qtOpt->Bpm <= 0.0f) || (qtOpt->Bpi == 0u))
		return;

	if (ninjam->RequestServerTempo(qtOpt->Bpm, static_cast<int>(qtOpt->Bpi)))
		_hasPendingTempo.store(false, std::memory_order_release);
}

unsigned int Quantisation::EffectiveSamps() const noexcept
{
	return _effectiveQuantiseSamps.load(std::memory_order_acquire);
}

bool Quantisation::IsArmedForReclock() const noexcept
{
	return _armReclock.load(std::memory_order_acquire);
}

std::shared_ptr<Timer> Quantisation::Clock() const noexcept
{
	return _clock;
}

unsigned int Quantisation::RemoteSampleRate() const noexcept
{
	return _remoteSampleRate;
}

QuantisationPolicy Quantisation::Policy(const io::UserConfig& cfg)
{
	QuantisationPolicy policy;
	policy.SeedGrainMinMs = cfg.Loop.SeedGrainMinMs;
	policy.SeedGrainTargetMaxMs = cfg.Loop.SeedGrainTargetMaxMs;
	policy.SeedBpmMin = cfg.Loop.SeedBpmMin;
	policy.SeedUsesPowers = cfg.Loop.SeedUsesPowers;
	return policy;
}

unsigned int Quantisation::MinSeedSamps(unsigned int sampleRate, const QuantisationPolicy& policy)
{
	if (sampleRate == 0u)
		return 0u;

	const auto minMs = std::max(1u, policy.SeedGrainMinMs);
	return _RoundedToUInt((static_cast<double>(sampleRate) * static_cast<double>(minMs)) / 1000.0);
}

unsigned int Quantisation::IntervalSampsFromTempo(float bpm, unsigned int bpi, unsigned int sampleRate)
{
	if ((bpm <= 0.0f) || (bpi == 0u) || (sampleRate == 0u))
		return 0u;

	return _RoundedToUInt((static_cast<double>(sampleRate) * 60.0 * static_cast<double>(bpi)) / static_cast<double>(bpm));
}

std::optional<QuantisationTiming> Quantisation::TimingFromSeedAndMaster(unsigned int seedSamps,
	unsigned long masterSamps,
	unsigned int sampleRate)
{
	if ((seedSamps == 0u) || (masterSamps == 0ul) || (sampleRate == 0u))
		return std::nullopt;

	return _TimingFromSeed(seedSamps, masterSamps, sampleRate);
}

std::optional<QuantisationTiming> Quantisation::DeduceSeedTiming(unsigned long masterLoopSamps,
	unsigned int sampleRate,
	const QuantisationPolicy& policy)
{
	if ((masterLoopSamps == 0ul) || (sampleRate == 0u))
		return std::nullopt;

	auto seedSamps = masterLoopSamps;
	auto minSeed = static_cast<unsigned long>(MinSeedSamps(sampleRate, policy));
	if (minSeed == 0ul)
		return std::nullopt;

	const auto targetMaxMs = std::max(std::max(1u, policy.SeedGrainMinMs), policy.SeedGrainTargetMaxMs);
	const auto targetMaxSeed = static_cast<unsigned long>(std::max(static_cast<double>(minSeed),
		(static_cast<double>(sampleRate) * static_cast<double>(targetMaxMs)) / 1000.0));

	while ((seedSamps >= targetMaxSeed) && ((seedSamps / 2ul) >= minSeed))
		seedSamps /= 2ul;

	const auto minBpm = static_cast<float>(std::max(1u, policy.SeedBpmMin));
	while (((60.0f * static_cast<float>(sampleRate)) / static_cast<float>(seedSamps)) < minBpm
		&& ((seedSamps / 2ul) >= minSeed))
	{
		seedSamps /= 2ul;
	}

	if (seedSamps > std::numeric_limits<unsigned int>::max())
		return std::nullopt;

	return _TimingFromSeed(static_cast<unsigned int>(seedSamps), masterLoopSamps, sampleRate);
}

std::optional<QuantisationTiming> Quantisation::DeduceTapSeedTiming(unsigned long requestedSeedSamps,
	unsigned int sampleRate,
	const QuantisationPolicy& policy)
{
	if ((requestedSeedSamps == 0ul) || (sampleRate == 0u))
		return std::nullopt;

	const auto minSeed = MinSeedSamps(sampleRate, policy);
	if (minSeed == 0u)
		return std::nullopt;

	const auto seedSamps = _ClampToUInt(std::max<unsigned long>(requestedSeedSamps, minSeed));
	return _TimingFromSeed(seedSamps, seedSamps, sampleRate);
}

void TapTempoTracker::Clear() noexcept
{
	_lastTapSample.reset();
	_estimatedGapSamps.reset();
}

std::optional<QuantisationTiming> TapTempoTracker::TapAtSample(std::uint64_t samplePosition,
	unsigned int sampleRate,
	unsigned long masterLoopSamps,
	const QuantisationPolicy& policy)
{
	if (!_lastTapSample.has_value())
	{
		_lastTapSample = samplePosition;
		return std::nullopt;
	}

	if (samplePosition <= _lastTapSample.value())
	{
		std::cout << "[tap] rejected: non-increasing\n";
		_lastTapSample = samplePosition;
		return std::nullopt;
	}

	const auto gap = static_cast<double>(samplePosition - _lastTapSample.value());

	// Reset if the inter-tap gap exceeds the timeout; treat this tap as a fresh first tap.
	if (sampleRate > 0u && gap > TapTimeoutSecs * static_cast<double>(sampleRate))
	{
		Clear();
		_lastTapSample = samplePosition;
		return std::nullopt;
	}

	_lastTapSample = samplePosition;
	_estimatedGapSamps = _estimatedGapSamps.has_value() ?
		((_estimatedGapSamps.value() * 0.5) + (gap * 0.5)) :
		gap;

	if (sampleRate > 0u)
	{
		const auto msPerSamp = 1000.0 / static_cast<double>(sampleRate);
		std::cout << "[tap] raw=" << static_cast<int>(gap * msPerSamp + 0.5)
			<< "ms smooth=" << static_cast<int>(_estimatedGapSamps.value() * msPerSamp + 0.5)
			<< "ms\n";
	}

	return CurrentTiming(masterLoopSamps, sampleRate, policy);
}

std::optional<QuantisationTiming> TapTempoTracker::CurrentTiming(unsigned long masterLoopSamps,
	unsigned int sampleRate,
	const QuantisationPolicy& policy) const
{
	if (!_estimatedGapSamps.has_value())
		return std::nullopt;

	const auto requestedSeedSamps = static_cast<unsigned long>(_estimatedGapSamps.value() + 0.5);
	if (masterLoopSamps > 0ul)
		return Quantisation::DeduceTapSeedTimingFromMaster(requestedSeedSamps, masterLoopSamps, sampleRate);

	return Quantisation::DeduceTapSeedTiming(requestedSeedSamps, sampleRate, policy);
}

bool TapTempoTracker::HasEstimate() const noexcept
{
	return _estimatedGapSamps.has_value();
}

std::optional<QuantisationTiming> Quantisation::DeduceTapSeedTimingFromMaster(unsigned long tapGapSamps,
	unsigned long masterLoopSamps,
	unsigned int sampleRate)
{
	if ((tapGapSamps == 0ul) || (masterLoopSamps == 0ul) || (sampleRate == 0u))
		return std::nullopt;

	const auto bestSeed = _SnapSeedToMasterDivisor(tapGapSamps, masterLoopSamps);

	if ((bestSeed == 0ul) || (bestSeed > std::numeric_limits<unsigned int>::max()))
		return std::nullopt;

	return _TimingFromSeed(static_cast<unsigned int>(bestSeed), masterLoopSamps, sampleRate);
}
