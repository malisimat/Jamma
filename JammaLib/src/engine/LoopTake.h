#pragma once

#include <vector>
#include <memory>
#include "Loop.h"
#include "GuiElement.h"
#include "MultiAudioSource.h"
#include "MultiAudioSink.h"
#include "AudioSink.h"
#include "ActionUndo.h"
#include "Trigger.h"

namespace engine
{
	class LoopTakeParams :
		public base::GuiElementParams
	{
	public:
		LoopTakeParams() :
			base::GuiElementParams(DrawableParams{ "" },
				MoveableParams(utils::Position2d{ 0, 0 }, utils::Position3d{ 0, 0, 0 }, 1.0),
				SizeableParams{ 1,1 },
				"",
				"",
				"",
				{}),
			Id(""),
			Loops({})
		{
		}

		LoopTakeParams(base::GuiElementParams params,
			std::vector<LoopParams> loops) :
			base::GuiElementParams(params),
			Id(""),
			Loops(loops)
		{
		}

	public:
		std::string Id;
		std::vector<LoopParams> Loops;
	};

	class LoopTake :
		public virtual base::GuiElement,
		public virtual base::MultiAudioSource,
		public virtual base::MultiAudioSink
	{
	public:
		enum LoopTakeSource
		{
			SOURCE_ADC,
			SOURCE_STATION,
			SOURCE_LOOPTAKE
		};

		enum LoopTakeState
		{
			STATE_DEFAULT,
			STATE_RECORDING,
			STATE_PLAYINGRECORDING,
			STATE_PLAYING,
			STATE_OVERDUBBING,
			STATE_PUNCHEDIN
		};

	public:
		LoopTake(LoopTakeParams params);
		~LoopTake();

		// Copy
		LoopTake(const LoopTake&) = delete;
		LoopTake& operator=(const LoopTake&) = delete;

	public:
		static std::optional<std::shared_ptr<LoopTake>> FromFile(LoopTakeParams takeParams, io::JamFile::LoopTake takeStruct, std::wstring dir);

		virtual std::string ClassName() const { return "LoopTake"; }

		virtual void SetSize(utils::Size2d size) override;
		virtual MultiAudioDirection MultiAudibleDirection() const override { return MULTIAUDIO_BOTH; }
		virtual void OnPlay(const std::shared_ptr<base::MultiAudioSink> dest, unsigned int numSamps) override;
		virtual void EndMultiPlay(unsigned int numSamps) override;
		// TODO: Remove OnWrite method
		virtual void OnWrite(const std::shared_ptr<base::MultiAudioSource> src,
			unsigned int numSamps) override;
		virtual void OnWriteChannel(unsigned int channel,
			const std::shared_ptr<base::AudioSource> src,
			unsigned int numSamps);
		virtual void EndMultiWrite(unsigned int numSamps,
			bool updateIndex) override;
		virtual actions::ActionResult OnAction(actions::JobAction action) override;

		void OnPlayRaw(const std::shared_ptr<MultiAudioSink> dest,
			unsigned int delaySamps,
			unsigned int numSamps);		
		std::string Id() const;
		std::string SourceId() const;
		LoopTakeSource SourceType() const;
		unsigned long NumRecordedSamps() const;
		std::shared_ptr<Loop> AddLoop(unsigned int chan);
		void AddLoop(std::shared_ptr<Loop> loop);

		void Record(std::vector<unsigned int> channels);
		void Play(unsigned long index,
			unsigned long loopLength,
			unsigned int endRecordSamps);
		void EndRecording();
		void Ditch();
		void Overdub();
		void PunchIn();
		void PunchOut();

	protected:
		static unsigned int CalcLoopHeight(unsigned int takeHeight, unsigned int numLoops);

		virtual void _InitResources(resources::ResourceLib& resourceLib, bool forceInit) override;
		virtual std::vector<actions::JobAction> _CommitChanges() override;
		void ArrangeLoops();
		void UpdateLoops();

	protected:
		static const utils::Size2d _Gap;

		bool _flipLoopBuffer;
		bool _loopsNeedUpdating;
		bool _endRecordingCompleted;
		LoopTakeState _state;
		std::string _id;
		std::string _sourceId;
		LoopTakeSource _sourceType;
		unsigned long _recordedSampCount;
		unsigned int _endRecordSampCount;
		unsigned int _endRecordSamps;
		std::vector<std::shared_ptr<Loop>> _loops;
		std::vector<std::shared_ptr<Loop>> _backLoops;
	};
}
