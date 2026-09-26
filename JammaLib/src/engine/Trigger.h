#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>
#include <optional>
#include "ActionSender.h"
#include "Tickable.h"
#include "../utils/Timer.h"
#include "../midi/MidiEvent.h"
#include "../midi/MidiQueue.h"
#include "../actions/KeyAction.h"
#include "../actions/TriggerAction.h"
#include "../actions/ActionResult.h"
#include "../actions/DelayedAction.h"
#include "../audio/AudioMixer.h"
#include "../base/BounceWriter.h"
#include "../base/TriggerPunchTarget.h"
#include "../io/RigFile.h"

namespace engine
{
	struct TriggerTake
	{
		enum SourceType
		{
			SOURCE_ADC,
			SOURCE_LOOPTAKE,
			SOURCE_STATION
		};
		SourceType SourceType;
		std::string SourceTakeId;
		std::string TargetTakeId;
		std::shared_ptr<base::ActionReceiver> Receiver;
	};

	enum TriggerSource
	{
		TRIGGER_NOTSET,
		TRIGGER_KEY,
		TRIGGER_MIDI,
		TRIGGER_SERIAL
	};

	enum TriggerInputDomain : std::uint8_t { TRIGGER_INPUT_UI, TRIGGER_INPUT_JOB };
	enum TriggerControl : std::uint8_t { TRIGGER_CONTROL_ACTIVATE, TRIGGER_CONTROL_DITCH };
	enum TriggerEdge : std::uint8_t { TRIGGER_EDGE_DOWN, TRIGGER_EDGE_UP };

	struct TriggerInputEdge
	{
		std::uint64_t RigRevision = 0u;
		std::uint16_t BindingIndex = 0u;
		TriggerControl Control = TRIGGER_CONTROL_ACTIVATE;
		TriggerEdge Edge = TRIGGER_EDGE_UP;
		std::int64_t EventTimeUsec = 0;
	};

	class TriggerBinding
	{
	public:
		TriggerBinding() :
			TriggerSource(TRIGGER_NOTSET),
			Value(0),
			State(0),
			Device()
		{
		}

		TriggerBinding(TriggerSource source,
			unsigned int value,
			unsigned int state,
			std::string device = "") :
			TriggerSource(source),
			Value(value),
			State(state),
			Device(std::move(device))
		{
		}

	public:
		bool Test(TriggerSource source,
			unsigned int value,
			unsigned int state,
			const std::string& device = "") const
		{
			if ((TRIGGER_NOTSET != TriggerSource) &&
				(source == TriggerSource) &&
				(value == Value) &&
				(state == State) &&
				(Device.empty() || (Device == device)))
				return true;

			return false;
		}

		bool operator==(const TriggerBinding& other) {
			if (TriggerSource != other.TriggerSource)
				return false;

			if (Value != other.Value)
				return false;

			if (State != other.State)
				return false;

			return Device == other.Device;
		}

	public:
		TriggerSource TriggerSource;
		unsigned int Value;
		unsigned int State;
		std::string Device;
	};

	class DualBinding
	{
	public:
		enum TestResult
		{
			MATCH_NONE,
			MATCH_DOWN,
			MATCH_RELEASE
		};

	public:
		DualBinding() :
			_isDown(false)
		{
		};
		DualBinding(TriggerBinding downBinding,
			TriggerBinding releaseBinding) :
			_isDown(false)
		{
			SetDown(downBinding, false);
			SetRelease(releaseBinding, false);
		};

	public:
		void SetDown(TriggerBinding binding,
			bool inferRelease)
		{
			_triggerDown = binding;

			if (inferRelease)
			{
				_triggerRelease = TriggerBinding(_triggerDown.TriggerSource,
					_triggerDown.Value,
					_triggerDown.State > 0 ? 0 : 1,
					_triggerDown.Device);
			}
		}

		void SetRelease(TriggerBinding binding,
			bool inferDown)
		{
			_triggerRelease = binding;

			if (inferDown && _triggerRelease.has_value())
			{
				auto trigRelease = _triggerRelease.value();
				_triggerDown = TriggerBinding(trigRelease.TriggerSource,
					trigRelease.Value,
					trigRelease.State > 0 ? 0 : 1,
					trigRelease.Device);
			}
		}

		TestResult OnTrigger(TriggerSource source,
			unsigned int value,
			unsigned int state,
			const std::string& device = "")
		{
			if (!_isDown)
			{
				if (_triggerDown.Test(source, value, state, device))
				{
					if (_triggerRelease.has_value())
						_isDown = true;

					return MATCH_DOWN;
				}
			}
			else
			{
				if (_triggerRelease.has_value())
				{
					auto trigRelease = _triggerRelease.value();
					if (trigRelease.Test(source, value, state, device))
					{
						_isDown = false;
						return MATCH_RELEASE;
					}
				}
			}

			return MATCH_NONE;
		}

		TestResult Match(TriggerSource source,
			unsigned int value,
			unsigned int state,
			const std::string& device = "") const
		{
			if (_triggerDown.Test(source, value, state, device))
				return MATCH_DOWN;
			if (_triggerRelease.has_value() && _triggerRelease->Test(source, value, state, device))
				return MATCH_RELEASE;
			return MATCH_NONE;
		}

		TestResult ApplyResolved(TestResult edge)
		{
			if (edge == MATCH_DOWN && !_isDown)
			{
				if (_triggerRelease.has_value()) _isDown = true;
				return MATCH_DOWN;
			}
			if (edge == MATCH_RELEASE && _isDown)
			{
				_isDown = false;
				return MATCH_RELEASE;
			}
			return MATCH_NONE;
		}

		void Reset() { _isDown = false;	}

		bool operator==(const DualBinding& other) {
			if (!(_triggerDown == other._triggerDown))
				return false;

			if (_triggerRelease.has_value() != other._triggerRelease.has_value())
				return false;

			if (!_triggerRelease.has_value())
				return true;

			return _triggerRelease.value() == other._triggerRelease.value();
		}

	private:
		TriggerBinding _triggerDown;
		std::optional<TriggerBinding> _triggerRelease;
		bool _isDown;
	};

	struct TriggerParams
	{
		unsigned int Index = 0u;
		std::string Name;
		std::vector<DualBinding> Activate;
		std::vector<DualBinding> Ditch;
		std::vector<unsigned int> InputChannels;
		std::vector<std::string> MidiInputDevices;
		unsigned int DebounceMs = 0u;
	};

	enum TriggerState
	{
		TRIGSTATE_DEFAULT,
		TRIGSTATE_RECORDING,
		TRIGSTATE_OVERDUBBING,
		TRIGSTATE_PUNCHEDIN
	};

	class Trigger :
		public base::Tickable,
		public base::ActionSender,
		public base::BounceWriter
	{
	public:
		static constexpr std::size_t MaxBindingCount = 64u;
		Trigger(TriggerParams trigParams);
		~Trigger();

	public:
		static std::optional<std::shared_ptr<Trigger>> FromFile(TriggerParams trigParams,
			io::RigFile::Trigger trigStruct);
		static audio::BounceMixBehaviourParams GetOverdubBehaviourParams(std::vector<unsigned int> channels);
		static audio::AudioMixerParams GetOverdubMixerParams(std::vector<unsigned int> channels);
		static std::shared_ptr<base::BounceWriter> CreateBounceWriter(
			const std::shared_ptr<audio::AudioMixer>& mixer);
		static const char* ActionLabel(actions::ActionResultType rt) noexcept;

		actions::ActionResult OnAction(actions::KeyAction action);
		actions::ActionResult OnEvent(const midi::MidiEvent& event,
			const base::Action& action);
		actions::ActionResult OnEvent(TriggerSource source,
			unsigned int value,
			unsigned int state,
			const base::Action& action,
			const std::string& device = "");
		actions::ActionResult QueueExternalControlAction(bool isActivate,
			bool isDown,
			const base::Action& action,
			std::uint64_t rigRevision = 0u);
		actions::ActionResult QueueInputEvent(TriggerInputDomain domain,
			std::uint64_t rigRevision,
			TriggerSource source,
			unsigned int value,
			unsigned int state,
			const base::Action& action,
			const std::string& device = "");
		actions::ActionResult QueueMidiInputEvent(TriggerInputDomain domain,
			std::uint64_t rigRevision,
			const midi::MidiEvent& event,
			const base::Action& action);
		virtual void OnTick(Time curTime,
			unsigned int samps,
			const std::optional<io::UserConfig>& cfg,
			const std::optional<audio::AudioStreamParams>& params) override;
		void OnTick(Time curTime,
			unsigned int samps,
			const std::optional<io::UserConfig>& cfg,
			const std::optional<audio::AudioStreamParams>& params,
			std::uint64_t rigRevision);
		std::uint64_t UiInputDropCount() const noexcept { return _uiInputQueue.DroppedCount(); }
		std::uint64_t JobInputDropCount() const noexcept { return _jobInputQueue.DroppedCount(); }
		std::uint64_t StructuralCommandDropCount() const noexcept { return _structuralCommandDropCount.load(std::memory_order_acquire); }
		std::uint64_t ActivationOutcomeCount() const noexcept { return _activationOutcomeCount.load(std::memory_order_acquire); }
		std::uint64_t DitchOutcomeCount() const noexcept { return _ditchOutcomeCount.load(std::memory_order_acquire); }
		// Scene job-thread consumer for fixed-size structural commands emitted by
		// the audio-owned state machine. The caller serializes this with Station
		// edits by holding the Scene mutex.
		void ProcessStructuralActionsOnJob(
			const std::optional<io::UserConfig>& cfg,
			const std::optional<audio::AudioStreamParams>& params);

		bool AddBinding(DualBinding activate, DualBinding ditch);
		void RemoveBinding(DualBinding activate, DualBinding ditch);
		void ClearBindings();
		void AddInputChannel(unsigned int chan);
		void RemoveInputChannel(unsigned int chan);
		void ClearInputChannels();
		void AddMidiInputDevice(std::string device);
		// Called at an audio boundary. The prepared containers are exchanged, so
		// capture-route changes do not allocate on the real-time path.
		void ApplyCaptureRouting(std::shared_ptr<base::ActionReceiver>& receiver,
			std::vector<unsigned int>& inputChannels,
			std::vector<std::string>& midiInputDevices,
			io::RigFile::Trigger::MidiInputMode& midiInputMode,
			std::shared_ptr<audio::AudioMixer>& overdubMixer,
			std::shared_ptr<base::BounceWriter>& overdubWriter) noexcept;
		const std::vector<std::string>& MidiInputDevices() const noexcept { return _midiInputDevices; }
		io::RigFile::Trigger::MidiInputMode MidiInputMode() const noexcept { return _midiInputMode; }
		TriggerState GetState() const;
		bool IsActivateInputDown() const;
		bool IsDitchInputDown() const;
		bool IsDitchDown() const;
		// Audio-boundary predicate used before replacing a trigger.
		bool CanEditRouting() const noexcept;
		// A capture route or station target update retains this trigger and its take
		// history, so it needs only an idle input/state boundary.
		bool CanApplyCaptureRouting() const noexcept;
		void Reset();
		std::string Name() const;
		void SetName(std::string name);
		std::vector<TriggerTake> GetTakes() const;
		void WriteBlock(const std::shared_ptr<base::MultiAudioSink> dest,
			const float* srcBuf,
			unsigned int numSamps,
			unsigned int destChannel) override;

	protected:
		void _UpdateBehaviour();

	private:
		static constexpr std::uint8_t MidiCcStatus = 0xB0u;
		static constexpr unsigned int MidiBindingKindShift = 12u;
		static constexpr unsigned int MidiBindingChannelShift = 8u;
		static unsigned int EncodeMidiBindingValue(io::RigFile::MidiTriggerEvent kind,
			unsigned int channel,
			unsigned int id);
		static DualBinding MakeMidiBinding(io::RigFile::MidiTriggerEvent kind,
			unsigned int channel,
			unsigned int id,
			unsigned int state);
		static void AddMidiBindingForChannels(const io::RigFile::Trigger::MidiTriggerBindingSpec& bindingSpec,
			const std::function<void(const DualBinding&)>& onBinding);
		static bool IsValidMidiBindingSpec(const io::RigFile::Trigger::MidiTriggerBindingSpec& bindingSpec);
		static bool TryEncodeMidiEvent(const midi::MidiEvent& event,
			unsigned int& outValue,
			unsigned int& outState);
		bool IgnoreRepeats(bool isActivate,
			DualBinding::TestResult trigResult);
		bool Debounce(bool isActivate,
			DualBinding::TestResult trigResult,
			Time acionTime);
		bool TryChangeState(DualBinding& binding,
			bool isActivate,
			TriggerSource source,
			unsigned int value,
			unsigned int state,
			const base::Action& action,
			const std::string& device);
		bool StateMachine(bool isDown,
			bool isActivate,
			const std::optional<io::UserConfig>& cfg,
			const std::optional<audio::AudioStreamParams>& params);
		void _ProcessQueuedInputActions(std::uint64_t rigRevision,
			const std::optional<io::UserConfig>& cfg,
			const std::optional<audio::AudioStreamParams>& params) noexcept;
		bool _ApplyInputEdge(const TriggerInputEdge& edge,
			const std::optional<io::UserConfig>& cfg,
			const std::optional<audio::AudioStreamParams>& params) noexcept;
		bool _CanEditRoutingAtAudioBoundary() const noexcept;
		bool _CanApplyCaptureRoutingAtAudioBoundary() const noexcept;
		void _PublishTriggerStateSnapshot() noexcept;

		// Only call from state machine
		void StartRecording(const std::optional<io::UserConfig>& cfg, const std::optional<audio::AudioStreamParams>& params);
		void EndRecording(const std::optional<io::UserConfig>& cfg, const std::optional<audio::AudioStreamParams>& params);
		void SetDitchDown(const std::optional<io::UserConfig>& cfg, const std::optional<audio::AudioStreamParams>& params);
		void SetDitchUp(const std::optional<io::UserConfig>& cfg, const std::optional<audio::AudioStreamParams>& params);
		void Ditch(const std::optional<io::UserConfig>& cfg, const std::optional<audio::AudioStreamParams>& params);
		void StartOverdub(const std::optional<io::UserConfig>& cfg, const std::optional<audio::AudioStreamParams>& params);
		void EndOverdub(const std::optional<io::UserConfig>& cfg, const std::optional<audio::AudioStreamParams>& params);
		void DitchOverdub(const std::optional<io::UserConfig>& cfg, const std::optional<audio::AudioStreamParams>& params);
		bool StartPunchIn(const std::optional<io::UserConfig>& cfg, const std::optional<audio::AudioStreamParams>& params);
		bool EndPunchIn(const std::optional<io::UserConfig>& cfg, const std::optional<audio::AudioStreamParams>& params);
		unsigned int CalcInputAlignedDelaySamps(const std::optional<io::UserConfig>& cfg,
			const std::optional<audio::AudioStreamParams>& params) const;
		unsigned int CalcPunchStateDelaySamps(const std::optional<io::UserConfig>& cfg) const;
		enum StructuralCompletion : std::uint8_t
		{
			STRUCTURAL_NONE,
			STRUCTURAL_START_RECORDING,
			STRUCTURAL_END_RECORDING,
			STRUCTURAL_DITCH,
			STRUCTURAL_START_OVERDUB,
			STRUCTURAL_END_OVERDUB,
			STRUCTURAL_DITCH_OVERDUB
		};
		struct StructuralCommand
		{
			std::uint64_t Sequence = 0u;
			actions::TriggerAction::TriggerActionType ActionType = actions::TriggerAction::TRIGGER_REC_START;
			StructuralCompletion Completion = STRUCTURAL_NONE;
			std::uint64_t HistoryToken = 0u;
			unsigned long SampleCount = 0u;
			bool ApplyToTargetTake = true;
			bool ApplyToSourceTake = true;
			bool ApplyToTargetAudio = true;
			bool ApplyToTargetMidi = true;
		};
		struct StructuralResult
		{
			std::uint64_t Sequence = 0u;
			StructuralCompletion Completion = STRUCTURAL_NONE;
			bool IsEaten = false;
			actions::DitchDisposition DitchResult = actions::DitchDisposition::NotApplicable;
			std::uint64_t HistoryToken = 0u;
			base::TriggerPunchTarget* SourceTake = nullptr;
			base::TriggerPunchTarget* TargetTake = nullptr;
		};
		struct RuntimeTriggerTake
		{
			decltype(TriggerTake{}.SourceType) SourceType = TriggerTake::SOURCE_ADC;
			std::uint64_t Token = 0u;
			base::TriggerPunchTarget* SourceTake = nullptr;
			base::TriggerPunchTarget* TargetTake = nullptr;
		};
		struct DelayedMixerAction
		{
			unsigned int SampsLeft = 0u;
			double Target = 0.0;
		};
		struct DelayedPunchAction
		{
			base::TriggerPunchTarget* TargetTake = nullptr;
			unsigned int SampsLeft = 0u;
			bool IsPunchIn = false;
		};
		bool _QueueStructuralCommand(actions::TriggerAction::TriggerActionType actionType,
			StructuralCompletion completion,
			std::uint64_t historyToken,
			unsigned long sampleCount,
			bool applyToTargetTake = true,
			bool applyToSourceTake = true,
			bool applyToTargetAudio = true,
			bool applyToTargetMidi = true) noexcept;
		void _FlushDelayedPunchActions(unsigned int samps) noexcept;
		void _ProcessStructuralResults() noexcept;
		void _ApplyStructuralResult(const StructuralResult& result) noexcept;
		void _EraseHistory(std::size_t index) noexcept;
		std::optional<std::size_t> _FindJobHistory(std::uint64_t token) const noexcept;
		void _PublishJobHistory();

	private:
		std::string _name;
		double _debounceTimeMs;
		std::vector<DualBinding> _activateBindings;
		std::vector<DualBinding> _ditchBindings;
		static constexpr std::uint16_t _DirectBindingIndex = (std::numeric_limits<std::uint16_t>::max)();
		static constexpr std::size_t _InputQueueCapacity = 64u;
		static constexpr std::size_t _InputFallbackBindingCapacity = MaxBindingCount;
		static constexpr std::size_t _InputFallbackBindingsPerControl = _InputFallbackBindingCapacity + 1u;
		static constexpr std::size_t _InputFallbackCount = _InputFallbackBindingsPerControl * 2u;
		static constexpr std::size_t _HistoryCapacity = 64u;
		static constexpr std::size_t _DelayedActionCapacity = 64u;
		static constexpr std::size_t _StructuralQueueCapacity = 64u;
		struct InputFallback
		{
			void Publish(const TriggerInputEdge& edge) noexcept;
			bool ReadLatest(std::uint64_t consumedSequence,
				TriggerInputEdge& edge,
				std::uint64_t& observedSequence) const noexcept;
			std::atomic<std::uint64_t> Sequence{ 0u };
			std::atomic<std::uint64_t> RigRevision{ 0u };
			std::atomic<std::uint16_t> BindingIndex{ 0u };
			std::atomic<std::uint8_t> Control{ 0u };
			std::atomic<std::uint8_t> Edge{ 0u };
			std::atomic<std::int64_t> EventTimeUsec{ 0 };
		};
		static std::size_t _InputFallbackIndex(const TriggerInputEdge& edge) noexcept;
		void _PublishInputFallback(TriggerInputDomain domain, const TriggerInputEdge& edge) noexcept;
		bool _InputFallbacksConsumed(const std::array<InputFallback, _InputFallbackCount>& fallbacks,
			const std::array<std::uint64_t, _InputFallbackCount>& consumedSequences) const noexcept;
		static_assert(std::atomic<std::uint64_t>::is_always_lock_free &&
			std::atomic<std::int64_t>::is_always_lock_free &&
			std::atomic<std::uint16_t>::is_always_lock_free &&
			std::atomic<std::uint8_t>::is_always_lock_free,
			"Trigger ingress fallback must remain lock-free on the audio path");
		midi::MidiQueue<_InputQueueCapacity, TriggerInputEdge> _uiInputQueue;
		midi::MidiQueue<_InputQueueCapacity, TriggerInputEdge> _jobInputQueue;
		std::array<InputFallback, _InputFallbackCount> _uiInputFallbacks{};
		std::array<InputFallback, _InputFallbackCount> _jobInputFallbacks{};
		std::atomic<std::uint64_t> _uiFallbackPublicationCount{ 0u };
		std::atomic<std::uint64_t> _jobFallbackPublicationCount{ 0u };
		std::array<std::uint64_t, _InputFallbackCount> _consumedUiFallbackSequences{};
		std::array<std::uint64_t, _InputFallbackCount> _consumedJobFallbackSequences{};
		std::uint64_t _consumedUiFallbackPublicationCount = 0u;
		std::uint64_t _consumedJobFallbackPublicationCount = 0u;
		std::vector<unsigned int> _inputChannels;
		std::vector<std::string> _midiInputDevices;
		io::RigFile::Trigger::MidiInputMode _midiInputMode = io::RigFile::Trigger::MidiInputMode::None;
		TriggerState _state;
		std::atomic<std::uint8_t> _publishedTriggerState{ static_cast<std::uint8_t>(TRIGSTATE_DEFAULT) };
		std::atomic<bool> _publishedActivateInputDown{ false };
		std::atomic<bool> _publishedDitchInputDown{ false };
		std::atomic<bool> _publishedTriggerDitchDown{ false };
		std::atomic<bool> _publishedCanEditRouting{ true };
		std::atomic<bool> _publishedCanApplyCaptureRouting{ true };
		std::atomic<std::uint64_t> _activationOutcomeCount{ 0u };
		std::atomic<std::uint64_t> _ditchOutcomeCount{ 0u };
		std::string _overdubSourceId;
		// Written by audio thread (OnTick) and read by event-handler threads
		// (key/MIDI/serial pumps) during state transitions. Atomic load/store
		// (relaxed) gives a coherent snapshot without locks on the RT path.
		std::atomic<unsigned long> _recordSampCount;
		Time _lastActivateTime;
		Time _lastDitchTime;
		bool _isDitchDown;
		bool _isLastActivateDown;
		bool _isLastDitchDown;
		bool _isLastActivateDownRaw;
		bool _isLastDitchDownRaw;
		std::array<RuntimeTriggerTake, _HistoryCapacity> _loopTakeHistory{};
		std::size_t _loopTakeHistorySize = 0u;
		std::optional<std::size_t> _activeHistoryIndex;
		std::array<DelayedMixerAction, _DelayedActionCapacity> _delayedActions{};
		std::size_t _delayedActionCount = 0u;
		std::array<DelayedPunchAction, _DelayedActionCapacity> _delayedPunchActions{};
		std::size_t _delayedPunchActionCount = 0u;
		std::array<DelayedMixerAction, _DelayedActionCapacity> _pendingDitchDelayedActions{};
		std::size_t _pendingDitchDelayedActionCount = 0u;
		std::array<DelayedPunchAction, _DelayedActionCapacity> _pendingDitchDelayedPunchActions{};
		std::size_t _pendingDitchDelayedPunchActionCount = 0u;
		midi::MidiQueue<_StructuralQueueCapacity, StructuralCommand> _structuralCommands;
		midi::MidiQueue<_StructuralQueueCapacity, StructuralResult> _structuralResults;
		std::atomic<std::uint32_t> _jobStructuralActionsInFlight{ 0u };
		std::atomic<std::uint64_t> _structuralCommandDropCount{ 0u };
		StructuralCompletion _pendingCompletion = STRUCTURAL_NONE;
		std::uint64_t _pendingSequence = 0u;
		std::uint64_t _pendingHistoryToken = 0u;
		TriggerState _pendingPriorState = TRIGSTATE_DEFAULT;
		std::optional<std::size_t> _pendingPriorActiveIndex;
		unsigned long _pendingRecordSamps = 0u;
		std::uint64_t _nextStructuralSequence = 1u;
		std::uint64_t _nextHistoryToken = 1u;
		// Job-thread-only variable-size metadata. Audio history uses only stable
		// tokens and raw non-owning punch targets retained by this ledger/Station.
		std::array<TriggerTake, _HistoryCapacity> _jobTakeHistory{};
		std::array<std::uint64_t, _HistoryCapacity> _jobTakeTokens{};
		std::size_t _jobTakeHistorySize = 0u;
		std::atomic<std::shared_ptr<const std::vector<TriggerTake>>> _publishedTakeHistory;
		std::shared_ptr<audio::AudioMixer> _overdubMixer;
		std::shared_ptr<base::BounceWriter> _overdubWriter;
	};
}
