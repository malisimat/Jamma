#include "TimingQuantiser.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include "../engine/Loop.h"
#include "../engine/LoopTake.h"
#include "../engine/Station.h"
#include "../ninjam/NinjamConnection.h"
#include "../ninjam/NinjamSession.h"
#include "../io/UserConfig.h"
#include "../midi/MidiQuantisation.h"
#include "../base/GuiElement.h"

using namespace actions;
using namespace base;
using namespace engine;
using namespace utils;

namespace timing
{



unsigned int TimingQuantiser::_ClampToUInt(unsigned long value)
{
	return value > std::numeric_limits<unsigned int>::max() ?
		std::numeric_limits<unsigned int>::max() :
		static_cast<unsigned int>(value);
}

unsigned int TimingQuantiser::_RoundedToUInt(double value)
{
	if (value <= 0.0)
		return 0u;

	if (value >= static_cast<double>(std::numeric_limits<unsigned int>::max()))
		return std::numeric_limits<unsigned int>::max();

	return static_cast<unsigned int>(value + 0.5);
}

std::int32_t TimingQuantiser::_ClampPhaseOffset(std::int64_t offsetSamps) noexcept
{
	if (offsetSamps > static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max()))
		return std::numeric_limits<std::int32_t>::max();
	if (offsetSamps < static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min()))
		return std::numeric_limits<std::int32_t>::min();
	return static_cast<std::int32_t>(offsetSamps);
}

unsigned long TimingQuantiser::_SnapSeedToMasterDivisor(unsigned long requestedSeedSamps,
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

std::optional<QuantisationTiming> TimingQuantiser::_TimingFromSeed(unsigned int seedSamps,
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

void TimingQuantiser::SetClock(std::shared_ptr<Timer> clock)
{
	_clock = std::move(clock);
}

void TimingQuantiser::SetSeedUsesPowers(bool seedUsesPowers) noexcept
{
	_seedUsesPowers = seedUsesPowers;
}

void TimingQuantiser::Set(unsigned int samps, utils::Timer::QuantisationType type)
{
	if (_clock)
		_clock->SetQuantisation(samps, type);
	_effectiveQuantiseSamps.store(samps, std::memory_order_release);
}

void TimingQuantiser::Clear(bool clearTapTempo)
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

void TimingQuantiser::ArmReclock()
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

void TimingQuantiser::ApplyTiming(const QuantisationTiming& timing, const char* source)
{
	if (!_clock || (timing.SeedSamps == 0u))
		return;

	_clock->SetQuantisation(timing.SeedSamps,
		_seedUsesPowers ? utils::Timer::QUANTISE_POWER : utils::Timer::QUANTISE_MULTIPLE);
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

void TimingQuantiser::SetMidiGrain(unsigned int grainSamps,
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

void TimingQuantiser::SetGlobalPhaseOffsetSamps(std::int32_t offsetSamps,
	const std::vector<std::shared_ptr<Station>>& stations)
{
	if (_globalPhaseOffsetSamps == offsetSamps)
		return;

	_globalPhaseOffsetSamps = offsetSamps;
	for (const auto& station : stations)
	{
		if (station)
			station->SetGlobalPhaseOffsetSamps(offsetSamps);
	}
}

std::int32_t TimingQuantiser::ResolvePhaseOffsetDrag(std::int32_t startOffsetSamps,
	int deltaX,
	unsigned int sampleRate) noexcept
{
	if (0u == sampleRate)
		return startOffsetSamps;

	const auto dragMs = static_cast<std::int64_t>(deltaX) / PhaseOffsetDragPixelsPerMillisecond;
	const auto dragSamps = dragMs * static_cast<std::int64_t>(sampleRate) / 1000LL;
	return _ClampPhaseOffset(static_cast<std::int64_t>(startOffsetSamps) + dragSamps);
}

bool TimingQuantiser::HandleTapTempo(std::uint64_t estimatedSampleAt,
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

	const auto quantisation = cfg.Loop.SeedUsesPowers ? utils::Timer::QUANTISE_POWER : utils::Timer::QUANTISE_MULTIPLE;
	if (_clock)
		_clock->SetQuantisation(timing->SeedSamps, quantisation);
	SetSeedUsesPowers(cfg.Loop.SeedUsesPowers);
	ApplyTiming(timing.value(), "tap tempo");
	SetMidiGrain(timing->SeedSamps, "tap tempo", stations);
	return true;
}

void TimingQuantiser::PulseOverlay()
{
	auto expected = _overlayState.load(std::memory_order_relaxed);
	do {
		if (expected == StateHeld)
			return;
	} while (!_overlayState.compare_exchange_weak(
		expected,
		utils::Timer::GetTime().time_since_epoch().count(),
		std::memory_order_release,
		std::memory_order_relaxed));
}

void TimingQuantiser::SetOverlayHeld(bool held)
{
	_overlayState.store(
		held ? StateHeld : utils::Timer::GetTime().time_since_epoch().count(),
		std::memory_order_release);
}

void TimingQuantiser::ClearOverlay() noexcept
{
	_overlayState.store(StateInactive, std::memory_order_release);
}

float TimingQuantiser::OverlayAlpha(Time now) const
{
	const auto state = _overlayState.load(std::memory_order_acquire);
	if (state == StateHeld)
		return 1.0f;
	if (state == StateInactive)
		return 0.0f;

	const auto lastActive = Time(Time::duration(state));
	const auto elapsed = utils::Timer::GetElapsedSeconds(lastActive, now);
	if (elapsed >= OverlayFadeSeconds)
		return 0.0f;

	return static_cast<float>(1.0 - (elapsed / OverlayFadeSeconds));
}

void TimingQuantiser::ApplyOverlayAlpha(float alpha,
	const std::vector<std::shared_ptr<Station>>& stations)
{
	for (const auto& station : stations)
	{
		if (station)
			station->SetQuantisationOverlayAlpha(alpha);
	}
}

bool TimingQuantiser::TrySetMasterFromHover(const std::shared_ptr<base::GuiElement>& hovering,
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

void TimingQuantiser::UpdateStationHints(const std::shared_ptr<base::GuiElement>& candidate,
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

void TimingQuantiser::ClearStationHints(const std::vector<std::shared_ptr<Station>>& stations)
{
	for (const auto& station : stations)
		station->ClearQuantisationParams();
}

std::optional<TimingQuantiser::InteractionTarget> TimingQuantiser::_ResolveInteractionTarget(
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

void TimingQuantiser::ApplyRemoteTempo(const ninjam::NinjamRemoteSnapshot& snapshot,
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

	const auto quantisation = cfg.Loop.SeedUsesPowers ? utils::Timer::QUANTISE_POWER : utils::Timer::QUANTISE_MULTIPLE;
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

void TimingQuantiser::QueueLocalTempo(unsigned int remoteSampleRate,
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

void TimingQuantiser::SendQueuedTempo(const ninjam::NinjamRemoteSnapshot& snapshot,
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

	const auto qtOpt = TimingQuantiser::TimingFromSeedAndMaster(
		_effectiveQuantiseSamps.load(std::memory_order_acquire),
		_masterLoopLengthSamps.load(std::memory_order_acquire),
		sampleRate);
	if (!qtOpt.has_value() || (qtOpt->Bpm <= 0.0f) || (qtOpt->Bpi == 0u))
		return;

	if (ninjam->RequestServerTempo(qtOpt->Bpm, static_cast<int>(qtOpt->Bpi)))
		_hasPendingTempo.store(false, std::memory_order_release);
}

unsigned int TimingQuantiser::EffectiveSamps() const noexcept
{
	return _effectiveQuantiseSamps.load(std::memory_order_acquire);
}

std::int32_t TimingQuantiser::GlobalPhaseOffsetSamps() const noexcept
{
	return _globalPhaseOffsetSamps;
}

bool TimingQuantiser::IsArmedForReclock() const noexcept
{
	return _armReclock.load(std::memory_order_acquire);
}

std::shared_ptr<Timer> TimingQuantiser::Clock() const noexcept
{
	return _clock;
}

unsigned int TimingQuantiser::RemoteSampleRate() const noexcept
{
	return _remoteSampleRate;
}

QuantisationPolicy TimingQuantiser::Policy(const io::UserConfig& cfg)
{
	QuantisationPolicy policy;
	policy.SeedGrainMinMs = cfg.Loop.SeedGrainMinMs;
	policy.SeedGrainTargetMaxMs = cfg.Loop.SeedGrainTargetMaxMs;
	policy.SeedBpmMin = cfg.Loop.SeedBpmMin;
	policy.SeedUsesPowers = cfg.Loop.SeedUsesPowers;
	return policy;
}

unsigned int TimingQuantiser::MinSeedSamps(unsigned int sampleRate, const QuantisationPolicy& policy)
{
	if (sampleRate == 0u)
		return 0u;

	const auto minMs = std::max(1u, policy.SeedGrainMinMs);
	return _RoundedToUInt((static_cast<double>(sampleRate) * static_cast<double>(minMs)) / 1000.0);
}

unsigned int TimingQuantiser::IntervalSampsFromTempo(float bpm, unsigned int bpi, unsigned int sampleRate)
{
	if ((bpm <= 0.0f) || (bpi == 0u) || (sampleRate == 0u))
		return 0u;

	return _RoundedToUInt((static_cast<double>(sampleRate) * 60.0 * static_cast<double>(bpi)) / static_cast<double>(bpm));
}

std::optional<QuantisationTiming> TimingQuantiser::TimingFromSeedAndMaster(unsigned int seedSamps,
	unsigned long masterSamps,
	unsigned int sampleRate)
{
	if ((seedSamps == 0u) || (masterSamps == 0ul) || (sampleRate == 0u))
		return std::nullopt;

	return _TimingFromSeed(seedSamps, masterSamps, sampleRate);
}

std::optional<QuantisationTiming> TimingQuantiser::DeduceSeedTiming(unsigned long masterLoopSamps,
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

std::optional<QuantisationTiming> TimingQuantiser::DeduceTapSeedTiming(unsigned long requestedSeedSamps,
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
		return TimingQuantiser::DeduceTapSeedTimingFromMaster(requestedSeedSamps, masterLoopSamps, sampleRate);

	return TimingQuantiser::DeduceTapSeedTiming(requestedSeedSamps, sampleRate, policy);
}

bool TapTempoTracker::HasEstimate() const noexcept
{
	return _estimatedGapSamps.has_value();
}

std::optional<QuantisationTiming> TimingQuantiser::DeduceTapSeedTimingFromMaster(unsigned long tapGapSamps,
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

	// ── TimingQuantiserController implementation ──

TimingQuantiserController::TimingQuantiserController(graphics::CtrlHandleOverlay& overlay,
	TimingQuantiser& quantisation,
	std::vector<std::shared_ptr<Station>>& stations) :
	_overlay(overlay),
	_quantisation(quantisation),
	_stations(stations),
	_ctrlHandleReleasedAt(utils::Timer::GetZero()),
	_fractionDragStartFraction(midi::MidiQuantisationFraction::Whole)
{
}

void TimingQuantiserController::OnCtrlModifierChanged(bool held,
	Time now,
	const QuantisationInteractionContext& context,
	const ChildResolver& childResolver)
{
	if (held && !_ctrlHandleHeld)
		_CaptureContext(context, childResolver);

	_quantisation.SetOverlayHeld(held);
	_ctrlHandleHeld = held;
	if (!held)
		_ctrlHandleReleasedAt = now;
	RefreshOverlay(context, childResolver);
}

void TimingQuantiserController::RefreshOverlay(const QuantisationInteractionContext& context,
	const ChildResolver& childResolver)
{
	if (_ctrlOverlayContext.has_value())
	{
		_overlay.SetVisibleButtonCount(_ctrlOverlayContext->VisibleButtonCount);
		_ApplyOverlayScopes(context, childResolver);
		_overlay.SetAnchor(_ctrlOverlayContext->Anchor, context.ViewportSize);
		return;
	}

	_overlay.SetVisibleButtonCount(_VisibleButtonCount(context));
	_ApplyOverlayScopes(context, childResolver);
	if (_ctrlHandleHeld)
		_overlay.SetAnchor(context.CursorPos, context.ViewportSize);
}

void TimingQuantiserController::Tick(Time now)
{
	_ApplyCtrlHandleAlpha(_CtrlHandleAlpha(now));
}

std::optional<ActionResult> TimingQuantiserController::TryHandleTouchAction(TouchAction action,
	unsigned int sampleRate,
	bool ctrlModifier,
	const QuantisationInteractionContext& context,
	const ChildResolver& childResolver)
{
	if (_isFractionDragging)
	{
		if (TouchAction::TouchState::TOUCH_UP == action.State)
			return _EndFractionDrag(action);

		ActionResult res;
		res.IsEaten = true;
		res.ResultType = ACTIONRESULT_DEFAULT;
		return res;
	}

	if (_isMidiPhaseDragging)
	{
		if (TouchAction::TouchState::TOUCH_UP == action.State)
			return _EndMidiPhaseDrag(action, sampleRate);

		ActionResult res;
		res.IsEaten = true;
		res.ResultType = ACTIONRESULT_DEFAULT;
		return res;
	}

	if ((TouchAction::TouchState::TOUCH_DOWN != action.State)
		|| (0 != action.Index)
		|| !ctrlModifier)
		return std::nullopt;

	const int hitBtn = _overlay.HitTestButton(action.Position);
	if (0 == hitBtn)
		return _BeginMidiPhaseDrag(action, context, childResolver);
	if (1 == hitBtn)
		return _BeginFractionDrag(action, context, childResolver);

	ActionResult res;
	res.IsEaten = false;
	res.ResultType = ACTIONRESULT_DEFAULT;
	return res;
}

std::optional<ActionResult> TimingQuantiserController::TryHandleTouchMove(TouchMoveAction action,
	unsigned int sampleRate)
{
	if (_isMidiPhaseDragging)
		return _UpdateMidiPhaseDrag(action, sampleRate);

	if (_isFractionDragging)
		return _UpdateFractionDrag(action);

	return std::nullopt;
}

int TimingQuantiserController::VisibleButtonCountForTest() const noexcept
{
	return _overlay.VisibleButtonCount();
}

std::optional<Position2d> TimingQuantiserController::ButtonCenterForTest(int buttonIndex) const noexcept
{
	return _overlay.ButtonCenter(buttonIndex);
}

void TimingQuantiserController::_CaptureContext(const QuantisationInteractionContext& context,
	const ChildResolver& childResolver)
{
	CtrlOverlayContext captured;
	captured.Anchor = context.CursorPos;
	captured.VisibleButtonCount = _VisibleButtonCount(context);
	captured.SelectDepth = context.SelectDepth;

	std::vector<unsigned char> hoverPath = context.HoverPath;
	auto hovering = childResolver(hoverPath);
	if (!hovering && !context.HoverPath3d.empty())
	{
		hoverPath = context.HoverPath3d;
		hovering = childResolver(hoverPath);
	}
	if (!hovering)
		hoverPath.clear();

	captured.HoverPath = std::move(hoverPath);
	_ctrlOverlayContext = std::move(captured);
}

float TimingQuantiserController::_CtrlHandleAlpha(Time now) const
{
	if (_ctrlHandleHeld)
		return 1.0f;
	if (utils::Timer::IsZero(_ctrlHandleReleasedAt))
		return 0.0f;
	static constexpr double FadeSeconds = 0.5;
	const auto elapsed = utils::Timer::GetElapsedSeconds(_ctrlHandleReleasedAt, now);
	if (elapsed >= FadeSeconds)
		return 0.0f;
	return static_cast<float>(1.0 - elapsed / FadeSeconds);
}

void TimingQuantiserController::_ApplyCtrlHandleAlpha(float alpha)
{
	_overlay.SetAlpha(alpha);
	if ((alpha <= 0.001f) && !_ctrlHandleHeld)
		_ctrlOverlayContext = std::nullopt;
}

int TimingQuantiserController::_VisibleButtonCount(const QuantisationInteractionContext& context) const
{
	if (_ctrlOverlayContext.has_value())
		return _ctrlOverlayContext->VisibleButtonCount;

	switch (_SelectDepth(context))
	{
	case base::SelectDepth::DEPTH_LOOPTAKE:
	case base::SelectDepth::DEPTH_LOOP:
	case base::SelectDepth::DEPTH_STATION:
		return 2;
	default:
		return 1;
	}
}

SelectDepth TimingQuantiserController::_SelectDepth(const QuantisationInteractionContext& context) const noexcept
{
	if (_ctrlOverlayContext.has_value())
		return _ctrlOverlayContext->SelectDepth;

	return context.SelectDepth;
}

std::shared_ptr<GuiElement> TimingQuantiserController::_HoverElement(const QuantisationInteractionContext& context,
	const ChildResolver& childResolver) const
{
	if (_ctrlOverlayContext.has_value())
		return childResolver(_ctrlOverlayContext->HoverPath);

	auto hovering = childResolver(context.HoverPath);
	if (!hovering && !context.HoverPath3d.empty())
		hovering = childResolver(context.HoverPath3d);
	return hovering;
}

void TimingQuantiserController::_ApplyOverlayScopes(const QuantisationInteractionContext& context,
	const ChildResolver& childResolver)
{
	_overlay.SetButtonScope(0,
		_IsPhaseGlobalTarget(context, childResolver)
			? graphics::CtrlHandleOverlay::ButtonScope::Global
			: graphics::CtrlHandleOverlay::ButtonScope::Local);
	_overlay.SetButtonScope(1,
		_IsDivisionGlobalTarget(context, childResolver)
			? graphics::CtrlHandleOverlay::ButtonScope::Global
			: graphics::CtrlHandleOverlay::ButtonScope::Local);
}

std::shared_ptr<Station> TimingQuantiserController::_StationFromElement(const std::shared_ptr<GuiElement>& element) const
{
	if (!element)
		return nullptr;

	auto station = std::dynamic_pointer_cast<Station>(element);
	if (station)
		return station;

	auto take = std::dynamic_pointer_cast<LoopTake>(element);
	if (take)
		return _StationForTake(take);

	return _StationForTake(_TakeForLoop(std::dynamic_pointer_cast<Loop>(element)));
}

std::vector<std::shared_ptr<Station>> TimingQuantiserController::_SelectedStations() const
{
	std::vector<std::shared_ptr<Station>> selected;
	for (const auto& station : _stations)
	{
		if (!station || station->IsRemote() || !station->IsSelected())
			continue;
		selected.push_back(station);
	}
	return selected;
}

std::vector<std::shared_ptr<LoopTake>> TimingQuantiserController::_SelectedLoopTakes(base::SelectDepth depth) const
{
	std::vector<std::shared_ptr<LoopTake>> selected;
	auto addTake = [&selected](const std::shared_ptr<LoopTake>& take) {
		if (!take)
			return;
		if (std::find(selected.begin(), selected.end(), take) == selected.end())
			selected.push_back(take);
	};

	_ForEachTake([depth, &addTake](const std::shared_ptr<Station>& station,
		const std::shared_ptr<LoopTake>& take) {
		if (!station || station->IsRemote())
			return;

		if ((depth == base::SelectDepth::DEPTH_LOOPTAKE) && take->IsSelected())
		{
			addTake(take);
			return;
		}

		if (depth == base::SelectDepth::DEPTH_LOOP)
		{
			for (const auto& loop : take->GetLoops())
			{
				if (loop && loop->IsSelected())
				{
					addTake(take);
					break;
				}
			}
		}
	});

	return selected;
}

std::vector<std::shared_ptr<LoopTake>> TimingQuantiserController::_AllLocalLoopTakes() const
{
	std::vector<std::shared_ptr<LoopTake>> targets;
	_ForEachTake([&targets](const std::shared_ptr<Station>& station,
		const std::shared_ptr<LoopTake>& take) {
		if (!station || station->IsRemote())
			return;
		targets.push_back(take);
	});
	return targets;
}

std::vector<std::shared_ptr<LoopTake>> TimingQuantiserController::_LoopTakesForStations(const std::vector<std::shared_ptr<Station>>& stations) const
{
	std::vector<std::shared_ptr<LoopTake>> targets;
	for (const auto& station : stations)
	{
		if (!station)
			continue;

		for (const auto& take : station->GetLoopTakes())
		{
			if (!take)
				continue;
			if (std::find(targets.begin(), targets.end(), take) == targets.end())
				targets.push_back(take);
		}
	}
	return targets;
}

bool TimingQuantiserController::_IsPhaseGlobalTarget(const QuantisationInteractionContext& context,
	const ChildResolver& childResolver) const
{
	const auto depth = _SelectDepth(context);
	if (depth == base::SelectDepth::DEPTH_STATION)
	{
		if (!_SelectedStations().empty())
			return false;
		auto hoveredStation = _StationFromElement(_HoverElement(context, childResolver));
		return !hoveredStation || hoveredStation->IsRemote();
	}

	if ((depth == base::SelectDepth::DEPTH_LOOPTAKE) || (depth == base::SelectDepth::DEPTH_LOOP))
	{
		if (!_SelectedLoopTakes(depth).empty())
			return false;
		return _TakeFromElement(_HoverElement(context, childResolver)) == nullptr;
	}

	return true;
}

bool TimingQuantiserController::_IsDivisionGlobalTarget(const QuantisationInteractionContext& context,
	const ChildResolver& childResolver) const
{
	const auto depth = _SelectDepth(context);
	if (depth == base::SelectDepth::DEPTH_STATION)
	{
		if (!_SelectedStations().empty())
			return false;
		auto hoveredStation = _StationFromElement(_HoverElement(context, childResolver));
		return !hoveredStation || hoveredStation->IsRemote();
	}

	if ((depth == base::SelectDepth::DEPTH_LOOPTAKE) || (depth == base::SelectDepth::DEPTH_LOOP))
	{
		if (!_SelectedLoopTakes(depth).empty())
			return false;
		return _TakeFromElement(_HoverElement(context, childResolver)) == nullptr;
	}

	return true;
}

ActionResult TimingQuantiserController::_BeginMidiPhaseDrag(TouchAction action,
	const QuantisationInteractionContext& context,
	const ChildResolver& childResolver)
{
	_isMidiPhaseDragging = true;
	_midiPhaseDragStartPosition = action.Position;
	_midiPhaseDragTarget = _ResolveMidiPhaseDragTarget(context, childResolver);
	_midiPhaseDragStartOffsetSamps = _MidiPhaseOffsetForTarget(_midiPhaseDragTarget);
	_quantisation.SetOverlayHeld(true);
	_quantisation.ApplyOverlayAlpha(1.0f, _stations);

	ActionResult res;
	res.IsEaten = true;
	res.ResultType = ACTIONRESULT_DEFAULT;
	return res;
}

ActionResult TimingQuantiserController::_UpdateMidiPhaseDrag(TouchMoveAction action,
	unsigned int sampleRate)
{
	const auto delta = action.Position - _midiPhaseDragStartPosition;
	const auto offsetSamps = TimingQuantiser::ResolvePhaseOffsetDrag(_midiPhaseDragStartOffsetSamps,
		delta.X,
		sampleRate);
	_SetMidiPhaseOffsetForTarget(_midiPhaseDragTarget, offsetSamps);
	_quantisation.ApplyOverlayAlpha(1.0f, _stations);

	ActionResult res;
	res.IsEaten = true;
	res.ResultType = ACTIONRESULT_DEFAULT;
	return res;
}

ActionResult TimingQuantiserController::_EndMidiPhaseDrag(TouchAction action,
	unsigned int sampleRate)
{
	if (_isMidiPhaseDragging)
	{
		const auto delta = action.Position - _midiPhaseDragStartPosition;
		const auto offsetSamps = TimingQuantiser::ResolvePhaseOffsetDrag(_midiPhaseDragStartOffsetSamps,
			delta.X,
			sampleRate);
		_SetMidiPhaseOffsetForTarget(_midiPhaseDragTarget, offsetSamps);
	}

	_isMidiPhaseDragging = false;
	_midiPhaseDragTarget = MidiPhaseDragTarget{};
	_quantisation.SetOverlayHeld(false);
	_quantisation.PulseOverlay();
	_quantisation.ApplyOverlayAlpha(_quantisation.OverlayAlpha(utils::Timer::GetTime()), _stations);

	return ActionResult::NoAction();
}

ActionResult TimingQuantiserController::_BeginFractionDrag(TouchAction action,
	const QuantisationInteractionContext& context,
	const ChildResolver& childResolver)
{
	_fractionDragTargets = _ResolveFractionDragTargets(context, childResolver);
	if (_fractionDragTargets.empty())
		return ActionResult::NoAction();

	_fractionDragStartY = action.Position.Y;
	_fractionDragMoved = false;
	_fractionDragTake.reset();
	_fractionDragStartFraction = _fractionDragTargets.front()->MidiQuantisation().Fraction;

	if (_fractionDragTargets.size() == 1u)
	{
		auto take = _fractionDragTargets.front();
		auto res = take->BeginMidiQuantisationGesture(action);
		if (!res.IsEaten)
		{
			_fractionDragTargets.clear();
			return res;
		}

		_isFractionDragging = true;
		_fractionDragTake = take;
		return res;
	}

	_isFractionDragging = true;

	ActionResult res;
	res.IsEaten = true;
	res.ResultType = ACTIONRESULT_DEFAULT;
	return res;
}

ActionResult TimingQuantiserController::_UpdateFractionDrag(TouchMoveAction action)
{
	if (!_isFractionDragging)
		return ActionResult::NoAction();

	_fractionDragMoved = true;

	if (_fractionDragTake)
	{
		auto res = _fractionDragTake->OnAction(action);
		return res;
	}

	if (_fractionDragTargets.empty())
		return ActionResult::NoAction();

	const auto deltaY = action.Position.Y - _fractionDragStartY;
	const auto fraction = midi::MidiQuantisation::ResolveDragFraction(_fractionDragStartFraction,
		deltaY);
	for (const auto& take : _fractionDragTargets)
	{
		if (!take)
			continue;

		auto settings = take->MidiQuantisation();
		settings.Enabled = true;
		settings.Fraction = fraction;
		take->SetMidiQuantisation(settings);
	}

	ActionResult res;
	res.IsEaten = true;
	res.ResultType = ACTIONRESULT_DEFAULT;
	return res;
}

ActionResult TimingQuantiserController::_EndFractionDrag(TouchAction action)
{
	auto take = _fractionDragTake;
	auto targets = _fractionDragTargets;
	const auto moved = _fractionDragMoved;
	_isFractionDragging = false;
	_fractionDragStartY = 0;
	_fractionDragTake.reset();
	_fractionDragTargets.clear();
	_fractionDragMoved = false;

	if (take)
		return take->OnAction(action);

	if (targets.empty())
		return ActionResult::NoAction();

	if (!moved)
	{
		for (const auto& target : targets)
		{
			if (!target)
				continue;

			auto settings = target->MidiQuantisation();
			settings.Enabled = !settings.Enabled;
			target->SetMidiQuantisation(settings);
		}
	}

	ActionResult res;
	res.IsEaten = true;
	res.ResultType = ACTIONRESULT_DEFAULT;
	return res;
}

void TimingQuantiserController::_ForEachTake(const std::function<void(const std::shared_ptr<Station>& station,
	const std::shared_ptr<LoopTake>& take)>& visit) const
{
	for (const auto& station : _stations)
	{
		if (!station)
			continue;

		for (const auto& take : station->GetLoopTakes())
		{
			if (!take)
				continue;

			visit(station, take);
		}
	}
}

std::shared_ptr<Station> TimingQuantiserController::_StationForTake(const std::shared_ptr<LoopTake>& take) const
{
	if (!take)
		return nullptr;

	std::shared_ptr<Station> resolved;
	_ForEachTake([&resolved, &take](const std::shared_ptr<Station>& station,
		const std::shared_ptr<LoopTake>& candidate) {
		if (!resolved && (candidate == take))
			resolved = station;
	});
	return resolved;
}

std::shared_ptr<LoopTake> TimingQuantiserController::_TakeForLoop(const std::shared_ptr<Loop>& loop) const
{
	if (!loop)
		return nullptr;

	std::shared_ptr<LoopTake> resolved;
	_ForEachTake([&resolved, &loop](const std::shared_ptr<Station>&,
		const std::shared_ptr<LoopTake>& candidateTake) {
		if (resolved)
			return;
		const auto& loops = candidateTake->GetLoops();
		if (std::find(loops.begin(), loops.end(), loop) != loops.end())
			resolved = candidateTake;
	});
	return resolved;
}

std::shared_ptr<LoopTake> TimingQuantiserController::_TakeFromElement(const std::shared_ptr<GuiElement>& element) const
{
	if (!element)
		return nullptr;

	auto take = std::dynamic_pointer_cast<LoopTake>(element);
	if (take)
		return take;

	return _TakeForLoop(std::dynamic_pointer_cast<Loop>(element));
}

std::shared_ptr<LoopTake> TimingQuantiserController::_FirstTakeForStation(const std::shared_ptr<Station>& station) const
{
	if (!station)
		return nullptr;

	for (const auto& take : station->GetLoopTakes())
	{
		if (take)
			return take;
	}

	return nullptr;
}

std::vector<std::shared_ptr<LoopTake>> TimingQuantiserController::_ResolveFractionDragTargets(const QuantisationInteractionContext& context,
	const ChildResolver& childResolver) const
{
	const auto depth = _SelectDepth(context);
	if (depth == base::SelectDepth::DEPTH_STATION)
	{
		auto selectedStations = _SelectedStations();
		if (!selectedStations.empty())
			return _LoopTakesForStations(selectedStations);

		auto hoveredStation = _StationFromElement(_HoverElement(context, childResolver));
		if (hoveredStation && !hoveredStation->IsRemote())
			return _LoopTakesForStations({ hoveredStation });

		return _AllLocalLoopTakes();
	}

	if ((depth == base::SelectDepth::DEPTH_LOOPTAKE) || (depth == base::SelectDepth::DEPTH_LOOP))
	{
		auto selected = _SelectedLoopTakes(depth);
		if (!selected.empty())
			return selected;

		auto hoveredTake = _TakeFromElement(_HoverElement(context, childResolver));
		if (hoveredTake)
			return { hoveredTake };

		return _AllLocalLoopTakes();
	}

	return {};
}

TimingQuantiserController::MidiPhaseDragTarget TimingQuantiserController::_ResolveMidiPhaseDragTarget(
	const QuantisationInteractionContext& context,
	const ChildResolver& childResolver) const
{
	MidiPhaseDragTarget target;

	const auto depth = _SelectDepth(context);
	if (depth == base::SelectDepth::DEPTH_STATION)
	{
		auto selectedStations = _SelectedStations();
		if (!selectedStations.empty())
		{
			target.Kind = MidiPhaseDragTargetKind::Station;
			target.StationTargets = std::move(selectedStations);
			target.StationRef = target.StationTargets.front();
			return target;
		}

		auto hoveredStation = _StationFromElement(_HoverElement(context, childResolver));
		if (hoveredStation && !hoveredStation->IsRemote())
		{
			target.Kind = MidiPhaseDragTargetKind::Station;
			target.StationRef = hoveredStation;
			target.StationTargets.push_back(hoveredStation);
		}
		return target;
	}

	if ((depth != base::SelectDepth::DEPTH_LOOPTAKE) && (depth != base::SelectDepth::DEPTH_LOOP))
		return target;

	auto selectedTakes = _SelectedLoopTakes(depth);
	if (!selectedTakes.empty())
	{
		target.Kind = MidiPhaseDragTargetKind::LoopTake;
		target.TakeTargets = std::move(selectedTakes);
		target.TakeRef = target.TakeTargets.front();
		return target;
	}

	auto hoveredTake = _TakeFromElement(_HoverElement(context, childResolver));
	if (hoveredTake)
	{
		target.Kind = MidiPhaseDragTargetKind::LoopTake;
		target.TakeRef = hoveredTake;
		target.TakeTargets.push_back(hoveredTake);
	}

	return target;
}

std::int32_t TimingQuantiserController::_MidiPhaseOffsetForTarget(const MidiPhaseDragTarget& target) const noexcept
{
	switch (target.Kind)
	{
	case MidiPhaseDragTargetKind::Station:
		return target.StationRef ? target.StationRef->StationPhaseOffsetSamps() : 0;
	case MidiPhaseDragTargetKind::LoopTake:
		return target.TakeRef ? target.TakeRef->MidiQuantisation().PhaseOffsetSamps : 0;
	case MidiPhaseDragTargetKind::Global:
	default:
		return _quantisation.GlobalPhaseOffsetSamps();
	}
}

void TimingQuantiserController::_SetMidiPhaseOffsetForTarget(const MidiPhaseDragTarget& target,
	std::int32_t offsetSamps) noexcept
{
	switch (target.Kind)
	{
	case MidiPhaseDragTargetKind::Station:
		for (const auto& station : target.StationTargets)
		{
			if (station)
				station->SetStationPhaseOffsetSamps(offsetSamps);
		}
		break;
	case MidiPhaseDragTargetKind::LoopTake:
		for (const auto& take : target.TakeTargets)
		{
			if (!take)
				continue;

			auto settings = take->MidiQuantisation();
			settings.PhaseOffsetSamps = offsetSamps;
			take->SetMidiQuantisation(settings);
		}
		break;
	case MidiPhaseDragTargetKind::Global:
	default:
		_quantisation.SetGlobalPhaseOffsetSamps(offsetSamps, _stations);
		break;
	}
}
} // namespace timing
