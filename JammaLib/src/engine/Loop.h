#pragma once

#include <string>
#include <memory>
#include "MultiAudioSource.h"
#include "ActionReceiver.h"
#include "ResourceUser.h"
#include "GuiElement.h"
#include "GlUtils.h"
#include "VU.h"
#include "LoopModel.h"
#include "../gui/GuiModel.h"
#include "../io/FileReadWriter.h"
#include "../io/JamFile.h"
#include "../audio/BufferBank.h"
#include "../audio/AudioMixer.h"
#include "../graphics/GlDrawContext.h"
#include "../resources/WavResource.h"

namespace engine
{
	class LoopParams :
		public base::GuiElementParams
	{
	public:
		LoopParams() :
			base::GuiElementParams(DrawableParams{ "" },
				MoveableParams(utils::Position2d{ 0, 0 }, utils::Position3d{ 0, 0, 0 }, 1.0),
				SizeableParams{ 1,1 },
				"",
				"",
				"",
				{}),
			Id(""),
			TakeId(""),
			Wav(""),
			PlayTexture(""),
			RecordTexture(""),
			OverdubTexture(""),
			PunchTexture(""),
			Channel(0),
			FadeSamps(800u)
		{
		}

		LoopParams(base::GuiElementParams params,
			std::string wav) :
			base::GuiElementParams(params),
			Id(""),
			TakeId(""),
			Wav(wav),
			PlayTexture(""),
			RecordTexture(""),
			OverdubTexture(""),
			PunchTexture(""),
			Channel(0u),
			FadeSamps(800u)
		{
		}

	public:
		std::string Id;
		std::string TakeId;
		std::string Wav;
		std::string PlayTexture;
		std::string RecordTexture;
		std::string OverdubTexture;
		std::string PunchTexture;
		unsigned int Channel;
		unsigned int FadeSamps;
	};

	class Loop :
		public virtual base::GuiElement,
		public virtual base::AudioSink,
		public virtual base::MultiAudioSource
	{
	public:
		enum LoopVisualState
		{
			STATE_INACTIVE,
			STATE_RECORDING,
			STATE_PLAYINGRECORDING,
			STATE_PLAYING,
			STATE_OVERDUBBING,
			STATE_PUNCHEDIN
		};

	public:
		Loop(LoopParams loopParams,
			audio::AudioMixerParams mixerParams);
		~Loop() { ReleaseResources(); }

		// Copy
		Loop(const Loop&) = delete;
		Loop& operator=(const Loop&) = delete;

		// Move
		Loop(Loop&& other) :
			GuiElement(other._guiParams),
			_lastPeak(other._lastPeak),
			_pitch(other._pitch),
			_loopLength(other._loopLength),
			_state(other._state),
			_playIndex(other._playIndex),
			_loopParams{other._loopParams},
			_mixer(std::move(other._mixer)),
			_model(std::move(other._model)),
			_vu(std::move(other._vu)),
			_bufferBank(std::move(other._bufferBank))
		{
			other._writeIndex = 0;
			other._loopParams = LoopParams();
			other._mixer = std::make_unique<audio::AudioMixer>(audio::AudioMixerParams());
		}

		Loop& operator=(Loop&& other)
		{
			if (this != &other)
			{
				ReleaseResources();
				std::swap(_lastPeak, other._lastPeak);
				std::swap(_pitch, other._pitch);
				std::swap(_loopLength, other._loopLength);
				std::swap(_state, other._state);
				std::swap(_guiParams, other._guiParams);
				std::swap(_writeIndex, other._writeIndex);
				std::swap(_playIndex, other._playIndex);
				std::swap(_loopParams, other._loopParams);
				_mixer.swap(other._mixer);
				_model.swap(other._model);
				_vu.swap(other._vu);
				std::swap(_bufferBank, other._bufferBank);
			}

			return *this;
		}

	public:
		static std::optional<std::shared_ptr<Loop>> FromFile(LoopParams loopParams,
			io::JamFile::Loop loopStruct,
			std::wstring dir);
		static audio::AudioMixerParams GetMixerParams(utils::Size2d loopSize,
			audio::BehaviourParams behaviour,
			unsigned int channel);

		virtual std::string ClassName() const { return "Loop"; }
		virtual void SetSize(utils::Size2d size) override;
		virtual MultiAudioDirection MultiAudibleDirection() const override { return MULTIAUDIO_BOTH; }
		virtual void Draw3d(base::DrawContext& ctx, unsigned int numInstances) override;
		virtual void OnPlay(const std::shared_ptr<base::MultiAudioSink> dest, unsigned int numSamps) override;
		virtual void EndMultiPlay(unsigned int numSamps) override;
		inline virtual int OnWrite(float samp, int indexOffset) override;
		inline virtual int OnOverwrite(float samp, int indexOffset) override;
		virtual void EndWrite(unsigned int numSamps, bool updateIndex) override;

		void OnPlayRaw(const std::shared_ptr<base::MultiAudioSink> dest,
			unsigned int channel,
			unsigned int delaySamps,
			unsigned int numSamps);
		unsigned int LoopChannel() const;
		void SetLoopChannel(unsigned int channel);
		unsigned int InputChannel() const;
		void SetInputChannel(unsigned int channel);
		std::string Id() const;

		void Update();
		bool Load(const io::WavReadWriter& readWriter);
		void Record();
		void Play(unsigned long index,
			unsigned long loopLength,
			bool continueRecording);
		void EndRecording();
		void Ditch();
		void Overdub();
		void PunchIn();
		void PunchOut();

	protected:
		void Reset();
		unsigned long LoopIndex() const;
		static double CalcDrawRadius(unsigned long loopLength);
		void UpdateLoopModel();

	protected:
		unsigned long _playIndex;
		float _lastPeak;
		double _pitch;
		unsigned long _loopLength;
		LoopVisualState _state;
		LoopParams _loopParams;
		std::shared_ptr<audio::AudioMixer> _mixer;
		std::shared_ptr<LoopModel> _model;
		std::shared_ptr<VU> _vu;
		audio::BufferBank _bufferBank;
	};
}
