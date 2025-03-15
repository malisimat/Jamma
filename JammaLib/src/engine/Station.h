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
			FadeSamps(constants::DefaultFadeSamps)
		{
		}

	public:
		std::string Name;
		unsigned int FadeSamps;
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
		virtual unsigned int NumOutputChannels() const override;
		virtual unsigned int NumInputChannels() const override;
		virtual void Zero(unsigned int numSamps,
			Audible::AudioSourceType source) override;
		virtual void OnPlay(const std::shared_ptr<base::MultiAudioSink> dest,
			const std::shared_ptr<Trigger> trigger,
			int indexOffset,
			unsigned int numSamps) override;
		virtual void EndMultiPlay(unsigned int numSamps) override;
		virtual void OnWriteChannel(unsigned int channel,
			const std::shared_ptr<base::AudioSource> src,
			int indexOffset,
			unsigned int numSamps,
			Audible::AudioSourceType source);
		virtual void EndMultiWrite(unsigned int numSamps,
			bool updateIndex,
			Audible::AudioSourceType source) override;
		virtual actions::ActionResult OnAction(actions::KeyAction action) override;
		virtual actions::ActionResult OnAction(actions::GuiAction action) override;
		virtual actions::ActionResult OnAction(actions::TriggerAction action) override;
		virtual void OnTick(Time curTime,
			unsigned int samps,
			std::optional<io::UserConfig> cfg,
			std::optional<audio::AudioStreamParams> params) override;
		virtual void Reset() override;
		
		std::shared_ptr<LoopTake> AddTake();
		void AddTake(std::shared_ptr<LoopTake> take);
		void AddTrigger(std::shared_ptr<Trigger> trigger);
		unsigned int NumTakes() const;
		std::string Name() const;
		void SetName(std::string name);
		void SetClock(std::shared_ptr<Timer> clock);
		void SetupBuffers(unsigned int chans, unsigned int bufSize);
		unsigned int BufSize() const;
		void OnBounce(unsigned int numSamps, io::UserConfig config);

	protected:
		static unsigned int _CalcTakeHeight(unsigned int stationHeight, unsigned int numTakes);

		virtual void _InitReceivers() override;
		virtual std::vector<actions::JobAction> _CommitChanges() override;
		virtual const std::shared_ptr<base::AudioSink> _InputChannel(unsigned int channel,
			Audible::AudioSourceType source) override;
		virtual void _ArrangeChildren() override;

		gui::GuiRackParams _GetRackParams(utils::Size2d size);
		std::optional<std::shared_ptr<LoopTake>> _TryGetTake(std::string id);

	protected:
		static const utils::Size2d _Gap;

		bool _flipTakeBuffer;
		bool _flipAudioBuffer;
		std::string _name;
		unsigned int _fadeSamps;
		std::shared_ptr<Timer> _clock;
		std::shared_ptr<gui::GuiRack> _guiRack;
		std::shared_ptr<audio::AudioMixer> _mixer;
		std::vector<std::shared_ptr<audio::AudioMixer>> _audioMixers;
		std::vector<std::shared_ptr<audio::AudioMixer>> _backAudioMixers;
		std::shared_ptr<gui::GuiToggle> _mixerToggle;
		std::shared_ptr<gui::GuiToggle> _routerToggle;
		std::shared_ptr<gui::GuiRouter> _router;
		std::vector<std::shared_ptr<LoopTake>> _loopTakes;
		std::vector<std::shared_ptr<Trigger>> _triggers;
		std::vector<std::shared_ptr<LoopTake>> _backLoopTakes;
		std::vector<std::shared_ptr<audio::AudioBuffer>> _audioBuffers;
		std::vector<std::shared_ptr<audio::AudioBuffer>> _backAudioBuffers;
	};
}
