#pragma once

#include <string>
#include <memory>
#include "Trigger.h"
#include "ActionReceiver.h"
#include "Tweakable.h"
#include "ResourceUser.h"
#include "GlUtils.h"
#include "VU.h"
#include "LoopModel.h"
#include "../base/Jammable.h"
#include "../gui/GuiModel.h"
#include "../io/FileReadWriter.h"
#include "../io/JamFile.h"
#include "../audio/BufferBank.h"
#include "../audio/AudioMixer.h"
#include "../audio/Hanning.h"
#include "../graphics/GlDrawContext.h"
#include "../resources/WavResource.h"

namespace engine
{
	class LoopParams :
		public base::JammableParams
	{
	public:
		LoopParams() :
			base::JammableParams(
				GuiElementParams(0, DrawableParams{ "" },
				MoveableParams(utils::Position2d{ 0, 0 }, utils::Position3d{ 0, 0, 0 }, 1.0),
				SizeableParams{ 1,1 },
				"",
				"",
				"",
				{})
			),
			Id(""),
			TakeId(""),
			Wav(""),
			PlayTexture(""),
			RecordTexture(""),
			OverdubTexture(""),
			PunchTexture(""),
			Channel(0),
			FadeSamps(constants::DefaultFadeSamps)
		{
		}

		LoopParams(base::JammableParams params,
			std::string wav) :
			base::JammableParams(params),
			Id(""),
			TakeId(""),
			Wav(wav),
			PlayTexture(""),
			RecordTexture(""),
			OverdubTexture(""),
			PunchTexture(""),
			Channel(0u),
			FadeSamps(constants::DefaultFadeSamps)
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
		public base::Jammable,
		public base::AudioSink
	{
	public:
		enum LoopPlayState
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
		Loop(LoopParams params,
			audio::AudioMixerParams mixerParams);
		~Loop() { ReleaseResources(); }

		// Copy
		Loop(const Loop&) = delete;
		Loop& operator=(const Loop&) = delete;

		// Move
		Loop(Loop&& other) :
			Jammable(other._loopParams),
			_lastPeak(other._lastPeak),
			_pitch(other._pitch),
			_loopLength(other._loopLength),
			_playState(other._playState),
			_playIndex(other._playIndex),
			_loopParams{other._loopParams},
			_mixer(std::move(other._mixer)),
			_hanning(std::move(other._hanning)),
			_model(std::move(other._model)),
			_vu(std::move(other._vu)),
			_bufferBank(std::move(other._bufferBank)),
			_monitorBufferBank(std::move(other._monitorBufferBank))
		{
			other._writeIndex = 0ul;
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
				_hanning.swap(other._hanning);
				_model.swap(other._model);
				_vu.swap(other._vu);
				std::swap(_bufferBank, other._bufferBank);
				std::swap(_monitorBufferBank, other._monitorBufferBank);
			}

			return *this;
		}

	public:
		static std::optional<std::shared_ptr<Loop>> FromFile(LoopParams loopParams,
			io::JamFile::Loop loopStruct,
			std::wstring dir);
		static audio::AudioMixerParams GetMixerParams(utils::Size2d loopSize,
			audio::BehaviourParams behaviour);

		virtual std::string ClassName() const override { return "Loop"; }
		virtual MultiAudioPlugType MultiAudioPlug() const override { return MULTIAUDIOPLUG_BOTH; }
		virtual void SetSize(utils::Size2d size) override;
		virtual void Draw3d(base::DrawContext& ctx, unsigned int numInstances, base::DrawPass pass) override;
		virtual void OnPlay(const std::shared_ptr<base::MultiAudioSink> dest,
			const std::shared_ptr<Trigger> trigger,
			int sampOffset,
			unsigned int numSamps) override;
		virtual void EndMultiPlay(unsigned int numSamps) override;
		inline virtual int OnMixWrite(float samp,
			float fadeCurrent,
			float fadeNew,
			int indexOffset,
			Audible::AudioSourceType source) override;
		virtual void EndWrite(unsigned int numSamps,
			bool updateIndex) override;
		virtual bool Select() override;
		virtual bool DeSelect() override;
		virtual bool Mute() override;
		virtual bool UnMute() override;
		virtual void Reset() override;

		unsigned int LoopChannel() const;
		void SetLoopChannel(unsigned int channel);
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
		static double _CalcDrawRadius(unsigned long loopLength);
		static LoopModel::LoopModelState _GetLoopModelState(base::DrawPass pass, LoopPlayState state, bool isMuted);
		
		unsigned long _LoopIndex() const;
		void _UpdateLoopModel();

	protected:
		unsigned long _playIndex;
		float _lastPeak;
		double _pitch;
		unsigned long _loopLength;
		LoopPlayState _playState;
		LoopParams _loopParams;
		std::shared_ptr<audio::AudioMixer> _mixer;
		std::shared_ptr<audio::Hanning> _hanning;
		std::shared_ptr<LoopModel> _model;
		std::shared_ptr<VU> _vu;
		audio::BufferBank _bufferBank;
		audio::BufferBank _monitorBufferBank;
	};
}
