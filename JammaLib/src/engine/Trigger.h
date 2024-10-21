#pragma once

#include <vector>
#include <optional>
#include "ActionReceiver.h"
#include "GuiElement.h"
#include "Tickable.h"
#include "Timer.h"
#include "../actions/KeyAction.h"
#include "../actions/TriggerAction.h"
#include "../actions/ActionResult.h"
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
		std::string TakeId;
	};

	enum TriggerSource
	{
		TRIGGER_NOTSET,
		TRIGGER_KEY,
		TRIGGER_MIDI
	};

	class TriggerBinding
	{
	public:
		TriggerBinding() :
			TriggerSource(TRIGGER_NOTSET),
			Value(0),
			State(0)
		{
		}

		TriggerBinding(TriggerSource source,
			unsigned int value,
			unsigned int state) :
			TriggerSource(source),
			Value(value),
			State(state)
		{
		}

	public:
		bool Test(TriggerSource source,
			unsigned int value,
			unsigned int state)
		{
			if ((TRIGGER_NOTSET != TriggerSource) &&
				(source == TriggerSource) &&
				(value == Value) &&
				(state == State))
				return true;

			return false;
		}

		bool operator==(const TriggerBinding& other) {
			if (TriggerSource != other.TriggerSource)
				return false;

			if (Value != other.Value)
				return false;

			return State == other.State;
		}

	public:
		TriggerSource TriggerSource;
		unsigned int Value;
		unsigned int State;
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
					_triggerDown.State > 0 ? 0 : 1);
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
					trigRelease.State > 0 ? 0 : 1);
			}
		}

		TestResult OnTrigger(TriggerSource source,
			unsigned int value,
			unsigned int state)
		{
			if (!_isDown)
			{
				if (_triggerDown.Test(source, value, state))
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
					if (trigRelease.Test(source, value, state))
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

	class TriggerParams :
		public base::GuiElementParams
	{
	public:
		TriggerParams() :
			base::GuiElementParams(DrawableParams{ "" },
			MoveableParams(utils::Position2d{ 0, 0 }, utils::Position3d{ 0, 0, 0 }, 1.0),
			SizeableParams{ 1,1 },
			"",
			"",
			"",
			{})
		{};

	public:
		std::string Name;
		std::vector<DualBinding> Activate;
		std::vector<DualBinding> Ditch;
		std::vector<unsigned int> InputChannels;
		std::string TextureRecording;
		std::string TextureDitchDown;
		std::string TextureOverdubbing;
		std::string TexturePunchedIn;
		unsigned int DebounceMs;
	};

	enum TriggerState
	{
		TRIGSTATE_DEFAULT,
		TRIGSTATE_RECORDING,
		TRIGSTATE_DITCHDOWN,
		TRIGSTATE_OVERDUBBING,
		TRIGSTATE_PUNCHEDIN
	};
	
	class Trigger :
		public base::Tickable,
		public base::GuiElement
	{
	public:
		Trigger(TriggerParams trigParams);
		~Trigger();

	public:
		static std::optional<std::shared_ptr<Trigger>> FromFile(TriggerParams trigParams, io::RigFile::Trigger trigStruct);

		virtual	utils::Position2d Position() const override;
		virtual actions::ActionResult OnAction(actions::KeyAction action) override;
		virtual void OnTick(Time curTime,
			unsigned int samps,
			std::optional<io::UserConfig> cfg,
			std::optional<audio::AudioStreamParams> params) override;
		virtual void Draw(base::DrawContext& ctx) override;

		void AddBinding(DualBinding activate, DualBinding ditch);
		void RemoveBinding(DualBinding activate, DualBinding ditch);
		void ClearBindings();
		void AddInputChannel(unsigned int chan);
		void RemoveInputChannel(unsigned int chan);
		void ClearInputChannels();
		TriggerState GetState() const;
		void Reset();
		std::string Name() const;
		void SetName(std::string name);

	protected:
		virtual void _InitResources(resources::ResourceLib& resourceLib, bool forceInit) override;
		virtual void _ReleaseResources() override;

	private:
		bool IgnoreRepeats(bool isActivate,
			DualBinding::TestResult trigResult);
		bool Debounce(bool isActivate,
			DualBinding::TestResult trigResult,
			Time acionTime);
		bool TryChangeState(DualBinding& binding,
			bool isActivate,
			const actions::KeyAction& action,
			int keyState);
		bool StateMachine(bool isDown,
			bool isActivate,
			std::optional<io::UserConfig> cfg,
			std::optional<audio::AudioStreamParams> params);

		// Only call from state machine
		void StartRecording(std::optional<io::UserConfig> cfg, std::optional<audio::AudioStreamParams> params);
		void EndRecording(std::optional<io::UserConfig> cfg, std::optional<audio::AudioStreamParams> params);
		void SetDitchDown(std::optional<io::UserConfig> cfg, std::optional<audio::AudioStreamParams> params);
		void Ditch(std::optional<io::UserConfig> cfg, std::optional<audio::AudioStreamParams> params);
		void StartOverdub(std::optional<io::UserConfig> cfg, std::optional<audio::AudioStreamParams> params);
		void EndOverdub(std::optional<io::UserConfig> cfg, std::optional<audio::AudioStreamParams> params);
		void DitchOverdub(std::optional<io::UserConfig> cfg, std::optional<audio::AudioStreamParams> params);
		void StartPunchIn(std::optional<io::UserConfig> cfg, std::optional<audio::AudioStreamParams> params);
		void EndPunchIn(std::optional<io::UserConfig> cfg, std::optional<audio::AudioStreamParams> params);

	private:
		std::string _name;
		double _debounceTimeMs;
		std::vector<DualBinding> _activateBindings;
		std::vector<DualBinding> _ditchBindings;
		std::vector<unsigned int> _inputChannels;
		TriggerState _state;
		std::string _overdubSourceId;
		unsigned long _recordSampCount;
		Time _lastActivateTime;
		Time _lastDitchTime;
		bool _isLastActivateDown;
		bool _isLastDitchDown;
		bool _isLastActivateDownRaw;
		bool _isLastDitchDownRaw;
		graphics::Image _textureRecording;
		graphics::Image _textureDitchDown;
		graphics::Image _textureOverdubbing;
		graphics::Image _texturePunchedIn;
		std::vector<TriggerTake> _lastLoopTakes;
	};
}
