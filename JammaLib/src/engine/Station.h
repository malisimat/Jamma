#pragma once

#include "LoopTake.h"
#include "Trigger.h"
#include "AudioSink.h"
#include "MultiAudioSource.h"
#include "MultiAudioSink.h"

namespace engine
{
	class StationParams :
		public base::GuiElementParams
	{
	public:
		StationParams() :
			base::GuiElementParams(DrawableParams{ "" },
				MoveableParams(utils::Position2d{ 0, 0 }, utils::Position3d{ 0, 0, 0 }, 1.0),
				SizeableParams{ 1,1 },
				"",
				"",
				"",
				{})
		{
		}
	};
	
	class Station :
		public base::Tickable,
		public base::GuiElement,
		public base::MultiAudioSource,
		public base::MultiAudioSink
	{
	public:
		Station(StationParams params);
		~Station();

		// Copy
		Station(const Station&) = delete;
		Station& operator=(const Station&) = delete;

	public:
		static std::optional<std::shared_ptr<Station>> FromFile(StationParams stationParams,
			io::JamFile::Station stationStruct,
			std::wstring dir);

		virtual std::string ClassName() const { return "Station"; }

		virtual void SetSize(utils::Size2d size) override;
		virtual	utils::Position2d Position() const override;
		virtual MultiAudioDirection MultiAudibleDirection() const override { return MULTIAUDIO_BOTH; }
		virtual void OnPlay(const std::shared_ptr<base::MultiAudioSink> dest, unsigned int numSamps) override;
		virtual void EndMultiPlay(unsigned int numSamps) override;
		// TODO: Remove OnWrite method
		virtual void OnWrite(const std::shared_ptr<base::MultiAudioSource> src, unsigned int numSamps) override;
		virtual void OnWriteChannel(unsigned int channel,
			const std::shared_ptr<base::AudioSource> src,
			unsigned int numSamps);
		virtual void EndMultiWrite(unsigned int numSamps, bool updateIndex) override;
		virtual actions::ActionResult OnAction(actions::KeyAction action) override;
		virtual actions::ActionResult OnAction(actions::TouchAction action) override;
		virtual actions::ActionResult OnAction(actions::TriggerAction action) override;
		virtual void OnTick(Time curTime,
			unsigned int samps,
			std::optional<io::UserConfig> cfg,
			std::optional<audio::AudioStreamParams> params) override;
		
		std::shared_ptr<LoopTake> AddTake();
		void AddTake(std::shared_ptr<LoopTake> take);
		void AddTrigger(std::shared_ptr<Trigger> trigger);
		void Reset();
		void SetClock(std::shared_ptr<Timer> clock);

	protected:
		static unsigned int CalcTakeHeight(unsigned int stationHeight, unsigned int numTakes);

		virtual std::vector<actions::JobAction> _CommitChanges() override;
		void ArrangeTakes();
		std::optional<std::shared_ptr<LoopTake>> TryGetTake(std::string id);

	protected:
		static const utils::Size2d _Gap;

		std::shared_ptr<Timer> _clock;
		std::vector<std::shared_ptr<LoopTake>> _loopTakes;
		std::vector<std::shared_ptr<Trigger>> _triggers;

		std::vector<std::shared_ptr<LoopTake>> _backLoopTakes;
	};
}
