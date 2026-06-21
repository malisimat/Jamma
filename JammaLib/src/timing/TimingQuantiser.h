#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>
#include "../include/Constants.h"
#include "../utils/Timer.h"
#include "../actions/ActionResult.h"
#include "../actions/TouchAction.h"
#include "../actions/TouchMoveAction.h"
#include "../graphics/CtrlHandleOverlay.h"
#include "../base/Jammable.h"
#include "../utils/CommonTypes.h"

namespace base
{
	class GuiElement;
	class Jammable;
}

namespace engine
{
	class Loop;
	class LoopTake;
	class Station;
}

namespace io
{
	struct UserConfig;
}

namespace ninjam
{
	struct NinjamRemoteSnapshot;
	class NinjamSession;
}

namespace midi
{
	enum class MidiQuantisationFraction : std::uint8_t;
}

namespace timing
{
	struct QuantisationParams
	{
		unsigned int SeedSamps = 0u;
		unsigned int MasterSamps = 0u;
	};

	struct QuantisationLoopTakeVisual
	{
		unsigned long LoopLengthSamps = 0ul;
		std::uint32_t GrainSamps = 0u;
		std::uint32_t LoopGrains = 0u;
		double LoopIndexFrac = 0.0;
		float YCenter = 0.0f;
		float HalfHeight = 0.0f;
		float Radius = 0.0f;
		midi::MidiQuantisationFraction Fraction;
		std::int32_t PhaseOffsetSamps = 0;
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

	class TimingQuantiser
	{
	public:
		TimingQuantiser() = default;

		void SetClock(std::shared_ptr<utils::Timer> clock);
		void SetSeedUsesPowers(bool seedUsesPowers) noexcept;
		void Set(unsigned int samps, utils::Timer::QuantisationType type);
		void Clear(bool clearTapTempo);
		void ArmReclock();
		void ApplyTiming(const QuantisationTiming& timing, const char* source);

		void SetMidiGrain(unsigned int grainSamps,
			const char* source,
			const std::vector<std::shared_ptr<engine::Station>>& stations);
		void SetGlobalPhaseOffsetSamps(std::int32_t offsetSamps,
			const std::vector<std::shared_ptr<engine::Station>>& stations);

		bool HandleTapTempo(std::uint64_t estimatedSampleAt,
			unsigned int sampleRate,
			const std::vector<std::shared_ptr<engine::Station>>& stations,
			const io::UserConfig& cfg);

		bool TrySetMasterFromHover(const std::shared_ptr<base::GuiElement>& hovering,
			unsigned int depth,
			const std::vector<std::shared_ptr<engine::Station>>& stations,
			unsigned int sampleRate,
			const io::UserConfig& cfg,
			bool confirm);

		void UpdateStationHints(const std::shared_ptr<base::GuiElement>& candidate,
			unsigned int depth,
			bool confirmCandidate,
			const std::vector<std::shared_ptr<engine::Station>>& stations);

		void ClearStationHints(const std::vector<std::shared_ptr<engine::Station>>& stations);

		void PulseOverlay();
		void SetOverlayHeld(bool held);
		void ClearOverlay() noexcept;
		float OverlayAlpha(Time now) const;
		void ApplyOverlayAlpha(float alpha,
			const std::vector<std::shared_ptr<engine::Station>>& stations);

		void ApplyRemoteTempo(const ninjam::NinjamRemoteSnapshot& snapshot,
			const std::vector<std::shared_ptr<engine::Station>>& stations,
			const io::UserConfig& cfg);

		void QueueLocalTempo(unsigned int remoteSampleRate,
			unsigned int audioDeviceSampleRate,
			const io::UserConfig& cfg);

		void SendQueuedTempo(const ninjam::NinjamRemoteSnapshot& snapshot,
			ninjam::NinjamSession* ninjam,
			unsigned int remoteSampleRate,
			unsigned int audioDeviceSampleRate);

		unsigned int EffectiveSamps() const noexcept;
		std::int32_t GlobalPhaseOffsetSamps() const noexcept;
		bool IsArmedForReclock() const noexcept;
		std::shared_ptr<utils::Timer> Clock() const noexcept;
		unsigned int RemoteSampleRate() const noexcept;

		static unsigned int MinSeedSamps(unsigned int sampleRate,
			const QuantisationPolicy& policy);
		static std::int32_t ResolvePhaseOffsetDrag(std::int32_t startOffsetSamps,
			int deltaX,
			unsigned int sampleRate) noexcept;
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
		static constexpr int PhaseOffsetDragPixelsPerMillisecond = 1;
		static constexpr std::int64_t StateInactive = 0LL;
		static constexpr std::int64_t StateHeld = (std::numeric_limits<std::int64_t>::max)();
		static std::int32_t _ClampPhaseOffset(std::int64_t offsetSamps) noexcept;

		static unsigned int _ClampToUInt(unsigned long value);
		static unsigned int _RoundedToUInt(double value);
		static unsigned long _SnapSeedToMasterDivisor(unsigned long requestedSeedSamps,
			unsigned long masterLoopSamps);
		static std::optional<QuantisationTiming> _TimingFromSeed(unsigned int seedSamps,
			unsigned long masterLoopSamps,
			unsigned int sampleRate);

		struct InteractionTarget
		{
			std::shared_ptr<engine::Station> StationRef;
			std::shared_ptr<engine::LoopTake> TakeRef;
			std::shared_ptr<engine::Loop> LoopRef;
			unsigned long MasterLengthSamps = 0ul;
			std::shared_ptr<engine::Loop> RepresentativeLoopRef;
		};

		std::optional<InteractionTarget> _ResolveInteractionTarget(
			const std::shared_ptr<base::GuiElement>& target,
			unsigned int depth,
			const std::vector<std::shared_ptr<engine::Station>>& stations) const;

		std::shared_ptr<utils::Timer> _clock;
		std::shared_ptr<engine::Loop> _masterLoop;
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
		std::int32_t _globalPhaseOffsetSamps = 0;
	};

	inline unsigned int MinSeedSamps(unsigned int sampleRate, const QuantisationPolicy& policy)
	{
		return TimingQuantiser::MinSeedSamps(sampleRate, policy);
	}

	inline std::int32_t ResolvePhaseOffsetDrag(std::int32_t startOffsetSamps,
		int deltaX,
		unsigned int sampleRate) noexcept
	{
		return TimingQuantiser::ResolvePhaseOffsetDrag(startOffsetSamps, deltaX, sampleRate);
	}

	inline unsigned int IntervalSampsFromTempo(float bpm, unsigned int bpi, unsigned int sampleRate)
	{
		return TimingQuantiser::IntervalSampsFromTempo(bpm, bpi, sampleRate);
	}

	inline std::optional<QuantisationTiming> TimingFromSeedAndMaster(unsigned int seedSamps,
		unsigned long masterSamps,
		unsigned int sampleRate)
	{
		return TimingQuantiser::TimingFromSeedAndMaster(seedSamps, masterSamps, sampleRate);
	}

	inline std::optional<QuantisationTiming> DeduceSeedTiming(unsigned long masterLoopSamps,
		unsigned int sampleRate,
		const QuantisationPolicy& policy)
	{
		return TimingQuantiser::DeduceSeedTiming(masterLoopSamps, sampleRate, policy);
	}

	inline std::optional<QuantisationTiming> DeduceTapSeedTiming(unsigned long requestedSeedSamps,
		unsigned int sampleRate,
		const QuantisationPolicy& policy)
	{
		return TimingQuantiser::DeduceTapSeedTiming(requestedSeedSamps, sampleRate, policy);
	}

	inline std::optional<QuantisationTiming> DeduceTapSeedTimingFromMaster(unsigned long tapGapSamps,
		unsigned long masterLoopSamps,
		unsigned int sampleRate)
	{
		return TimingQuantiser::DeduceTapSeedTimingFromMaster(tapGapSamps, masterLoopSamps, sampleRate);
	}

	// ── TimingQuantiserController (merged from QuantisationInteractionController) ───

	struct QuantisationInteractionContext
	{
		utils::Position2d CursorPos{};
		utils::Size2d ViewportSize{};
		base::SelectDepth SelectDepth = base::SelectDepth::DEPTH_STATION;
		std::vector<unsigned char> HoverPath;
		std::vector<unsigned char> HoverPath3d;
	};

	class TimingQuantiserController
	{
	public:
		using ChildResolver = std::function<std::shared_ptr<base::GuiElement>(const std::vector<unsigned char>& path)>;

		TimingQuantiserController(graphics::CtrlHandleOverlay& overlay,
			TimingQuantiser& quantisation,
			std::vector<std::shared_ptr<engine::Station>>& stations);

		void OnCtrlModifierChanged(bool held,
			Time now,
			const QuantisationInteractionContext& context,
			const ChildResolver& childResolver);
		void RefreshOverlay(const QuantisationInteractionContext& context,
			const ChildResolver& childResolver);
		void Tick(Time now);

		std::optional<actions::ActionResult> TryHandleTouchAction(actions::TouchAction action,
			unsigned int sampleRate,
			bool ctrlModifier,
			const QuantisationInteractionContext& context,
			const ChildResolver& childResolver);
		std::optional<actions::ActionResult> TryHandleTouchMove(actions::TouchMoveAction action,
			unsigned int sampleRate);

	private:
		enum class MidiPhaseDragTargetKind : std::uint8_t
		{
			Global,
			Station,
			LoopTake
		};

		struct MidiPhaseDragTarget
		{
			MidiPhaseDragTargetKind Kind = MidiPhaseDragTargetKind::Global;
			std::shared_ptr<engine::Station> StationRef;
			std::shared_ptr<engine::LoopTake> TakeRef;
			std::vector<std::shared_ptr<engine::Station>> StationTargets;
			std::vector<std::shared_ptr<engine::LoopTake>> TakeTargets;
		};

		struct CtrlOverlayContext
		{
			utils::Position2d Anchor{};
			int VisibleButtonCount = 1;
			std::vector<unsigned char> HoverPath;
			base::SelectDepth SelectDepth = base::SelectDepth::DEPTH_STATION;
		};

		void _CaptureContext(const QuantisationInteractionContext& context,
			const ChildResolver& childResolver);
		float _CtrlHandleAlpha(Time now) const;
		void _ApplyCtrlHandleAlpha(float alpha);
		int _VisibleButtonCount(const QuantisationInteractionContext& context) const;
		base::SelectDepth _SelectDepth(const QuantisationInteractionContext& context) const noexcept;
		std::shared_ptr<base::GuiElement> _HoverElement(const QuantisationInteractionContext& context,
			const ChildResolver& childResolver) const;
		void _ApplyOverlayScopes(const QuantisationInteractionContext& context,
			const ChildResolver& childResolver);
		std::shared_ptr<engine::Station> _StationFromElement(const std::shared_ptr<base::GuiElement>& element) const;
		std::vector<std::shared_ptr<engine::Station>> _SelectedStations() const;
		std::vector<std::shared_ptr<engine::LoopTake>> _SelectedLoopTakes(base::SelectDepth depth) const;
		std::vector<std::shared_ptr<engine::LoopTake>> _AllLocalLoopTakes() const;
		std::vector<std::shared_ptr<engine::LoopTake>> _LoopTakesForStations(const std::vector<std::shared_ptr<engine::Station>>& stations) const;
		bool _IsPhaseGlobalTarget(const QuantisationInteractionContext& context,
			const ChildResolver& childResolver) const;
		bool _IsDivisionGlobalTarget(const QuantisationInteractionContext& context,
			const ChildResolver& childResolver) const;

		actions::ActionResult _BeginMidiPhaseDrag(actions::TouchAction action,
			const QuantisationInteractionContext& context,
			const ChildResolver& childResolver);
		actions::ActionResult _UpdateMidiPhaseDrag(actions::TouchMoveAction action,
			unsigned int sampleRate);
		actions::ActionResult _EndMidiPhaseDrag(actions::TouchAction action,
			unsigned int sampleRate);

		actions::ActionResult _BeginFractionDrag(actions::TouchAction action,
			const QuantisationInteractionContext& context,
			const ChildResolver& childResolver);
		actions::ActionResult _UpdateFractionDrag(actions::TouchMoveAction action);
		actions::ActionResult _EndFractionDrag(actions::TouchAction action);

		void _ForEachTake(const std::function<void(const std::shared_ptr<engine::Station>& station,
			const std::shared_ptr<engine::LoopTake>& take)>& visit) const;
		std::shared_ptr<engine::Station> _StationForTake(const std::shared_ptr<engine::LoopTake>& take) const;
		std::shared_ptr<engine::LoopTake> _TakeForLoop(const std::shared_ptr<engine::Loop>& loop) const;
		std::shared_ptr<engine::LoopTake> _TakeFromElement(const std::shared_ptr<base::GuiElement>& element) const;
		std::shared_ptr<engine::LoopTake> _FirstTakeForStation(const std::shared_ptr<engine::Station>& station) const;
		std::vector<std::shared_ptr<engine::LoopTake>> _ResolveFractionDragTargets(const QuantisationInteractionContext& context,
			const ChildResolver& childResolver) const;
		MidiPhaseDragTarget _ResolveMidiPhaseDragTarget(
			const QuantisationInteractionContext& context,
			const ChildResolver& childResolver) const;
		std::int32_t _MidiPhaseOffsetForTarget(const MidiPhaseDragTarget& target) const noexcept;
		void _SetMidiPhaseOffsetForTarget(const MidiPhaseDragTarget& target,
			std::int32_t offsetSamps) noexcept;

		graphics::CtrlHandleOverlay& _overlay;
		TimingQuantiser& _quantisation;
		std::vector<std::shared_ptr<engine::Station>>& _stations;

		bool _ctrlHandleHeld = false;
		Time _ctrlHandleReleasedAt;
		std::optional<CtrlOverlayContext> _ctrlOverlayContext;
		bool _isMidiPhaseDragging = false;
		utils::Position2d _midiPhaseDragStartPosition;
		std::int32_t _midiPhaseDragStartOffsetSamps = 0;
		MidiPhaseDragTarget _midiPhaseDragTarget;
		bool _isFractionDragging = false;
		int _fractionDragStartY = 0;
		std::shared_ptr<engine::LoopTake> _fractionDragTake;
		std::vector<std::shared_ptr<engine::LoopTake>> _fractionDragTargets;
		midi::MidiQuantisationFraction _fractionDragStartFraction;
		bool _fractionDragMoved = false;
	};
}
