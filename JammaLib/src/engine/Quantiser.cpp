#include <iomanip>
#include <sstream>
#include "Quantiser.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include "../engine/Loop.h"
#include "../engine/LoopTake.h"
#include "../engine/Station.h"
#include "../io/UserConfig.h"
#include "../midi/MidiQuantisation.h"
#include "../base/GuiElement.h"

using namespace actions;
using namespace base;
using namespace engine;
using namespace utils;

namespace engine
{



unsigned int Quantiser::_ClampToUInt(unsigned long value)
{
	return value > std::numeric_limits<unsigned int>::max() ?
		std::numeric_limits<unsigned int>::max() :
		static_cast<unsigned int>(value);
}

unsigned int Quantiser::_RoundedToUInt(double value)
{
	if (value <= 0.0)
		return 0u;

	if (value >= static_cast<double>(std::numeric_limits<unsigned int>::max()))
		return std::numeric_limits<unsigned int>::max();

	return static_cast<unsigned int>(value + 0.5);
}

std::int32_t Quantiser::_ClampPhaseOffset(std::int64_t offsetSamps) noexcept
{
	if (offsetSamps > static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max()))
		return std::numeric_limits<std::int32_t>::max();
	if (offsetSamps < static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min()))
		return std::numeric_limits<std::int32_t>::min();
	return static_cast<std::int32_t>(offsetSamps);
}

unsigned long Quantiser::_SnapSeedToMasterDivisor(unsigned long requestedSeedSamps,
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

std::optional<QuantisationTiming> Quantiser::_TimingFromSeed(unsigned int seedSamps,
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
	// Preserve the recording buffer separately from this logical source interval.
	// Normalising downward makes every committed local loop grain-clean.
	timing.MasterLoopSamps = _ClampToUInt(seedCount * static_cast<unsigned long>(seedSamps));
	timing.SeedCount = static_cast<unsigned int>(seedCount);
	timing.Bpm = (60.0f * static_cast<float>(sampleRate)) / static_cast<float>(seedSamps);
	timing.Bpi = timing.SeedCount;
	return timing;
}

void Quantiser::SetClock(std::shared_ptr<Timer> clock)
{
	_clock = std::move(clock);
}

void Quantiser::SetSeedUsesPowers(bool seedUsesPowers) noexcept
{
	_seedUsesPowers = seedUsesPowers;
}

void Quantiser::Set(unsigned int samps, utils::Timer::QuantisationType type)
{
	if (_clock)
		_clock->SetQuantisation(samps, type);
	_effectiveQuantiseSamps.store(samps, std::memory_order_release);
}

void Quantiser::Clear(bool clearTapTempo, bool preserveTiming)
{
	if (!preserveTiming)
	{
		if (_clock)
			_clock->Clear();
		_masterLoopLengthSamps.store(0ul, std::memory_order_release);
		_masterOriginalBufferLengthSamps.store(0ul, std::memory_order_release);
		_effectiveQuantiseSamps.store(0u, std::memory_order_release);
		_activeGridDivisions.store(0u, std::memory_order_release);
	}

	_masterLoop.reset();
	if (!preserveTiming)
	{
		_acceptedRemoteGrid = {};
		_gridSource.store(QuantisationGridSource::Default, std::memory_order_release);
	}
	_armReclock.store(false, std::memory_order_release);
	_preReclockTakeIds.clear();

	if (clearTapTempo)
	{
		std::scoped_lock tapTempoLock(_tapTempoMutex);
		_tapTempo.Clear();
	}
}

void Quantiser::ArmReclock()
{
	ArmReclock({});
}

void Quantiser::ArmReclock(const std::vector<std::shared_ptr<engine::Station>>& stations)
{
	_reclockGeneration.fetch_add(1u, std::memory_order_acq_rel);
	_acceptedRemoteGrid = {};
	_gridSource.store(QuantisationGridSource::Default, std::memory_order_release);
	if (_clock)
		_clock->Clear();
	_masterLoop.reset();
	_masterLoopLengthSamps.store(0ul, std::memory_order_release);
	_masterOriginalBufferLengthSamps.store(0ul, std::memory_order_release);
	{
		std::scoped_lock tapTempoLock(_tapTempoMutex);
		_tapTempo.Clear();
	}
	_armReclock.store(true, std::memory_order_release);
	_effectiveQuantiseSamps.store(0u, std::memory_order_release);
	_activeGridDivisions.store(0u, std::memory_order_release);
	_preReclockTakeIds.clear();
	for (const auto& station : stations)
		if (station && !station->IsRemote())
			for (const auto& take : station->GetLoopTakes())
				if (take)
					_preReclockTakeIds.push_back(take->Id());
}

void Quantiser::ApplyTiming(const QuantisationTiming& timing, const char* source)
{
	if (!_clock || (timing.SeedSamps == 0u))
		return;

	_clock->SetQuantisation(timing.SeedSamps,
		_seedUsesPowers ? utils::Timer::QUANTISE_POWER : utils::Timer::QUANTISE_MULTIPLE);
	_clock->SetSeedSourceLength(timing.MasterLoopSamps);

	_effectiveQuantiseSamps.store(timing.SeedSamps, std::memory_order_release);
	_activeGridDivisions.store(timing.Bpi, std::memory_order_release);
	_gridSource.store(QuantisationGridSource::Default, std::memory_order_release);
	_armReclock.store(false, std::memory_order_release);

	std::cout << "Quantisation " << source
		<< ": seed=" << timing.SeedSamps
		<< " master=" << timing.MasterLoopSamps
		<< " seeds=" << timing.SeedCount
		<< " bpm=" << timing.Bpm
		<< " bpi=" << timing.Bpi << std::endl;
}

void Quantiser::SetMidiGrain(unsigned int grainSamps,
	const char* source,
	const std::vector<std::shared_ptr<Station>>& stations)
{
	_effectiveQuantiseSamps.store(grainSamps, std::memory_order_release);
	unsigned int takeCount = 0u;
	for (const auto& station : stations)
	{
		if (!station || station->IsRemote())
			continue;

		for (const auto& take : station->GetLoopTakes())
		{
			if (!take)
				continue;
			// Grain publication never changes remote timing authority.

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

void Quantiser::SetRemoteMidiGrid(const RemoteTransportGeometry& geometry,
	std::int64_t originSamps,
	const std::vector<std::shared_ptr<Station>>& stations)
{
	const auto changed = _acceptedRemoteGrid.IntervalLengthSamps != geometry.IntervalLengthSamps
		|| _acceptedRemoteGrid.Bpi != geometry.Bpi;
	_acceptedRemoteGrid = geometry;
	_gridSource = geometry.IntervalLengthSamps && geometry.Bpi ? QuantisationGridSource::Remote : QuantisationGridSource::Default;
	for (const auto& station : stations)
	{
		if (!station)
			continue;
		for (const auto& take : station->GetLoopTakes())
		{
			if (take)
			{
				take->SetRemoteMidiQuantisationGrid(geometry, originSamps);
				if (changed) take->SetMidiBaseGrid(0u, 0u);
			}
		}
	}
}

void Quantiser::SetGlobalPhaseOffsetSamps(std::int32_t offsetSamps,
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

std::int32_t Quantiser::ResolvePhaseOffsetDrag(std::int32_t startOffsetSamps,
	int deltaX,
	unsigned int sampleRate) noexcept
{
	if (0u == sampleRate)
		return startOffsetSamps;

	const auto dragMs = static_cast<std::int64_t>(deltaX) / PhaseOffsetDragPixelsPerMillisecond;
	const auto dragSamps = dragMs * static_cast<std::int64_t>(sampleRate) / 1000LL;
	return _ClampPhaseOffset(static_cast<std::int64_t>(startOffsetSamps) + dragSamps);
}

bool Quantiser::HandleTapTempo(std::uint64_t estimatedSampleAt,
    unsigned int sampleRate,
    const std::vector<std::shared_ptr<Station>>& stations,
    const io::UserConfig& cfg)
{
    PulseOverlay();
    unsigned int eligible = 0u;
    std::shared_ptr<LoopTake> soleTake;
    for (const auto& station : stations)
        if (station && !station->IsRemote())
            for (const auto& take : station->GetLoopTakes())
                if (take && take->IsCompletedRecording()
                    && std::find(_preReclockTakeIds.begin(), _preReclockTakeIds.end(), take->Id()) == _preReclockTakeIds.end())
                {
                    ++eligible;
                    soleTake = take;
                }
    const bool remote = _acceptedRemoteGrid.IntervalLengthSamps > 0ul && _acceptedRemoteGrid.Bpi > 0u;
    if (eligible == 1u && !_masterLoopLengthSamps.load(std::memory_order_acquire))
    {
        _masterLoop = soleTake->GetLoops().empty() ? nullptr : soleTake->GetLoops().front();
        const auto length = soleTake->VisualLoopLengthSamps();
        _masterLoopLengthSamps.store(length, std::memory_order_release);
        _masterOriginalBufferLengthSamps.store(_masterLoop ? _masterLoop->PhysicalLoopLength() : length, std::memory_order_release);
    }
    const auto original = _masterOriginalBufferLengthSamps.load(std::memory_order_acquire);
    const auto interval = remote ? _acceptedRemoteGrid.IntervalLengthSamps :
        (eligible == 1u && original ? original : (_clock ? _clock->SeedSourceLength() : 0ul));
    std::optional<QuantisationTiming> timing;
    double requestedDivisions = 0.0;
    double smoothedGap = 0.0;
    {
        std::scoped_lock tapTempoLock(_tapTempoMutex);
        timing = _tapTempo.TapAtSample(estimatedSampleAt, sampleRate, interval, Policy(cfg));
        smoothedGap = _tapTempo.EstimatedGapSamps();
        if (smoothedGap > 0.0) requestedDivisions = interval / smoothedGap;
    }
    if (!timing)
    {
        std::cout << "[quantisation] tap sequence/no update: sample=" << estimatedSampleAt
            << " generation=" << ReclockGeneration() << " sr=" << sampleRate << " eligible=" << eligible << " authority=" << (remote ? "remote" : "local") << std::endl;
        return true;
    }
    if (eligible != 1u || remote)
    {
        const auto grain = _clock ? _clock->QuantiseSamps() : 0u;
        const auto base = remote ? _acceptedRemoteGrid.Bpi :
            (grain && _clock ? static_cast<unsigned int>(_clock->SeedSourceLength() / grain) : 0u);
        if (!base || !interval)
        {
            std::cout << "[quantisation] tap rejected: no valid base geometry generation=" << ReclockGeneration()
                << " eligible=" << eligible << " sr=" << sampleRate << " interval=" << interval << " grain=" << grain << std::endl;
            return true;
        }
        const auto previous = ActiveGridDivisions();
        const auto selected = NearestPermittedDivision(base, requestedDivisions);
        for (const auto& station : stations)
            if (station && !station->IsRemote())
                for (const auto& take : station->GetLoopTakes())
                    if (take && (static_cast<std::uint64_t>(take->VisualLoopLengthSamps()) * selected + interval - 1u) / interval > 8192u)
                    {
                        std::cout << "[quantisation] tap rejected: effective grid exceeds 8192 cells take=" << take->Id() << std::endl;
                        return true;
                    }
        _activeGridDivisions.store(selected, std::memory_order_release);
        _gridSource = remote ? QuantisationGridSource::Remote : QuantisationGridSource::Tap;
        for (const auto& station : stations)
            if (station && !station->IsRemote())
                for (const auto& take : station->GetLoopTakes())
                    if (take)
                    {
                        // A frozen master keeps its grain/beat grid; tapping selects
                        // the subdivision itself, also shown by the fraction radio.
                        take->SetMidiBaseGrid(static_cast<std::uint32_t>(interval), base);
                        auto settings = take->MidiQuantisation();
                        for (const auto fraction : midi::MidiQuantisation::FractionDisplayOrder)
                            if (midi::MidiQuantisation::Divisor(fraction) == selected / base)
                                settings.Fraction = fraction;
                        take->SetMidiQuantisation(settings);
                    }
        unsigned int minimumDivisor = 32u;
        unsigned int maximumDivisor = 1u;
        for (const auto& station : stations)
            if (station && !station->IsRemote())
                for (const auto& take : station->GetLoopTakes())
                    if (take)
                    {
                        const auto divisor = midi::MidiQuantisation::Divisor(take->ResolvedMidiQuantisation().Fraction);
                        minimumDivisor = (std::min)(minimumDivisor, divisor);
                        maximumDivisor = (std::max)(maximumDivisor, divisor);
                    }
        std::cout << "[quantisation] tap subdivisions " << (previous == selected ? "unchanged" : "accepted")
            << ": generation=" << ReclockGeneration() << " sr=" << sampleRate << " eligible=" << eligible
            << " authority=" << (remote ? "remote" : "local") << " interval=" << interval
            << " grain=" << grain << " base=" << base << " requested=" << timing->Bpi
            << " before=" << previous << " gap=" << smoothedGap << " gapMs=" << (1000.0 * smoothedGap / sampleRate)
            << " bpm=" << (60.0 * sampleRate / smoothedGap) << " fractionDivisors=" << minimumDivisor << ".." << maximumDivisor
            << " effectiveCells=" << base * minimumDivisor << ".." << base * maximumDivisor
            << " requestedRatio=" << requestedDivisions << " selected=" << selected << " tie=smaller geometry=preserved" << std::endl;
        return true;
    }
    if (!LocalAudioGeometry::Create(original, timing->MasterLoopSamps, timing->SeedSamps, timing->Bpi))
    {
        std::cout << "[quantisation] tap geometry rejected: generation=" << ReclockGeneration()
            << " physical=" << original << " logical=" << timing->MasterLoopSamps << " grain=" << timing->SeedSamps
            << " grains=" << timing->Bpi << std::endl;
        return true;
    }
    // The recording's physical storage remains intact; Loop::Play only changes
    // its logical boundary and retains the recorded tail for a later resolution.
    for (const auto& loop : soleTake->GetLoops())
        if (loop) loop->Play(loop->PlayIndex(), timing->MasterLoopSamps, false);
    soleTake->SetMidiBaseGrid(timing->MasterLoopSamps, timing->Bpi);
    SetSeedUsesPowers(cfg.Loop.SeedUsesPowers);
    ApplyTiming(*timing, "tap tempo");
    _gridSource.store(QuantisationGridSource::Tap, std::memory_order_release);
    _masterLoopLengthSamps.store(timing->MasterLoopSamps, std::memory_order_release);
    SetMidiGrain(timing->SeedSamps, "tap tempo", stations);
    std::cout << "[quantisation] tap geometry accepted: take=" << soleTake->Id()
        << " generation=" << ReclockGeneration() << " sr=" << sampleRate << " eligible=" << eligible
        << " authority=local source=tap physical=" << original << " logical=" << timing->MasterLoopSamps
        << " grain=" << timing->SeedSamps << " grains=" << timing->Bpi << " bpm=" << timing->Bpm
        << " gap=" << smoothedGap << " fraction=" << midi::MidiQuantisation::FractionLabel(soleTake->ResolvedMidiQuantisation().Fraction)
        << " effectiveCells=" << soleTake->ResolvedMidiQuantisation().GridDivisions()
        << " tail=" << original - timing->MasterLoopSamps << " rounding=nearest-beats/up-ties,floor-grain" << std::endl;
    return true;
}

unsigned int Quantiser::NearestPermittedDivision(unsigned int base, double requested) noexcept
{
    if (!base || !std::isfinite(requested) || requested <= 0.0) return base;
    unsigned int best = base;
    // Use the same supported straight/triplet subdivisions as the radio.
    for (const auto fraction : midi::MidiQuantisation::FractionDisplayOrder)
    {
        const auto candidate = static_cast<std::uint64_t>(base) * midi::MidiQuantisation::Divisor(fraction);
        if (candidate > std::numeric_limits<unsigned int>::max()) continue;
        const auto distance = std::abs(static_cast<double>(candidate) - requested);
        const auto previous = std::abs(static_cast<double>(best) - requested);
        if (distance < previous || (distance == previous && candidate < best))
            best = static_cast<unsigned int>(candidate);
    }
    return best;
}

void Quantiser::PulseOverlay()
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

void Quantiser::SetOverlayHeld(bool held)
{
	if (_spaceOverlayHeld.exchange(held, std::memory_order_acq_rel) == held)
		return;
	held = _spaceOverlayHeld.load(std::memory_order_acquire) || _gestureOverlayHeld.load(std::memory_order_acquire);
	_overlayState.store(
		held ? StateHeld : utils::Timer::GetTime().time_since_epoch().count(),
		std::memory_order_release);
}

void Quantiser::SetGestureOverlayHeld(bool held)
{
    if (_gestureOverlayHeld.exchange(held, std::memory_order_acq_rel) == held)
        return;
    _overlayState.store((_spaceOverlayHeld.load(std::memory_order_acquire) || _gestureOverlayHeld.load(std::memory_order_acquire)) ? StateHeld :
        utils::Timer::GetTime().time_since_epoch().count(), std::memory_order_release);
}

void Quantiser::ClearOverlay() noexcept
{
	_spaceOverlayHeld = false;
	_gestureOverlayHeld = false;
	_overlayState.store(StateInactive, std::memory_order_release);
}

float Quantiser::OverlayAlpha(Time now) const
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

void Quantiser::ApplyOverlayAlpha(float alpha,
	const std::vector<std::shared_ptr<Station>>& stations)
{
	for (const auto& station : stations)
	{
		if (station)
			station->SetQuantisationOverlayAlpha(alpha);
	}
}

bool Quantiser::TrySetMasterFromHover(const std::shared_ptr<base::GuiElement>& hovering,
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
		_masterOriginalBufferLengthSamps.store(_masterLoop ? _masterLoop->PhysicalLoopLength() : masterLength,
			std::memory_order_release);
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

void Quantiser::UpdateStationHints(const std::shared_ptr<base::GuiElement>& candidate,
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

void Quantiser::ClearStationHints(const std::vector<std::shared_ptr<Station>>& stations)
{
	for (const auto& station : stations)
		station->ClearQuantisationParams();
}

std::optional<Quantiser::InteractionTarget> Quantiser::_ResolveInteractionTarget(
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

		resolved.MasterLengthSamps = resolved.TakeRef->VisualLoopLengthSamps();

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

unsigned int Quantiser::EffectiveSamps() const noexcept
{
	return _effectiveQuantiseSamps.load(std::memory_order_acquire);
}

unsigned int Quantiser::ActiveGridDivisions() const noexcept
{
	return _activeGridDivisions.load(std::memory_order_acquire);
}

QuantisationGrid Quantiser::ActiveGrid() const noexcept
{
	return { ActiveGridDivisions(), _gridSource.load(std::memory_order_acquire) };
}

std::int32_t Quantiser::GlobalPhaseOffsetSamps() const noexcept
{
	return _globalPhaseOffsetSamps;
}

bool Quantiser::IsArmedForReclock() const noexcept
{
	return _armReclock.load(std::memory_order_acquire);
}

std::shared_ptr<Timer> Quantiser::Clock() const noexcept
{
	return _clock;
}

std::optional<QuantisationTiming> Quantiser::CurrentTempoTiming(unsigned int sampleRate) const
{
	auto seedSamps = _effectiveQuantiseSamps.load(std::memory_order_acquire);
	auto masterLoopSamps = _masterLoopLengthSamps.load(std::memory_order_acquire);
	if (_clock && (seedSamps == 0u || masterLoopSamps == 0ul))
	{
		seedSamps = _clock->QuantiseSamps();
		masterLoopSamps = _clock->SeedSourceLength();
	}
	return TimingFromSeedAndMaster(seedSamps, masterLoopSamps, sampleRate);
}

void Quantiser::LogNinjamTempoEvent(const char* event,
	unsigned long masterLoopLengthSamps,
	unsigned int grainSamps,
	unsigned int bpi,
	float bpm,
	unsigned int sampleRate)
{
	std::cout << "[NINJAM] " << event
		<< ": master=" << masterLoopLengthSamps
		<< " grain=" << grainSamps
		<< " bpi=" << bpi
		<< " bpm=" << bpm
		<< " sr=" << sampleRate
		<< std::endl;
}

void Quantiser::LogNinjamManualTempoCommands(float bpm, unsigned int bpi)
{
	std::cout << "[NINJAM] Manual server tempo commands: /bpm " << static_cast<int>(bpm + 0.5f)
		<< " /bpi " << bpi
		<< " | !vote bpm " << static_cast<int>(bpm + 0.5f)
		<< " | !vote bpi " << bpi
		<< std::endl;
}

QuantisationPolicy Quantiser::Policy(const io::UserConfig& cfg)
{
	QuantisationPolicy policy;
	policy.SeedGrainMinMs = cfg.Loop.SeedGrainMinMs;
	policy.SeedGrainTargetMaxMs = cfg.Loop.SeedGrainTargetMaxMs;
	policy.SeedBpmMin = cfg.Loop.SeedBpmMin;
	policy.SeedUsesPowers = cfg.Loop.SeedUsesPowers;
	return policy;
}

unsigned int Quantiser::MinSeedSamps(unsigned int sampleRate, const QuantisationPolicy& policy)
{
	if (sampleRate == 0u)
		return 0u;

	const auto minMs = std::max(1u, policy.SeedGrainMinMs);
	return _RoundedToUInt((static_cast<double>(sampleRate) * static_cast<double>(minMs)) / 1000.0);
}

std::optional<QuantisationTiming> Quantiser::TimingFromSeedAndMaster(unsigned int seedSamps,
	unsigned long masterSamps,
	unsigned int sampleRate)
{
	if ((seedSamps == 0u) || (masterSamps == 0ul) || (sampleRate == 0u))
		return std::nullopt;

	return _TimingFromSeed(seedSamps, masterSamps, sampleRate);
}

std::optional<QuantisationTiming> Quantiser::DeduceSeedTiming(unsigned long masterLoopSamps,
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
	const auto targetMaxSeed = static_cast<unsigned long>(_RoundedToUInt(std::max(static_cast<double>(minSeed),
		(static_cast<double>(sampleRate) * static_cast<double>(targetMaxMs)) / 1000.0)));

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

std::optional<QuantisationTiming> Quantiser::DeduceTapSeedTiming(unsigned long requestedSeedSamps,
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
	if (!sampleRate)
	{
		Clear();
		std::cout << "[tap] rejected: invalid sample rate\n";
		return std::nullopt;
	}
	if (!_lastTapSample.has_value())
	{
		_lastTapSample = samplePosition;
		return std::nullopt;
	}

	if (samplePosition <= _lastTapSample.value())
	{
		std::cout << "[tap] rejected: non-increasing\n";
		Clear();
		_lastTapSample = samplePosition;
		return std::nullopt;
	}

	const auto gap = static_cast<double>(samplePosition - _lastTapSample.value());

	// Reset if the inter-tap gap exceeds the timeout; treat this tap as a fresh first tap.
	if (sampleRate > 0u && gap > TapTimeoutSecs * static_cast<double>(sampleRate))
	{
		Clear();
		_lastTapSample = samplePosition;
		std::cout << "[tap] sequence restarted: timeout >3000ms\n";
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
		return Quantiser::DeduceTapSeedTimingFromMaster(requestedSeedSamps, masterLoopSamps, sampleRate);

	return Quantiser::DeduceTapSeedTiming(requestedSeedSamps, sampleRate, policy);
}

bool TapTempoTracker::HasEstimate() const noexcept
{
	return _estimatedGapSamps.has_value();
}

std::optional<QuantisationTiming> Quantiser::DeduceTapSeedTimingFromMaster(unsigned long tapGapSamps,
	unsigned long masterLoopSamps,
	unsigned int sampleRate)
{
	if ((tapGapSamps == 0ul) || (masterLoopSamps == 0ul) || (sampleRate == 0u))
		return std::nullopt;

	// Taps express musical divisions, not a sample divisor search. Rebuild the
	// exact local geometry from the immutable recording length so repeated meter
	// changes never compound a previous trim.
	const auto requestedBpi = static_cast<unsigned long>((masterLoopSamps + (tapGapSamps / 2ul)) / tapGapSamps);
	if (requestedBpi == 0ul || requestedBpi > std::numeric_limits<unsigned int>::max())
		return std::nullopt;
	const auto grain = masterLoopSamps / requestedBpi;
	if (grain == 0ul || grain > std::numeric_limits<unsigned int>::max())
		return std::nullopt;
	auto timing = _TimingFromSeed(static_cast<unsigned int>(grain), grain * requestedBpi, sampleRate);
    if (timing)
    {
        timing->SeedCount = static_cast<unsigned int>(requestedBpi);
        timing->Bpi = static_cast<unsigned int>(requestedBpi);
    }
    return timing;
}

	// ── QuantiserController implementation ──

QuantiserController::QuantiserController(graphics::CtrlHandleOverlay& overlay,
	Quantiser& quantisation,
	std::vector<std::shared_ptr<Station>>& stations) :
	_overlay(overlay),
	_quantisation(quantisation),
	_stations(stations),
	_ctrlHandleReleasedAt(utils::Timer::GetZero()),
	_fractionDragStartFraction(midi::MidiQuantisationFraction::Whole)
{
}

void QuantiserController::OnCtrlModifierChanged(bool held,
	Time now,
	const QuantisationInteractionContext& context,
	const ChildResolver& childResolver)
{
	if (held == _ctrlHandleHeld)
		return;
	_transitionStartAlpha = _CtrlHandleAlpha(now);
	_ctrlHandleReleasedAt = now;
	if (held && !OwnsPointer())
	{
		_ctrlOverlayContext.reset();
		_capturedMidiTargets.clear();
		if (context.SelectDepth == base::SelectDepth::DEPTH_LOOP)
		{
			_capturedMidiTargets = context.SelectedMidiLoops;
			if (_capturedMidiTargets.empty() && context.HoveredMidiLoop) _capturedMidiTargets.push_back(context.HoveredMidiLoop);
		}
		_feedbackSampleRate = context.SampleRate;
		_CaptureContext(context, childResolver);
		_capturedPhaseTarget = _ResolveMidiPhaseDragTarget(context, childResolver);
		_capturedFractionTargets = _ResolveFractionDragTargets(context, childResolver);
		_capturedDivisionGlobal = _capturedMidiTargets.empty() && _IsDivisionGlobalTarget(context, childResolver);
	}
	_ctrlHandleHeld = held;
	RefreshOverlay(context, childResolver);
}

void QuantiserController::RefreshOverlay(const QuantisationInteractionContext& context,
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

void QuantiserController::Tick(Time now)
{
	bool invalid = false;
	for (const auto& loop : _fractionMidiTargets) invalid = invalid || !_OwnerForMidiLoop(loop);
	for (const auto& loop : _midiPhaseDragTarget.MidiTargets) invalid = invalid || !_OwnerForMidiLoop(loop);
	for (const auto& take : _fractionDragTargets) invalid = invalid || !_StationForTake(take);
	for (const auto& take : _midiPhaseDragTarget.TakeTargets) invalid = invalid || !_StationForTake(take);
	for (const auto& station : _midiPhaseDragTarget.StationTargets)
		invalid = invalid || std::find(_stations.begin(), _stations.end(), station) == _stations.end();
	if (invalid) CancelInteraction();
	_ApplyCtrlHandleAlpha(_CtrlHandleAlpha(now));
}

std::optional<ActionResult> QuantiserController::TryHandleTouchAction(TouchAction action,
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
		|| !ctrlModifier || !_ctrlHandleHeld)
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

std::optional<ActionResult> QuantiserController::TryHandleTouchMove(TouchMoveAction action,
	unsigned int sampleRate)
{
	if (_isMidiPhaseDragging)
		return _UpdateMidiPhaseDrag(action, sampleRate);

	if (_isFractionDragging)
		return _UpdateFractionDrag(action);

	return std::nullopt;
}

void QuantiserController::_CaptureContext(const QuantisationInteractionContext& context,
	const ChildResolver& childResolver)
{
	CtrlOverlayContext captured;
	captured.Anchor = context.CursorPos;
	captured.VisibleButtonCount = context.SelectDepth == base::SelectDepth::DEPTH_LOOP && context.LoopDepthHasAudioTarget && _capturedMidiTargets.empty() ? 0 : _VisibleButtonCount(context);
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

float QuantiserController::_CtrlHandleAlpha(Time now) const
{
	if (utils::Timer::IsZero(_ctrlHandleReleasedAt))
		return 0.0f;
	const auto elapsed = utils::Timer::GetElapsedSeconds(_ctrlHandleReleasedAt, now);
	const bool visible = _ctrlHandleHeld || OwnsPointer();
	const auto duration = visible ? 0.120 : 0.420;
	const auto progress = static_cast<float>(std::clamp(elapsed / duration, 0.0, 1.0));
	const auto eased = progress * progress * (3.0f - 2.0f * progress);
	return _transitionStartAlpha + ((visible ? 1.0f : 0.0f) - _transitionStartAlpha) * eased;
}

void QuantiserController::_ApplyCtrlHandleAlpha(float alpha)
{
	_overlay.SetAlpha(alpha);
	if ((alpha <= 0.001f) && !_ctrlHandleHeld && !OwnsPointer())
		_ctrlOverlayContext = std::nullopt;
}

int QuantiserController::_VisibleButtonCount(const QuantisationInteractionContext& context) const
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

SelectDepth QuantiserController::_SelectDepth(const QuantisationInteractionContext& context) const noexcept
{
	if (_ctrlOverlayContext.has_value())
		return _ctrlOverlayContext->SelectDepth;

	return context.SelectDepth;
}

std::shared_ptr<GuiElement> QuantiserController::_HoverElement(const QuantisationInteractionContext& context,
	const ChildResolver& childResolver) const
{
	if (_ctrlOverlayContext.has_value())
		return childResolver(_ctrlOverlayContext->HoverPath);

	auto hovering = childResolver(context.HoverPath);
	if (!hovering && !context.HoverPath3d.empty())
		hovering = childResolver(context.HoverPath3d);
	return hovering;
}

void QuantiserController::_ApplyOverlayScopes(const QuantisationInteractionContext& context,
	const ChildResolver& childResolver)
{
	_overlay.SetButtonScope(0,
		(_ctrlOverlayContext ? _capturedPhaseTarget.Kind == MidiPhaseDragTargetKind::Global : _IsPhaseGlobalTarget(context, childResolver))
			? graphics::CtrlHandleOverlay::ButtonScope::Global
			: graphics::CtrlHandleOverlay::ButtonScope::Local);
	_overlay.SetButtonScope(1,
		(_ctrlOverlayContext ? _capturedDivisionGlobal : _IsDivisionGlobalTarget(context, childResolver))
			? graphics::CtrlHandleOverlay::ButtonScope::Global
			: graphics::CtrlHandleOverlay::ButtonScope::Local);
}

std::shared_ptr<Station> QuantiserController::_StationFromElement(const std::shared_ptr<GuiElement>& element) const
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

std::vector<std::shared_ptr<Station>> QuantiserController::_SelectedStations() const
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

std::vector<std::shared_ptr<LoopTake>> QuantiserController::_SelectedLoopTakes(base::SelectDepth depth) const
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

std::vector<std::shared_ptr<LoopTake>> QuantiserController::_AllLocalLoopTakes() const
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

std::vector<std::shared_ptr<LoopTake>> QuantiserController::_LoopTakesForStations(const std::vector<std::shared_ptr<Station>>& stations) const
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

bool QuantiserController::_IsPhaseGlobalTarget(const QuantisationInteractionContext& context,
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

bool QuantiserController::_IsDivisionGlobalTarget(const QuantisationInteractionContext& context,
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

ActionResult QuantiserController::_BeginMidiPhaseDrag(TouchAction action,
	const QuantisationInteractionContext& context,
	const ChildResolver& childResolver)
{
	const auto now = utils::Timer::IsZero(action.GetActionTime()) ? utils::Timer::GetTime() : action.GetActionTime();
	_transitionStartAlpha = _CtrlHandleAlpha(now);
	_ctrlHandleReleasedAt = now;
	_isMidiPhaseDragging = true;
	_overlay.SetActiveButton(0);
	_midiPhaseDragStartPosition = action.Position;
	_midiPhaseDragTarget = _ctrlOverlayContext ? _capturedPhaseTarget : _ResolveMidiPhaseDragTarget(context, childResolver);
	_midiPhaseDragStartOffsetSamps = _MidiPhaseOffsetForTarget(_midiPhaseDragTarget);
	_quantisation.SetGestureOverlayHeld(true);
	_quantisation.ApplyOverlayAlpha(1.0f, _stations);
	_ShowPhaseFeedback(_feedbackSampleRate);

	ActionResult res;
	res.IsEaten = true;
	res.ResultType = ACTIONRESULT_DEFAULT;
	return res;
}

ActionResult QuantiserController::_UpdateMidiPhaseDrag(TouchMoveAction action,
	unsigned int sampleRate)
{
	const auto delta = action.Position - _midiPhaseDragStartPosition;
	const auto offsetSamps = Quantiser::ResolvePhaseOffsetDrag(_midiPhaseDragStartOffsetSamps,
		delta.X,
		sampleRate);
	_SetMidiPhaseOffsetForTarget(_midiPhaseDragTarget, offsetSamps);
	_quantisation.ApplyOverlayAlpha(1.0f, _stations);
	_ShowPhaseFeedback(sampleRate);

	ActionResult res;
	res.IsEaten = true;
	res.ResultType = ACTIONRESULT_DEFAULT;
	return res;
}

ActionResult QuantiserController::_EndMidiPhaseDrag(TouchAction action,
	unsigned int sampleRate)
{
	if (_isMidiPhaseDragging)
	{
		const auto delta = action.Position - _midiPhaseDragStartPosition;
		const auto offsetSamps = Quantiser::ResolvePhaseOffsetDrag(_midiPhaseDragStartOffsetSamps,
			delta.X,
			sampleRate);
		_SetMidiPhaseOffsetForTarget(_midiPhaseDragTarget, offsetSamps);
	}

	const auto now = utils::Timer::GetTime();
	_transitionStartAlpha = _CtrlHandleAlpha(now);
	_ctrlHandleReleasedAt = now;
	_isMidiPhaseDragging = false;
	_overlay.SetActiveButton(-1);
	if (_feedbackSink) _feedbackSink("");
	_midiPhaseDragTarget = MidiPhaseDragTarget{};
	_quantisation.SetGestureOverlayHeld(false);
	_quantisation.PulseOverlay();
	_quantisation.ApplyOverlayAlpha(_quantisation.OverlayAlpha(utils::Timer::GetTime()), _stations);

	return ActionResult::NoAction();
}

ActionResult QuantiserController::_BeginFractionDrag(TouchAction action,
	const QuantisationInteractionContext& context,
	const ChildResolver& childResolver)
{
	_fractionMidiTargets = _capturedMidiTargets;
	_fractionDragTargets = _ctrlOverlayContext ? _capturedFractionTargets : _ResolveFractionDragTargets(context, childResolver);
	if (_fractionDragTargets.empty() && _fractionMidiTargets.empty())
		return ActionResult::NoAction();

	_fractionDragStartY = action.Position.Y;
	_fractionDragMoved = false;
	_fractionDragTake.reset();
	_fractionDragStartFraction = !_fractionMidiTargets.empty() ? _fractionMidiTargets.front()->Quantisation().Fraction : _fractionDragTargets.front()->MidiQuantisation().Fraction;


	const auto now = utils::Timer::IsZero(action.GetActionTime()) ? utils::Timer::GetTime() : action.GetActionTime();
	_transitionStartAlpha = _CtrlHandleAlpha(now);
	_ctrlHandleReleasedAt = now;
	_isFractionDragging = true;
	_overlay.SetActiveButton(1);
	_quantisation.SetGestureOverlayHeld(true);
	_ShowFractionFeedback();

	ActionResult res;
	res.IsEaten = true;
	res.ResultType = ACTIONRESULT_DEFAULT;
	return res;
}

ActionResult QuantiserController::_UpdateFractionDrag(TouchMoveAction action)
{
	if (!_isFractionDragging)
		return ActionResult::NoAction();

	_fractionDragMoved = true;

	if (_fractionDragTake)
	{
		auto res = _fractionDragTake->OnAction(action);
		return res;
	}

	if (_fractionDragTargets.empty() && _fractionMidiTargets.empty())
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

	for (const auto& loop : _fractionMidiTargets)
	{
		if (const auto owner = _OwnerForMidiLoop(loop))
		{
			auto settings = loop->GetLoopQuantisationOverride();
			settings.Enabled = true;
			settings.Fraction = fraction;
			owner->SetMidiLoopQuantisationOverride(loop, settings);
		}
	}
	_ShowFractionFeedback();
	ActionResult res;
	res.IsEaten = true;
	res.ResultType = ACTIONRESULT_DEFAULT;
	return res;
}

ActionResult QuantiserController::_EndFractionDrag(TouchAction action)
{
	auto take = _fractionDragTake;
	auto targets = _fractionDragTargets;
	auto midiTargets = _fractionMidiTargets;
	_fractionMidiTargets.clear();
	const auto moved = _fractionDragMoved;
	const auto now = utils::Timer::GetTime();
	_transitionStartAlpha = _CtrlHandleAlpha(now);
	_ctrlHandleReleasedAt = now;
	_isFractionDragging = false;
	_fractionDragStartY = 0;
	_fractionDragTake.reset();
	_fractionDragTargets.clear();
	_fractionDragMoved = false;
	_overlay.SetActiveButton(-1);
	_quantisation.SetGestureOverlayHeld(false);
	if (_feedbackSink) _feedbackSink("");

	if (take)
		return take->OnAction(action);

	if (targets.empty() && midiTargets.empty())
		return ActionResult::NoAction();

	if (!moved)
	{
		for (const auto& loop : midiTargets)
		{
			if (const auto owner = _OwnerForMidiLoop(loop))
			{
				auto settings = loop->GetLoopQuantisationOverride();
				settings.Enabled = !loop->Quantisation().Enabled;
				owner->SetMidiLoopQuantisationOverride(loop, settings);
			}
		}
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

void QuantiserController::_ForEachTake(const std::function<void(const std::shared_ptr<Station>& station,
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

std::shared_ptr<Station> QuantiserController::_StationForTake(const std::shared_ptr<LoopTake>& take) const
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

std::shared_ptr<LoopTake> QuantiserController::_TakeForLoop(const std::shared_ptr<Loop>& loop) const
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

std::shared_ptr<LoopTake> QuantiserController::_TakeFromElement(const std::shared_ptr<GuiElement>& element) const
{
	if (!element)
		return nullptr;

	auto take = std::dynamic_pointer_cast<LoopTake>(element);
	if (take)
		return take;

	return _TakeForLoop(std::dynamic_pointer_cast<Loop>(element));
}

std::shared_ptr<LoopTake> QuantiserController::_FirstTakeForStation(const std::shared_ptr<Station>& station) const
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

std::vector<std::shared_ptr<LoopTake>> QuantiserController::_ResolveFractionDragTargets(const QuantisationInteractionContext& context,
	const ChildResolver& childResolver) const
{
	const auto depth = _SelectDepth(context);
	if (depth == base::SelectDepth::DEPTH_LOOP && (!_capturedMidiTargets.empty() || context.LoopDepthHasAudioTarget)) return {};
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

QuantiserController::MidiPhaseDragTarget QuantiserController::_ResolveMidiPhaseDragTarget(
	const QuantisationInteractionContext& context,
	const ChildResolver& childResolver) const
{
	MidiPhaseDragTarget target;

	const auto depth = _SelectDepth(context);
	if (depth == base::SelectDepth::DEPTH_LOOP && !_capturedMidiTargets.empty())
	{
		target.Kind = MidiPhaseDragTargetKind::MidiLoop;
		target.MidiTargets = _capturedMidiTargets;
		return target;
	}
	if (depth == base::SelectDepth::DEPTH_LOOP && context.LoopDepthHasAudioTarget) return target;

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

std::int32_t QuantiserController::_MidiPhaseOffsetForTarget(const MidiPhaseDragTarget& target) const noexcept
{
	switch (target.Kind)
	{
	case MidiPhaseDragTargetKind::MidiLoop:
		return target.MidiTargets.empty() ? 0 : target.MidiTargets.front()->GetLoopQuantisationOverride().PhaseOffsetSamps.value_or(0);
	case MidiPhaseDragTargetKind::Station:
		return target.StationRef ? target.StationRef->StationPhaseOffsetSamps() : 0;
	case MidiPhaseDragTargetKind::LoopTake:
		return target.TakeRef ? target.TakeRef->MidiQuantisation().PhaseOffsetSamps : 0;
	case MidiPhaseDragTargetKind::Global:
	default:
		return _quantisation.GlobalPhaseOffsetSamps();
	}
}

void QuantiserController::_SetMidiPhaseOffsetForTarget(const MidiPhaseDragTarget& target,
	std::int32_t offsetSamps) noexcept
{
	switch (target.Kind)
	{
	case MidiPhaseDragTargetKind::MidiLoop:
		for (const auto& loop : target.MidiTargets)
		{
			if (const auto owner = _OwnerForMidiLoop(loop))
			{
				auto settings = loop->GetLoopQuantisationOverride();
				settings.PhaseOffsetSamps = offsetSamps;
				owner->SetMidiLoopQuantisationOverride(loop, settings);
			}
		}
		break;
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
} // namespace engine

void engine::QuantiserController::CancelInteraction(bool resetModifier)
{
	const auto now = utils::Timer::GetTime();
	_transitionStartAlpha = _CtrlHandleAlpha(now);
	_ctrlHandleReleasedAt = now;
	_isMidiPhaseDragging = false;
	_isFractionDragging = false;
	_fractionDragTake.reset();
	_fractionDragTargets.clear();
	_fractionMidiTargets.clear();
	_midiPhaseDragTarget = MidiPhaseDragTarget{};
	if (resetModifier)
	{
		_capturedMidiTargets.clear();
		_ctrlOverlayContext.reset();
		_capturedFractionTargets.clear();
		_capturedPhaseTarget = MidiPhaseDragTarget{};
		_ctrlHandleHeld = false;
	}
	_overlay.SetActiveButton(-1);
	_quantisation.SetGestureOverlayHeld(false);
	if (_feedbackSink) _feedbackSink("");
}

void engine::QuantiserController::_ShowPhaseFeedback(unsigned int sampleRate)
{
	if (!_feedbackSink) return;
	const auto offset = _MidiPhaseOffsetForTarget(_midiPhaseDragTarget);
	const char* scope = _midiPhaseDragTarget.Kind == MidiPhaseDragTargetKind::Global ? "Global" :
		(_midiPhaseDragTarget.Kind == MidiPhaseDragTargetKind::Station ? "Station" : (_midiPhaseDragTarget.Kind == MidiPhaseDragTargetKind::MidiLoop ? "Loop" : "Take"));
	std::ostringstream text;
	text << scope << " SHFT " << std::showpos;
	if (sampleRate > 0u) text << std::fixed << std::setprecision(2) << 1000.0 * offset / sampleRate << " ms (" << offset << " samples)";
	else text << offset << " samples";
	std::int32_t minimum = offset, maximum = offset;
	std::size_t count = 1u;
	auto include = [&](std::int32_t value) { minimum = (std::min)(minimum, value); maximum = (std::max)(maximum, value); };
	if (_midiPhaseDragTarget.Kind == MidiPhaseDragTargetKind::Station)
	{
		count = _midiPhaseDragTarget.StationTargets.size();
		for (const auto& station : _midiPhaseDragTarget.StationTargets) if (station) include(station->StationPhaseOffsetSamps());
	}
	else if (_midiPhaseDragTarget.Kind == MidiPhaseDragTargetKind::LoopTake)
	{
		count = _midiPhaseDragTarget.TakeTargets.size();
		for (const auto& take : _midiPhaseDragTarget.TakeTargets) if (take) include(take->MidiQuantisation().PhaseOffsetSamps);
	}
	else if (_midiPhaseDragTarget.Kind == MidiPhaseDragTargetKind::MidiLoop)
	{
		count = _midiPhaseDragTarget.MidiTargets.size();
		for (const auto& loop : _midiPhaseDragTarget.MidiTargets) if (loop) include(loop->GetLoopQuantisationOverride().PhaseOffsetSamps.value_or(0));
	}
	if (minimum != maximum) text << " mixed [" << minimum << ", " << maximum << "] samples";
	if (count > 1u) text << " / " << std::noshowpos << count << " targets";
	_feedbackSink(text.str());
}

void engine::QuantiserController::_ShowFractionFeedback()
{
	if (!_feedbackSink || (_fractionDragTargets.empty() && _fractionMidiTargets.empty())) return;
	const auto settings = !_fractionMidiTargets.empty() ? _fractionMidiTargets.front()->Quantisation() : _fractionDragTargets.front()->MidiQuantisation();
	const auto mixedTakes = std::any_of(_fractionDragTargets.begin(), _fractionDragTargets.end(), [&settings](const auto& take) {
		return take && (take->MidiQuantisation().Fraction != settings.Fraction || take->MidiQuantisation().Enabled != settings.Enabled);
	});
	std::ostringstream text;
	text << (!_fractionMidiTargets.empty() ? "Loop" : (_capturedDivisionGlobal ? "Global" : "Selection")) << " DIV " << midi::MidiQuantisation::FractionLabel(settings.Fraction)
		<< " (" << midi::MidiQuantisation::Divisor(settings.Fraction) << " divisions)";
	const auto mixedLoops = std::any_of(_fractionMidiTargets.begin(), _fractionMidiTargets.end(), [&settings](const auto& loop) {
		return loop && (loop->Quantisation().Fraction != settings.Fraction || loop->Quantisation().Enabled != settings.Enabled);
	});
	if (mixedTakes || mixedLoops) text << " mixed";
	if (!_fractionMidiTargets.empty()) text << " / " << _fractionMidiTargets.size() << " loops";
	if (_fractionDragTargets.size() > 1u) text << " / " << _fractionDragTargets.size() << " takes";
	_feedbackSink(text.str());
}

std::shared_ptr<engine::LoopTake> engine::QuantiserController::_OwnerForMidiLoop(const std::shared_ptr<midi::MidiLoop>& loop) const
{
	std::shared_ptr<LoopTake> owner;
	_ForEachTake([&](const auto& station, const auto& take) {
		if (!owner && !station->IsRemote())
		{
			const auto loops = take->GetMidiLoopSnapshot();
			if (std::find(loops.begin(), loops.end(), loop) != loops.end()) owner = take;
		}
	});
	return owner;
}
