#pragma once

#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>
#include "../include/Constants.h"
#include "Timer.h"

namespace base
{
	class GuiElement;
}

namespace io
{
	struct NinjamRemoteSnapshot;
	struct UserConfig;
}

namespace engine
{
	class Loop;
	class LoopTake;
	class NinjamSession;
	class Station;

	struct QuantisationParams
	{
		unsigned int SeedSamps = 0u;
		unsigned int MasterSamps = 0u;
	};

	struct QuantisationLoopTakeVisual
	{
		unsigned long LoopLengthSamps = 0ul;
		double LoopIndexFrac = 0.0;
		float YCenter = 0.0f;
		float HalfHeight = 0.0f;
		float Radius = 0.0f;
	};

	struct QuantisationPolicy
	{
		unsigned int SeedGrainMinMs = constants::DefaultSeedGrainMinMs;
		unsigned int SeedGrainTargetMaxMs = constants::DefaultSeedGrainTargetMaxMs;
		unsigned int SeedBpmMin = constants::DefaultSeedBpmMin;
		bool SeedUsesPowers = true;
	};

	struct QuantisationTiming
	{
		unsigned int SeedSamps = 0u;
		unsigned int MasterLoopSamps = 0u;
		unsigned int SeedCount = 0u;
		float Bpm = 0.0f;
		unsigned int Bpi = 0u;
	};

	// Accumulates tap events and maintains a running average beat-gap estimate.
	class TapTempoTracker
	{
	public:
		// Discards all tap history; call before starting a new tap-tempo session.
		void Clear() noexcept;

		// Records a tap at the given sample position and returns updated timing if
		// at least two taps have been received.  Call on each tap button press.
		std::optional<QuantisationTiming> TapAtSample(std::uint64_t samplePosition,
			unsigned int sampleRate,
			unsigned long masterLoopSamps,
			const QuantisationPolicy& policy);

		// Returns the current timing estimate without advancing the tracker. With a
		// master loop, the estimate snaps to a clean master divisor; without one,
		// it is treated as a standalone tap-derived seed.
		std::optional<QuantisationTiming> CurrentTiming(unsigned long masterLoopSamps,
			unsigned int sampleRate,
			const QuantisationPolicy& policy) const;

		// Returns true once at least two taps have been received and a gap estimate exists.
		bool HasEstimate() const noexcept;

	private:
		static constexpr double TapTimeoutSecs = 2.5;

		std::optional<std::uint64_t> _lastTapSample;
		std::optional<double> _estimatedGapSamps;
	};

	class Quantisation
	{
	public:
		Quantisation() = default;

		void SetClock(std::shared_ptr<Timer> clock);
		void SetSeedUsesPowers(bool seedUsesPowers) noexcept;
		void Set(unsigned int samps, Timer::QuantisationType type);
		void Clear(bool clearTapTempo);
		void ArmReclock();
		void ApplyTiming(const QuantisationTiming& timing, const char* source);

		void SetMidiGrain(unsigned int grainSamps,
			const char* source,
			const std::vector<std::shared_ptr<Station>>& stations);

		bool HandleTapTempo(std::uint64_t estimatedSampleAt,
			unsigned int sampleRate,
			const std::vector<std::shared_ptr<Station>>& stations,
			const io::UserConfig& cfg);

		bool TrySetMasterFromHover(const std::shared_ptr<base::GuiElement>& hovering,
			unsigned int depth,
			const std::vector<std::shared_ptr<Station>>& stations,
			unsigned int sampleRate,
			const io::UserConfig& cfg,
			bool confirm);

		void UpdateStationHints(const std::shared_ptr<base::GuiElement>& candidate,
			unsigned int depth,
			bool confirmCandidate,
			const std::vector<std::shared_ptr<Station>>& stations);

		void ClearStationHints(const std::vector<std::shared_ptr<Station>>& stations);

		void PulseOverlay();
		void SetOverlayHeld(bool held);
		void ClearOverlay() noexcept;
		float OverlayAlpha(Time now) const;
		void ApplyOverlayAlpha(float alpha,
			const std::vector<std::shared_ptr<Station>>& stations);

		void ApplyRemoteTempo(const io::NinjamRemoteSnapshot& snapshot,
			const std::vector<std::shared_ptr<Station>>& stations,
			const io::UserConfig& cfg);

		void QueueLocalTempo(unsigned int remoteSampleRate,
			unsigned int audioDeviceSampleRate,
			const io::UserConfig& cfg);

		void SendQueuedTempo(const io::NinjamRemoteSnapshot& snapshot,
			NinjamSession* ninjam,
			unsigned int remoteSampleRate,
			unsigned int audioDeviceSampleRate);

		unsigned int EffectiveSamps() const noexcept;
		bool IsArmedForReclock() const noexcept;
		std::shared_ptr<Timer> Clock() const noexcept;
		unsigned int RemoteSampleRate() const noexcept;

		static unsigned int MinSeedSamps(unsigned int sampleRate,
			const QuantisationPolicy& policy);
		static unsigned int IntervalSampsFromTempo(float bpm,
			unsigned int bpi,
			unsigned int sampleRate);
		static std::optional<QuantisationTiming> TimingFromSeedAndMaster(unsigned int seedSamps,
			unsigned long masterSamps,
			unsigned int sampleRate);
		static std::optional<QuantisationTiming> DeduceSeedTiming(unsigned long masterLoopSamps,
			unsigned int sampleRate,
			const QuantisationPolicy& policy);
		static std::optional<QuantisationTiming> DeduceTapSeedTiming(unsigned long requestedSeedSamps,
			unsigned int sampleRate,
			const QuantisationPolicy& policy);
		static std::optional<QuantisationTiming> DeduceTapSeedTimingFromMaster(unsigned long tapGapSamps,
			unsigned long masterLoopSamps,
			unsigned int sampleRate);

		static QuantisationPolicy Policy(const io::UserConfig& cfg);

	private:
		static constexpr double OverlayFadeSeconds = 2.0;
		static constexpr std::int64_t StateInactive = 0LL;
		static constexpr std::int64_t StateHeld = (std::numeric_limits<std::int64_t>::max)();

		static unsigned int _ClampToUInt(unsigned long value);
		static unsigned int _RoundedToUInt(double value);
		static unsigned long _SnapSeedToMasterDivisor(unsigned long requestedSeedSamps,
			unsigned long masterLoopSamps);
		static std::optional<QuantisationTiming> _TimingFromSeed(unsigned int seedSamps,
			unsigned long masterLoopSamps,
			unsigned int sampleRate);

		struct InteractionTarget
		{
			std::shared_ptr<Station> StationRef;
			std::shared_ptr<LoopTake> TakeRef;
			std::shared_ptr<Loop> LoopRef;
			unsigned long MasterLengthSamps = 0ul;
			std::shared_ptr<Loop> RepresentativeLoopRef;
		};

		std::optional<InteractionTarget> _ResolveInteractionTarget(
			const std::shared_ptr<base::GuiElement>& target,
			unsigned int depth,
			const std::vector<std::shared_ptr<Station>>& stations) const;

		std::shared_ptr<Timer> _clock;
		std::shared_ptr<Loop> _masterLoop;
		std::atomic_ulong _masterLoopLengthSamps{ 0ul };
		std::atomic_uint _effectiveQuantiseSamps{ 0u };
		std::atomic_bool _armReclock{ false };
		std::atomic_bool _hasPendingTempo{ false };
		std::atomic<std::int64_t> _overlayState{ StateInactive };
		std::mutex _tapTempoMutex;
		TapTempoTracker _tapTempo;
		unsigned int _remoteMasterLoopSamps = 0u;
		unsigned int _remoteSampleRate = 0u;
		unsigned int _lastRemoteIntervalPos = 0u;
		bool _seedUsesPowers = true;
	};

	inline unsigned int MinSeedSamps(unsigned int sampleRate, const QuantisationPolicy& policy)
	{
		return Quantisation::MinSeedSamps(sampleRate, policy);
	}

	inline unsigned int IntervalSampsFromTempo(float bpm, unsigned int bpi, unsigned int sampleRate)
	{
		return Quantisation::IntervalSampsFromTempo(bpm, bpi, sampleRate);
	}

	inline std::optional<QuantisationTiming> TimingFromSeedAndMaster(unsigned int seedSamps,
		unsigned long masterSamps,
		unsigned int sampleRate)
	{
		return Quantisation::TimingFromSeedAndMaster(seedSamps, masterSamps, sampleRate);
	}

	inline std::optional<QuantisationTiming> DeduceSeedTiming(unsigned long masterLoopSamps,
		unsigned int sampleRate,
		const QuantisationPolicy& policy)
	{
		return Quantisation::DeduceSeedTiming(masterLoopSamps, sampleRate, policy);
	}

	inline std::optional<QuantisationTiming> DeduceTapSeedTiming(unsigned long requestedSeedSamps,
		unsigned int sampleRate,
		const QuantisationPolicy& policy)
	{
		return Quantisation::DeduceTapSeedTiming(requestedSeedSamps, sampleRate, policy);
	}

	inline std::optional<QuantisationTiming> DeduceTapSeedTimingFromMaster(unsigned long tapGapSamps,
		unsigned long masterLoopSamps,
		unsigned int sampleRate)
	{
		return Quantisation::DeduceTapSeedTimingFromMaster(tapGapSamps, masterLoopSamps, sampleRate);
	}
}