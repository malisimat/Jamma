#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <optional>
#include "ActionSender.h"
#include "Tickable.h"
#include "../utils/Timer.h"
#include "../midi/MidiEvent.h"
#include "../actions/KeyAction.h"
#include "../actions/TriggerAction.h"
#include "../actions/ActionResult.h"
#include "../actions/DelayedAction.h"
#include "../audio/AudioMixer.h"
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
	};

	enum TriggerSource
	{
		TRIGGER_NOTSET,
		TRIGGER_KEY,
		TRIGGER_MIDI,
		TRIGGER_SERIAL
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
			const std::string& device = "")
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

	struct DelayedTriggerAction
	{
		actions::TriggerAction Action;
		unsigned int SampsLeft;
	};
	
	class Trigger :
		public base::Tickable,
		public base::ActionSender
	{
	public:
		Trigger(TriggerParams trigParams);
		~Trigger();

	public:
		static std::optional<std::shared_ptr<Trigger>> FromFile(TriggerParams trigParams,
			io::RigFile::Trigger trigStruct);
		static audio::BounceMixBehaviourParams GetOverdubBehaviourParams(std::vector<unsigned int> channels);
		static audio::AudioMixerParams GetOverdubMixerParams(std::vector<unsigned int> channels);
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
			const base::Action& action);
		virtual void OnTick(Time curTime,
			unsigned int samps,
			const std::optional<io::UserConfig>& cfg,
			const std::optional<audio::AudioStreamParams>& params) override;

		void AddBinding(DualBinding activate, DualBinding ditch);
		void RemoveBinding(DualBinding activate, DualBinding ditch);
		void ClearBindings();
		void AddInputChannel(unsigned int chan);
		void RemoveInputChannel(unsigned int chan);
		void ClearInputChannels();
		void AddMidiInputDevice(std::string device);
		const std::vector<std::string>& MidiInputDevices() const noexcept { return _midiInputDevices; }
		TriggerState GetState() const;
		bool IsActivateInputDown() const;
		bool IsDitchInputDown() const;
		bool IsDitchDown() const;
		void Reset();
		std::string Name() const;
		void SetName(std::string name);
		std::vector<TriggerTake> GetTakes() const;
		void WriteBlock(const std::shared_ptr<base::MultiAudioSink> dest,
			const float* srcBuf,
			unsigned int numSamps,
			unsigned int destChannel);

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
		void _ProcessQueuedExternalControlActions(const std::optional<io::UserConfig>& cfg,
			const std::optional<audio::AudioStreamParams>& params) noexcept;
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
		void StartPunchIn(const std::optional<io::UserConfig>& cfg, const std::optional<audio::AudioStreamParams>& params);
		void EndPunchIn(const std::optional<io::UserConfig>& cfg, const std::optional<audio::AudioStreamParams>& params);
		unsigned int CalcInputAlignedDelaySamps(const std::optional<io::UserConfig>& cfg,
			const std::optional<audio::AudioStreamParams>& params) const;
		unsigned int CalcPunchStateDelaySamps(const std::optional<io::UserConfig>& cfg) const;
		void QueueTriggerAction(const actions::TriggerAction& action, unsigned int sampsDelay);
		void DispatchTriggerAction(const actions::TriggerAction& action);
		void FlushDelayedTriggerActions(Time curTime,
			unsigned int samps,
			const std::optional<io::UserConfig>& cfg,
			const std::optional<audio::AudioStreamParams>& params);

	private:
		std::string _name;
		double _debounceTimeMs;
		std::vector<DualBinding> _activateBindings;
		std::vector<DualBinding> _ditchBindings;
		// External control thread produces edges; the audio thread consumes them.
		struct ExternalControlAction
		{
			bool IsActivate;
			bool IsDown;
		};
		static constexpr std::size_t _ExternalControlActionQueueCapacity = 64u;
		std::array<ExternalControlAction, _ExternalControlActionQueueCapacity> _externalControlActionQueue{};
		std::atomic<std::size_t> _externalControlActionHead{ 0u };
		std::atomic<std::size_t> _externalControlActionTail{ 0u };
		std::vector<unsigned int> _inputChannels;
		std::vector<std::string> _midiInputDevices;
		TriggerState _state;
		std::atomic<std::uint8_t> _publishedTriggerState{ static_cast<std::uint8_t>(TRIGSTATE_DEFAULT) };
		std::atomic<bool> _publishedActivateInputDown{ false };
		std::atomic<bool> _publishedDitchInputDown{ false };
		std::atomic<bool> _publishedTriggerDitchDown{ false };
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
		std::vector<TriggerTake> _loopTakeHistory;
		std::vector<actions::DelayedAction> _delayedActions;
		std::vector<DelayedTriggerAction> _delayedTriggerActions;
		std::shared_ptr<audio::AudioMixer> _overdubMixer;
	};
}
