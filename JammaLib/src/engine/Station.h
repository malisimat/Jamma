#pragma once

#include "LoopTake.h"
#include "Trigger.h"
#include "AudioSink.h"
#include "../audio/AudioMixer.h"
#include "../audio/AudioBuffer.h"
#include "../base/Jammable.h"
#include "../gui/GuiRack.h"

namespace engine
{
	class StationParams :
		public base::JammableParams
	{
	public:
		StationParams() :
			base::JammableParams(
				base::GuiElementParams(0, DrawableParams{ "" },
					MoveableParams(utils::Position2d{ 0, 0 }, utils::Position3d{ 0, 0, 0 }, 1.0),
					SizeableParams{ 1,1 },
					"",
					"",
					"",
					{})
				),
			Name(""),
			FadeSamps(constants::DefaultFadeSamps),
			NumBusChannels(0)
		{
		}

	public:
		std::string Name;
		unsigned int FadeSamps;
		unsigned int NumBusChannels;
	};
	
	class Station :
		public base::Tickable,
		public base::Jammable,
		public base::MultiAudioSink
	{
	public:
		enum StationPanelType
		{
			STATIONPANEL_MASTER,
			STATIONPANEL_MIXER,
			STATIONPANEL_ROUTER
		};

	public:
		Station(StationParams params,
			audio::AudioMixerParams mixerParams);
		~Station();

		// Copy
		Station(const Station&) = delete;
		Station& operator=(const Station&) = delete;

	public:
		static std::optional<std::shared_ptr<Station>> FromFile(StationParams stationParams,
			audio::AudioMixerParams mixerParams,
			io::JamFile::Station stationStruct,
			std::wstring dir);
		static audio::AudioMixerParams GetMixerParams(utils::Size2d stationSize,
			audio::BehaviourParams behaviour);

		virtual std::string ClassName() const override { return "Station"; }
		virtual MultiAudioPlugType MultiAudioPlug() const override { return MULTIAUDIOPLUG_BOTH; }
		virtual void SetSize(utils::Size2d size) override;
		virtual	utils::Position2d Position() const override;
		virtual unsigned int NumInputChannels(base::Audible::AudioSourceType source) const override;
		virtual unsigned int NumOutputChannels(base::Audible::AudioSourceType source) const override;
		virtual void Zero(unsigned int numSamps,
			Audible::AudioSourceType source) override;
		void WriteBlock(const std::shared_ptr<base::MultiAudioSink> dest,
			const std::shared_ptr<Trigger> trigger,
			int indexOffset,
			unsigned int numSamps);
		virtual void EndMultiPlay(unsigned int numSamps) override;
		virtual void OnBlockWriteChannel(unsigned int channel,
			const base::AudioWriteRequest& request,
			int writeOffset) override;
		virtual void EndMultiWrite(unsigned int numSamps,
			bool updateIndex,
			Audible::AudioSourceType source) override;
		virtual void SetSelectDepth(base::SelectDepth depth) override;
		virtual actions::ActionResult OnAction(actions::KeyAction action) override;
		virtual actions::ActionResult OnAction(actions::GuiAction action) override;
		virtual actions::ActionResult OnAction(actions::TriggerAction action) override;
		virtual void OnTick(Time curTime,
			unsigned int samps,
			std::optional<io::UserConfig> cfg,
			std::optional<audio::AudioStreamParams> params) override;
		virtual void Reset() override;
		
		const std::vector<std::shared_ptr<LoopTake>>& GetLoopTakes() const { return _loopTakes; }
		std::shared_ptr<LoopTake> AddTake();
		void AddTake(std::shared_ptr<LoopTake> take);
		void AddTrigger(std::shared_ptr<Trigger> trigger);
		unsigned int NumTakes() const;
		std::string Name() const;
		void SetName(std::string name);
		void SetClock(std::shared_ptr<Timer> clock);
		void SetupBuffers(unsigned int bufSize);
		void SetNumBusChannels(unsigned int chans);
		void SetNumAdcChannels(unsigned int chans);
		void SetNumDacChannels(unsigned int chans);
		unsigned int NumBusChannels() const;
		void OnBounce(unsigned int numSamps, io::UserConfig config);
		void SetRackVisibility(bool showStationRack, bool showLoopTakeRacks);

	protected:
		static unsigned int _CalcTakeHeight(unsigned int stationHeight, unsigned int numTakes);

		virtual void _InitReceivers() override;
		virtual std::vector<actions::JobAction> _CommitChanges() override;
		virtual const std::shared_ptr<base::AudioSink> _InputChannel(unsigned int channel,
			Audible::AudioSourceType source) override;
		virtual void _ArrangeChildren() override;
		void _CollapseOtherTakeRouters();
		void _CollapseOtherTakeRoutersToChannels();

		gui::GuiRackParams _GetRackParams(utils::Size2d size);
		std::optional<std::shared_ptr<LoopTake>> _TryGetTake(std::string id);
		void _WireVuSliders();

	protected:
		static const utils::Size2d _Gap;
		static const unsigned int _DefaultNumBusChannels;

		bool _flipTakeBuffer;
		bool _flipAudioBuffer;
		std::string _name;
		unsigned int _fadeSamps;
		unsigned int _lastBufSize;
		std::shared_ptr<Timer> _clock;
		std::shared_ptr<gui::GuiRack> _guiRack;
		std::shared_ptr<audio::AudioMixer> _masterMixer;
		std::shared_ptr<gui::GuiToggle> _mixerToggle;
		std::shared_ptr<gui::GuiToggle> _routerToggle;
		std::shared_ptr<gui::GuiRouter> _router;
		std::vector<std::shared_ptr<LoopTake>> _loopTakes;
		std::vector<std::shared_ptr<Trigger>> _triggers;
		std::vector<std::shared_ptr<LoopTake>> _backLoopTakes;
		std::vector<std::shared_ptr<audio::AudioMixer>> _audioMixers;
		std::vector<std::shared_ptr<audio::AudioMixer>> _backAudioMixers;
		std::vector<std::shared_ptr<audio::AudioBuffer>> _audioBuffers;
		std::vector<std::shared_ptr<audio::AudioBuffer>> _backAudioBuffers;
	};
}
