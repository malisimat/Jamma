#pragma once
#include "../audio/InputHistory.h"

// Audio recording/playback entity within a LoopTake; owns its buffers, logical
// length, read/write positions, fades, and loop-local phase.

#include <atomic>
#include <string>
#include <memory>
#include <mutex>
#include <limits>
#include "ActionReceiver.h"
#include "../base/BounceWriter.h"
#include "Tweakable.h"
#include "ResourceUser.h"
#include "GlUtils.h"
#include "../graphics/LoopModel.h"
#include "../graphics/AudioSpectralModel.h"
#include "../base/Jammable.h"
#include "../base/PublishedAudioSnapshot.h"
#include "../gui/GuiModel.h"
#include "../io/FileReadWriter.h"
#include "../io/JamFile.h"
#include "../audio/BufferBank.h"
#include "../spectral/AudioSpectralSource.h"
#include "../spectral/AudioSpectralEditState.h"
#include "../spectral/AudioSpectralPlaybackSnapshot.h"
#include "../audio/AudioMixer.h"
#include "../audio/Hanning.h"
#include "../graphics/VU.h"
#include "../graphics/GlDrawContext.h"
#include "../resources/WavResource.h"
#include "../vst/VstChain.h"
#include "../audio/PlaybackDelay.h"

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

		// Move (exclusive ownership only; both audio readers must be stopped).
		Loop(Loop&& other) :
			Jammable(other._loopParams, other._loggingConfig),
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
			_vstChain(),
			_backVstChain(std::move(other._backVstChain)),
			_flipVstChain(other._flipVstChain.load(std::memory_order_relaxed)),
			_pendingVstLoads(std::move(other._pendingVstLoads)),
			_pendingVstUnloads(std::move(other._pendingVstUnloads)),
			_sampleRate(other._sampleRate.load(std::memory_order_relaxed)),
			_blockSize(other._blockSize),
			_vstPluginPaths(std::move(other._vstPluginPaths))
		{
			other._InvalidateAudioSource();
			_vstChain.store(other._vstChain.exchange(nullptr, std::memory_order_acq_rel), std::memory_order_release);
			_writeIndex.store(other._writeIndex.load(std::memory_order_relaxed), std::memory_order_relaxed);
			other._writeIndex.store(0ul, std::memory_order_relaxed);
			other._visualUpdatesEnabled = true;
			other._isPunchInActive.store(false, std::memory_order_relaxed);
			other._loopParams = LoopParams();
			other._mixer = std::make_unique<audio::AudioMixer>(audio::AudioMixerParams());
			other._playbackDelay = std::make_shared<audio::PlaybackDelay>();
			other._flipVstChain.store(false, std::memory_order_relaxed);
		}

		Loop& operator=(Loop&& other)
		{
			if (this != &other)
			{
				_InvalidateAudioSource();
				other._InvalidateAudioSource();
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
				std::swap(_loggingConfig, other._loggingConfig);
				_mixer.swap(other._mixer);
				_hanning.swap(other._hanning);
				_model.swap(other._model);
				_vu.swap(other._vu);
				std::swap(_bufferBank, other._bufferBank);
				std::swap(_monitorBufferBank, other._monitorBufferBank);
				auto chain = _vstChain.exchange(other._vstChain.load(std::memory_order_acquire), std::memory_order_acq_rel);
				other._vstChain.store(chain, std::memory_order_release);
				_backVstChain.swap(other._backVstChain);
				_playbackDelay.swap(other._playbackDelay);
				bool flip = _flipVstChain.load(std::memory_order_relaxed);
				_flipVstChain.store(other._flipVstChain.load(std::memory_order_relaxed), std::memory_order_relaxed);
				other._flipVstChain.store(flip, std::memory_order_relaxed);
				std::swap(_pendingVstLoads, other._pendingVstLoads);
				std::swap(_pendingVstUnloads, other._pendingVstUnloads);
				const auto sampleRate = _sampleRate.load(std::memory_order_relaxed);
				_sampleRate.store(other._sampleRate.exchange(sampleRate, std::memory_order_relaxed),
					std::memory_order_relaxed);
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
			unsigned int numSamps, const audio::DacRenderContext* dac = nullptr,
            std::uint32_t trigger = 0u, std::int64_t delay = 0, unsigned int channel = 0u);

		// Reads source data via ReadBlock, then routes to destination
		// via mixer->WriteBlock → behaviour->ApplyBlock → dest->OnBlockWriteChannel.
		void WriteBlock(const std::shared_ptr<base::MultiAudioSink> dest,
			const std::shared_ptr<base::BounceWriter> bounceWriter,
			int sampOffset,
			unsigned int numSamps, const audio::DacRenderContext* dac = nullptr,
            std::uint32_t trigger = 0u, std::int64_t delay = 0, unsigned int channel = 0u);
		virtual void EndMultiPlay(unsigned int numSamps) override;
		virtual void OnBlockWrite(const base::AudioWriteRequest& request, int writeOffset) override;
		virtual void EndWrite(unsigned int numSamps,
			bool updateIndex) override;
		virtual bool Mute() override;
		virtual bool UnMute() override;
		bool IsMuted() const override;
		void SetGroupMutedAudio(bool muted) noexcept;
		void SetCaptureSourceMuteControl(
			const std::shared_ptr<const audio::CaptureSourceMuteControl>& control) noexcept;
		virtual void Reset() override;
		virtual void Update();
		void UpdateCapacity();
		void RefreshVisualModel();
		std::shared_ptr<LoopModel> Model() const noexcept { return _model; }
		// Presentation-only, called with the scene/layout read ownership on the window thread.
		graphics::CameraBounds CameraBoundsInParent(float parentVisualScale = 1.0f) const;
		float UnfittedVisualHalfHeight(float parentVisualScale) const;
		void SetAudioSpectralMorph(float morph) noexcept { _audioSpectralMorph = std::clamp(morph, 0.0f, 1.0f); }
		// UI/render owner only; never called from audio. Lazy editor-only geometry.
		void SetAudioSpectralModel(std::shared_ptr<graphics::AudioSpectralModel> model);
		std::shared_ptr<graphics::AudioSpectralModel> AudioSpectralModel() const noexcept { return _audioSpectralModel; }
		bool CanRefreshVisualModel() const noexcept;

		unsigned int LoopChannel() const;
		void SetLoopChannel(unsigned int channel);
		std::string Id() const;
		LoopPlayState PlayState() const { return _playState.load(std::memory_order_relaxed); }
		unsigned long LoopLength() const noexcept { return _loopLength.load(std::memory_order_relaxed); }
		// Advisory editor eligibility only. The bounded copy runs on the callback
		// itself; state checks alone cannot protect an off-audio PCM read.
		virtual bool SupportsAudioSpectralEditing() const noexcept { return true; }
		bool CanEditAudioSource() const noexcept;
		// UI-owner operations. No BufferBank PCM is read by the requesting thread.
		std::shared_ptr<spectral::AudioSpectralSourceCapture> RequestAudioSpectralSource(std::string& reason,
            const std::shared_ptr<spectral::AudioSpectralResourceBudget>& budget = nullptr,
            const spectral::AudioSpectralSourceLayout* retainedLayout = nullptr);
		void CancelAudioSpectralSourceCapture();
		bool IsCurrentAudioSpectralSource(const spectral::AudioSpectralSource& source) const noexcept;
		std::uint64_t AudioSourceGeneration() const noexcept
		{
			return _audioSourceGeneration.load(std::memory_order_acquire);
		}
		bool PublishAudioSpectralEdit(
			std::shared_ptr<const spectral::AudioSpectralEditState> state, std::string& reason);
		std::shared_ptr<const spectral::AudioSpectralEditState> AudioSpectralEdit() const;
		// Off-audio publication. Reject stale source/component revisions; callback
		// leases keep active/transition content alive without owning destruction.
		bool PublishAudioSpectralPlayback(
			std::shared_ptr<const spectral::AudioSpectralPlaybackSnapshot> snapshot, std::string& reason);
		// UI-owned Undo/cancellation barrier; existing audible content may remain
		// until a newer result is ready, but older queued results cannot be adopted.
		void InvalidateAudioSpectralResultsBefore(std::uint64_t spectralRevision,
            std::uint64_t expectedGeneration);
		std::shared_ptr<const spectral::AudioSpectralPlaybackSnapshot> AudioSpectralPlayback() const;
		std::uint64_t AppliedAudioSpectralRevision() const noexcept
		{
			return _appliedAudioSpectralRevision.load(std::memory_order_acquire);
		}
		// Immutable edited output export; no mutable bank reads and gain applied once.
		static std::vector<float> ExportAudioSpectralSamples(
			const spectral::AudioSpectralPlaybackSnapshot& snapshot, bool retainPhysicalTail = false);
		// Recorded storage length is an upper bound for any logical loop length.
		// It intentionally remains distinct after first-master tail normalisation.
		unsigned long PhysicalLoopLength() const noexcept
		{
			const auto length = _bufferBank.Length();
			return length > constants::MaxLoopFadeSamps ? length - constants::MaxLoopFadeSamps : 0ul;
		}
		static double CalcDrawRadius(unsigned long loopLength);
		// Legacy raw-bank export requires a stopped callback; edited output uses
		// ExportAudioSpectralSamples with an immutable captured revision.
		std::vector<float> ExportSamples(bool retainPhysicalTail = false) const;
		std::vector<float> ExportOriginalSamplesQuiescent(bool retainPhysicalTail = false) const;
		io::JamFile::Loop ToJamFile(const std::string& wavFilename) const;
		void SetMixerLevel(double level);
		// UI wiring keeps a weak reference; this Loop owns the post-gain source.
		std::shared_ptr<audio::AudioMixer> Mixer() const noexcept { return _mixer; }
		void SetMasterVisualScale(float scale) noexcept;
		void SetVisualUpdatesEnabled(bool enabled);
		bool Load(const io::WavReadWriter& readWriter);
		void Record();
		void Play(unsigned long index,
			unsigned long loopLength,
			bool continueRecording);
		// Directly re-position the play cursor to index without altering the state
		// machine.  Call only from a coherent reposition path (e.g. anchor re-derive
		// at remote interval wrap); the loop must already be in a playing state.
		void SetPlayIndex(unsigned long index) noexcept
		{
			const auto len = _loopLength.load(std::memory_order_relaxed);
			if (len == 0ul) return;
			_playIndex.store(index < len ? index : index % len, std::memory_order_relaxed);
		}
		void SetSceneAnchor(unsigned long anchor) noexcept
		{
			_sceneAnchor.store(anchor, std::memory_order_relaxed);
			_hasSceneAnchor.store(true, std::memory_order_release);
		}
		bool HasSceneAnchor() const noexcept { return _hasSceneAnchor.load(std::memory_order_acquire); }
		unsigned long SceneAnchor() const noexcept { return _sceneAnchor.load(std::memory_order_relaxed); }
		void InvalidateSceneAnchor() noexcept { _hasSceneAnchor.store(false, std::memory_order_release); }
		void ShiftPlayIndex(long long deltaSamps) noexcept;
		unsigned long PlayIndex() const noexcept
		{
			return _playIndex.load(std::memory_order_relaxed);
		}
		unsigned long BodyPlayIndex() const noexcept
		{
			const auto length = _loopLength.load(std::memory_order_relaxed);
			if (length == 0ul)
				return 0ul;
			const auto index = _playIndex.load(std::memory_order_relaxed);
			return (index + length - constants::MaxLoopFadeSamps) % length;
		}
		void SetBodyPlayIndex(unsigned long index) noexcept
		{
			const auto length = _loopLength.load(std::memory_order_relaxed);
			if (length == 0ul)
				return;
			_playIndex.store(constants::MaxLoopFadeSamps + (index % length), std::memory_order_relaxed);
		}
		void EndRecording();
		// Capture cursor uses signed physical storage coordinates. Configure before
		// publication; cursor/coverage are callback-owned, completion is job-owned.
		void ConfigureTriggerCapture(std::int64_t begin, bool requireAdc, bool requireBounce, bool requirePunchAdc = true) noexcept;
		void SetTriggerCaptureCursor(std::int64_t cursor) noexcept;
		bool SetTriggerCaptureLength(unsigned long length) noexcept;
		bool TriggerCaptureComplete() const noexcept;
        bool CanUseTemporaryPlayback(unsigned long bodyPhase, unsigned int count) const noexcept;
		bool TriggerCaptureCoverageValid() const noexcept { return _triggerCaptureCoverageValid.load(std::memory_order_acquire); }
		bool TriggerCaptureReady(unsigned long bodyPhase, unsigned int count) const noexcept;
		void NormalizeTriggerCapture();

		void Ditch();
		void Overdub();
		void PunchIn();
		void PunchOut();
		bool IsPunchInActive() const noexcept { return _isPunchInActive.load(std::memory_order_relaxed); }
		double LoopIndexFrac() const noexcept;

		// VST chain management — staging only; actual load/unload happens on the
		// job thread after CommitChanges() queues the appropriate job.
		void LoadVstPlugin(std::wstring path,
			std::vector<std::uint8_t> initialState = {});
		// Startup-only: synchronously construct and publish one plugin before this
		// loop can enter an audio snapshot.
		bool LoadVstPluginSynchronously(const std::wstring& path,
			const std::vector<std::uint8_t>& initialState = {},
			bool bypass = false, const std::string& stateId = {});
		void UnloadVstPlugin(size_t index);
		void ForceUnloadAllVstPlugins();
		void SetSampleRate(float sampleRate);
		void SetBlockSize(unsigned int blockSize) { _blockSize = blockSize; }
		float GetSampleRate() const noexcept { return _sampleRate.load(std::memory_order_relaxed); }
		unsigned int GetBlockSize() const noexcept { return _blockSize; }

		// Non-RT accessor to retrieve a loaded plugin instance (or nullptr).
		std::shared_ptr<vst::IVstPlugin> GetVstPlugin(size_t index) const;
		bool ReconfigureVstFormat(float sampleRate, unsigned int blockSize);

		// Job-owner compensation setup; polling/configuration may allocate or
		// dispatch to a controller. Clear is callback-only and constant time.
		void PollPlaybackPluginChanges() const noexcept;
		bool CanConfigureRecordedPlaybackDelay(unsigned int delaySamps) const;
		bool ConfigureRecordedPlaybackDelay(unsigned int delaySamps);
		void ClearRecordedPlaybackDelay() noexcept;
		// Real-time-safe latency sum of the published loaded, non-bypassed chain.
		int CurrentVstLatencySamps() const noexcept
		{
			auto chain = _vstChain.borrow();
			return chain ? chain->GetLatencySamples() : 0;
		}

		virtual actions::ActionResult OnAction(actions::JobAction action) override;

	protected:
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
		// Generation is scalar-only on callback/control threads. PCM copy progress is
		// callback-owned; publication/retirement is UI-owned, pumped by CommitChanges().
		static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
		static_assert(std::atomic<float>::is_always_lock_free);
		std::atomic<std::uint64_t> _audioSourceGeneration{ 1u };
		const std::shared_ptr<const spectral::AudioSpectralSourceIdentity> _audioSourceIdentity =
			std::make_shared<const spectral::AudioSpectralSourceIdentity>();
		// Serializes publication/collection on non-audio UI/configuration threads.
		std::mutex _audioSourcePublicationMutex;
		base::PublishedAudioSnapshot<spectral::AudioSpectralSourceCapture> _audioSourceCapture;

		// Edit owner/export/UI only; atomic shared ownership is never used by audio.
		std::atomic<std::shared_ptr<const spectral::AudioSpectralEditState>> _audioSpectralEdit;
		base::PublishedAudioSnapshot<const spectral::AudioSpectralPlaybackSnapshot> _audioSpectralPlayback;
		// Audio reader only. Declared after publisher so leases release before it.
		base::PublishedAudioSnapshot<const spectral::AudioSpectralPlaybackSnapshot>::Lease _activeAudioSpectral;
		base::PublishedAudioSnapshot<const spectral::AudioSpectralPlaybackSnapshot>::Lease _previousAudioSpectral;
		unsigned int _audioSpectralTransitionSamps = 0u;
		unsigned int _audioSpectralTransitionProgress = 0u;
		std::atomic<std::uint64_t> _appliedAudioSpectralRevision{ 0u };
		std::atomic<std::uint64_t> _minimumAudioSpectralRevision{ 0u };
        std::atomic<std::uint64_t> _minimumAudioSpectralGeneration{ 0u };

		bool _triggerCapture = false;
		bool _triggerCaptureRequiresAdc = true;
		bool _triggerCaptureRequiresBounce = false;
		bool _triggerPunchRequiresAdc = true;
		std::int64_t _triggerCaptureBegin = 0;
		std::int64_t _triggerCaptureCursor = 0; // callback only
		std::int64_t _triggerAdcBlockThrough = 0; // callback only
		std::atomic<bool> _triggerCaptureCoverageValid{ true };
		std::atomic<std::int64_t> _triggerCaptureEnd{ (std::numeric_limits<std::int64_t>::max)() };
		std::atomic<std::int64_t> _triggerAdcThrough{ 0 };
		std::atomic<std::int64_t> _triggerBounceThrough{ 0 };
		std::atomic<bool> _triggerCaptureNormalized{ false };
		bool _visualUpdatesEnabled;
		std::atomic<bool> _isPunchInActive;
		// Current playback position, in samples. Advanced by ReadBlock().
		// Source coordinates exclude plugin latency. Recorded rendering applies
		// compensation after the loop chain, preserving capture/fade coordinates.
		std::atomic<unsigned long> _playIndex;
		std::atomic<unsigned long> _sceneAnchor{ 0ul };
		std::atomic_bool _hasSceneAnchor{ false };
		float _lastPeak;
		double _pitch;
		std::atomic<unsigned long> _loopLength;
		std::atomic<LoopPlayState> _playState;
		LoopParams _loopParams;
		std::shared_ptr<audio::AudioMixer> _mixer;
		std::shared_ptr<audio::Hanning> _hanning;
		std::shared_ptr<LoopModel> _model;
		std::shared_ptr<graphics::AudioSpectralModel> _audioSpectralModel;
		float _audioSpectralMorph = 0.0f;
		std::shared_ptr<graphics::VU> _vu;
		float _masterVisualScale = 1.0f;
		audio::BufferBank _bufferBank;
		audio::BufferBank _monitorBufferBank;
		// Live VST chain published atomically for lock-free audio-thread reads.
		base::PublishedAudioSnapshot<vst::VstChain> _vstChain;
		std::shared_ptr<audio::PlaybackDelay> _playbackDelay = std::make_shared<audio::PlaybackDelay>();
		std::shared_ptr<vst::VstChain> _backVstChain;
		std::atomic<bool> _flipVstChain{ false };
		std::vector<std::pair<std::wstring, std::vector<std::uint8_t>>> _pendingVstLoads;
		std::vector<size_t> _pendingVstUnloads;
		std::atomic<float> _sampleRate{ static_cast<float>(constants::DefaultSampleRate) };
		unsigned int _blockSize{ constants::DefaultBufferSizeSamps };
		// Non-RT metadata: written on job thread (OnAction), read on main thread (VstEntries).
		// Access is guarded by _vstPathsMutex in both directions.
		mutable std::mutex _vstPathsMutex;
		std::vector<std::wstring> _vstPluginPaths;

	private:
		std::size_t _CollectAudioSourceCaptures(); // Off callback only.
		void _InvalidateAudioSource() noexcept;
		void _CopyAudioSpectralSourceAtAudioBoundary() noexcept;
		void _AdvanceAudioSpectralPlayback(unsigned int numSamps) noexcept;
		float _ReadAudioSpectralSample(unsigned long storageIndex, unsigned int blockOffset, std::uint64_t generation) const noexcept;
		spectral::AudioSpectralSourceLayout _AudioSpectralSourceLayout() const noexcept;
		static void _DrainVstChain(std::shared_ptr<vst::VstChain> chain);
	};
}
