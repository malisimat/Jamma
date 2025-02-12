#pragma once

#include <vector>
#include <memory>
#include "Loop.h"
#include "GuiElement.h"
#include "Tweakable.h"
#include "MultiAudioSource.h"
#include "MultiAudioSink.h"
#include "AudioSink.h"
#include "ActionUndo.h"
#include "Trigger.h"
#include "../audio/AudioMixer.h"
#include "../audio/AudioBuffer.h"

using base::Audible;

namespace engine
{
	class LoopTakeParams :
		public base::GuiElementParams,
		public base::TweakableParams
	{
	public:
		LoopTakeParams() :
			base::GuiElementParams(0, DrawableParams{ "" },
				MoveableParams(utils::Position2d{ 0, 0 }, utils::Position3d{ 0, 0, 0 }, 1.0),
				SizeableParams{ 1,1 },
				"",
				"",
				"",
				{}),
			Id(""),
			base::TweakableParams(),
			FadeSamps(constants::DefaultFadeSamps),
			Loops({})
		{
		}

		LoopTakeParams(base::GuiElementParams params,
			base::TweakableParams tweakParams,
			std::vector<LoopParams> loops) :
			base::GuiElementParams(params),
			base::TweakableParams(tweakParams),
			Id(""),
			FadeSamps(constants::DefaultFadeSamps),
			Loops(loops)
		{
		}

	public:
		std::string Id;
		unsigned int FadeSamps;
		std::vector<LoopParams> Loops;
	};

	class LoopTake :
		public base::GuiElement,
		public base::Tweakable,
		public base::MultiAudioSource,
		public base::MultiAudioSink
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
			STATE_INACTIVE,
			STATE_RECORDING,
			STATE_PLAYINGRECORDING,
			STATE_PLAYING,
			STATE_OVERDUBBING,
			STATE_PUNCHEDIN,
			STATE_OVERDUBBINGRECORDING
		};

	public:
		LoopTake(LoopTakeParams params,
			audio::AudioMixerParams mixerParams);
		~LoopTake();

		// Copy
		LoopTake(const LoopTake&) = delete;
		LoopTake& operator=(const LoopTake&) = delete;

	public:
		static std::optional<std::shared_ptr<LoopTake>> FromFile(LoopTakeParams takeParams,
			io::JamFile::LoopTake takeStruct,
			std::wstring dir);
		static audio::AudioMixerParams GetMixerParams(utils::Size2d loopSize,
			audio::BehaviourParams behaviour);

		virtual std::string ClassName() const { return "LoopTake"; }

		virtual void SetSize(utils::Size2d size) override;
		virtual MultiAudioPlugType MultiAudioPlug() const override { return MULTIAUDIOPLUG_NONE; }
		virtual unsigned int NumInputChannels() const override;
		virtual unsigned int NumOutputChannels() const override;
		virtual void Zero(unsigned int numSamps,
			Audible::AudioSourceType source) override;
		virtual void OnPlay(const std::shared_ptr<base::MultiAudioSink> dest,
			const std::shared_ptr<Trigger> trigger,
			int indexOffset,
			unsigned int numSamps) override;
		virtual void EndMultiPlay(unsigned int numSamps) override;
		virtual bool IsArmed() const override;
		virtual void EndMultiWrite(unsigned int numSamps,
			bool updateIndex,
			Audible::AudioSourceType source) override;
		virtual actions::ActionResult OnAction(actions::JobAction action) override;
		virtual bool Select() override;
		virtual bool DeSelect() override;
		virtual bool Mute() override;
		virtual bool UnMute() override;
		virtual void SetPickingFromState(EditMode mode, bool flipState) override;
		virtual void SetStateFromPicking(EditMode mode, bool flipState) override;

		std::string Id() const;
		std::string SourceId() const;
		LoopTakeSource TakeSourceType() const;
		LoopTakeState TakeState() const;
		unsigned long NumRecordedSamps() const;
		std::shared_ptr<Loop> AddLoop(unsigned int chan, std::string stationName);
		void AddLoop(std::shared_ptr<Loop> loop);
		void SetupBuffers(unsigned int chans, unsigned int bufSize);

		void Record(std::vector<unsigned int> channels, std::string stationName);
		void Play(unsigned long index,
			unsigned long loopLength,
			unsigned int endRecordSamps);
		void EndRecording();
		void Ditch();
		void Overdub(std::vector<unsigned int> channels, std::string stationName);
		void PunchIn();
		void PunchOut();

	protected:
		static unsigned int CalcLoopHeight(unsigned int takeHeight, unsigned int numLoops);

		virtual void _InitResources(resources::ResourceLib& resourceLib, bool forceInit) override;
		virtual std::vector<actions::JobAction> _CommitChanges() override;
		virtual const std::shared_ptr<base::AudioSink> InputChannel(unsigned int channel,
			base::AudioSource::AudioSourceType source);

		void ArrangeLoops();
		void UpdateLoops();

	protected:
		static const utils::Size2d _Gap;

		bool _flipLoopBuffer;
		bool _flipAudioBuffer;
		bool _loopsNeedUpdating;
		bool _endRecordingCompleted;
		LoopTakeState _state;
		std::string _id;
		std::string _sourceId;
		unsigned int _fadeSamps;
		LoopTakeSource _sourceType;
		unsigned long _recordedSampCount;
		unsigned int _endRecordSampCount;
		unsigned int _endRecordSamps;
		std::shared_ptr<audio::AudioMixer> _mixer;
		std::vector<std::shared_ptr<Loop>> _loops;
		std::vector<std::shared_ptr<Loop>> _backLoops;
		std::vector<std::shared_ptr<audio::AudioBuffer>> _audioBuffers;
		std::vector<std::shared_ptr<audio::AudioBuffer>> _backAudioBuffers;
	};
}
