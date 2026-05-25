#pragma once

#include <atomic>
#include <string>
#include <memory>
#include <mutex>
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
#include "../vst/VstChain.h"

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
		~Loop();

		// Copy
		Loop(const Loop&) = delete;
		Loop& operator=(const Loop&) = delete;

		// Move
		Loop(Loop&& other) :
			Jammable(other._loopParams),
			_visualUpdatesEnabled(other._visualUpdatesEnabled),
			_isPunchInActive(other._isPunchInActive.load(std::memory_order_relaxed)),
			_lastPeak(other._lastPeak),
			_pitch(other._pitch),
			_loopLength(other._loopLength.load(std::memory_order_relaxed)),
			_playState(other._playState.load(std::memory_order_relaxed)),
			_playIndex(other._playIndex.load(std::memory_order_relaxed)),
			_loopParams{other._loopParams},
			_mixer(std::move(other._mixer)),
			_hanning(std::move(other._hanning)),
			_model(std::move(other._model)),
			_vu(std::move(other._vu)),
			_bufferBank(std::move(other._bufferBank)),
			_monitorBufferBank(std::move(other._monitorBufferBank)),
			_vstChain(other._vstChain.exchange(nullptr, std::memory_order_acq_rel)),
			_backVstChain(std::move(other._backVstChain)),
			_flipVstChain(other._flipVstChain.load(std::memory_order_relaxed)),
			_pendingVstLoads(std::move(other._pendingVstLoads)),
			_pendingVstUnloads(std::move(other._pendingVstUnloads)),
			_sampleRate(other._sampleRate),
			_blockSize(other._blockSize),
			_vstPluginPaths(std::move(other._vstPluginPaths))
		{
			_writeIndex.store(other._writeIndex.load(std::memory_order_relaxed), std::memory_order_relaxed);
			other._writeIndex.store(0ul, std::memory_order_relaxed);
			other._visualUpdatesEnabled = true;
			other._isPunchInActive.store(false, std::memory_order_relaxed);
			other._loopParams = LoopParams();
			other._mixer = std::make_unique<audio::AudioMixer>(audio::AudioMixerParams());
			other._flipVstChain.store(false, std::memory_order_relaxed);
		}

		Loop& operator=(Loop&& other)
		{
			if (this != &other)
			{
				ReleaseResources();
				std::swap(_visualUpdatesEnabled, other._visualUpdatesEnabled);
				bool punchIn = _isPunchInActive.load(std::memory_order_relaxed);
				_isPunchInActive.store(other._isPunchInActive.exchange(punchIn, std::memory_order_relaxed), std::memory_order_relaxed);
				std::swap(_lastPeak, other._lastPeak);
				std::swap(_pitch, other._pitch);
				std::swap(_state, other._state);
				std::swap(_guiParams, other._guiParams);
				auto loopLength = _loopLength.load(std::memory_order_relaxed);
				_loopLength.store(other._loopLength.exchange(loopLength, std::memory_order_relaxed), std::memory_order_relaxed);
				auto writeIndex = _writeIndex.load(std::memory_order_relaxed);
				_writeIndex.store(other._writeIndex.exchange(writeIndex, std::memory_order_relaxed), std::memory_order_relaxed);
				auto playIndex = _playIndex.load(std::memory_order_relaxed);
				_playIndex.store(other._playIndex.exchange(playIndex, std::memory_order_relaxed), std::memory_order_relaxed);
				auto playState = _playState.load(std::memory_order_relaxed);
				_playState.store(other._playState.exchange(playState, std::memory_order_relaxed), std::memory_order_relaxed);
				std::swap(_loopParams, other._loopParams);
				_mixer.swap(other._mixer);
				_hanning.swap(other._hanning);
				_model.swap(other._model);
				_vu.swap(other._vu);
				std::swap(_bufferBank, other._bufferBank);
				std::swap(_monitorBufferBank, other._monitorBufferBank);
				auto chain = _vstChain.exchange(other._vstChain.load(std::memory_order_acquire), std::memory_order_acq_rel);
				other._vstChain.store(chain, std::memory_order_release);
				_backVstChain.swap(other._backVstChain);
				bool flip = _flipVstChain.load(std::memory_order_relaxed);
				_flipVstChain.store(other._flipVstChain.load(std::memory_order_relaxed), std::memory_order_relaxed);
				other._flipVstChain.store(flip, std::memory_order_relaxed);
				std::swap(_pendingVstLoads, other._pendingVstLoads);
				std::swap(_pendingVstUnloads, other._pendingVstUnloads);
				std::swap(_sampleRate, other._sampleRate);
				std::swap(_blockSize, other._blockSize);
				std::swap(_vstPluginPaths, other._vstPluginPaths);
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
		// Read source data from BufferBank into outBuf, handling crossfade.
		// Returns the number of samples written to outBuf (0 if not playing).
		// Pure data-out method — no destination parameter.
		unsigned int ReadBlock(float* outBuf,
			int sampOffset,
			unsigned int numSamps);

		// Reads source data via ReadBlock, then routes to destination
		// via mixer->WriteBlock → behaviour->ApplyBlock → dest->OnBlockWriteChannel.
		void WriteBlock(const std::shared_ptr<base::MultiAudioSink> dest,
			const std::shared_ptr<Trigger> trigger,
			int sampOffset,
			unsigned int numSamps);
		virtual void EndMultiPlay(unsigned int numSamps) override;
		virtual void OnBlockWrite(const base::AudioWriteRequest& request, int writeOffset) override;
		virtual void EndWrite(unsigned int numSamps,
			bool updateIndex) override;
		virtual bool Mute() override;
		virtual bool UnMute() override;
		virtual void Reset() override;
		virtual void Update();

		unsigned int LoopChannel() const;
		void SetLoopChannel(unsigned int channel);
		std::string Id() const;
		LoopPlayState PlayState() const { return _playState.load(std::memory_order_relaxed); }
		unsigned long LoopLength() const noexcept { return _loopLength.load(std::memory_order_relaxed); }
		std::vector<float> ExportSamples() const;
		io::JamFile::Loop ToJamFile(const std::string& wavFilename) const;
		void SetMixerLevel(double level);
		void SetVisualUpdatesEnabled(bool enabled);
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
		bool IsPunchInActive() const noexcept { return _isPunchInActive.load(std::memory_order_relaxed); }
		double LoopIndexFrac() const noexcept;

		// VST chain management — staging only; actual load/unload happens on the
		// job thread after CommitChanges() queues the appropriate job.
		void LoadVstPlugin(std::wstring path);
		void UnloadVstPlugin(size_t index);
		void SetSampleRate(float sampleRate) { _sampleRate = sampleRate; }
		void SetBlockSize(unsigned int blockSize) { _blockSize = blockSize; }
		float GetSampleRate() const noexcept { return _sampleRate; }
		unsigned int GetBlockSize() const noexcept { return _blockSize; }

		// Non-RT accessor to retrieve a loaded plugin instance (or nullptr).
		std::shared_ptr<vst::VstPlugin> GetVstPlugin(size_t index) const;

		virtual actions::ActionResult OnAction(actions::JobAction action) override;

	protected:
		static double _CalcDrawRadius(unsigned long loopLength);
		static LoopModel::LoopModelState _GetLoopModelState(base::DrawPass pass, LoopPlayState state, bool isMuted);
		virtual unsigned long _ModelDisplayLength(bool isRecording, unsigned long actualLoopLength) const;
		virtual double _DrawRadiusScale() const noexcept { return 1.0; }
		virtual void _ApplyLoopVisualModel(const audio::BufferBank& buffer,
			unsigned long actualLength,
			unsigned long displayLength,
			unsigned long offset,
			float radius);
		virtual std::vector<actions::JobAction> _CommitChanges() override;

		unsigned long _LoopIndex() const;
		void _UpdateLoopModel();
		void _ForceUpdateLoopModel();

	protected:
		bool _visualUpdatesEnabled;
		std::atomic<bool> _isPunchInActive;
		std::atomic<unsigned long> _playIndex;
		float _lastPeak;
		double _pitch;
		std::atomic<unsigned long> _loopLength;
		std::atomic<LoopPlayState> _playState;
		LoopParams _loopParams;
		std::shared_ptr<audio::AudioMixer> _mixer;
		std::shared_ptr<audio::Hanning> _hanning;
		std::shared_ptr<LoopModel> _model;
		std::shared_ptr<VU> _vu;
		audio::BufferBank _bufferBank;
		audio::BufferBank _monitorBufferBank;
		// Live VST chain published atomically for lock-free audio-thread reads.
		std::atomic<std::shared_ptr<vst::VstChain>> _vstChain;
		std::shared_ptr<vst::VstChain> _backVstChain;
		std::atomic<bool> _flipVstChain{ false };
		std::vector<std::wstring> _pendingVstLoads;
		std::vector<size_t> _pendingVstUnloads;
		float _sampleRate{ static_cast<float>(constants::DefaultSampleRate) };
		unsigned int _blockSize{ constants::DefaultBufferSizeSamps };
		// Non-RT metadata: written on job thread (OnAction), read on main thread (VstEntries).
		// Access is guarded by _vstPathsMutex in both directions.
		mutable std::mutex _vstPathsMutex;
		std::vector<std::wstring> _vstPluginPaths;
	};
}
