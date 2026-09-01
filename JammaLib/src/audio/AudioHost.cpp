#include "stdafx.h"
#include "AudioHost.h"
#include "../ninjam/NinjamLoopAlignment.h"
#include "../utils/Timer.h"
#include <algorithm>
#include <cmath>
#include <iostream>

using namespace engine;
using namespace utils;

namespace audio
{
	AudioHost::AudioHost(io::UserConfig userConfig) :
		_userConfig(userConfig),
		_tickUserConfig(_userConfig),
		_channelMixer(std::make_shared<audio::ChannelMixer>(audio::ChannelMixerParams{}))
	{
	}

	AudioHost::~AudioHost()
	{
		Close();
	}

	bool AudioHost::Init(std::shared_ptr<ninjam::NinjamController> ninjamController, TickCallback tickCallback)
	{
		std::scoped_lock lock(_audioMutex);

		_ninjamController = ninjamController;
		_tickCallback = tickCallback;

		auto dev = audio::AudioDevice::Open(AudioHost::AudioCallback,
			[](RtAudioError::Type type, const std::string& err) { std::cout << "[" << type << " RtAudio Error] " << err << std::endl; },
			_userConfig.Audio,
			this);

		if (dev.has_value())
		{
			_audioDevice = std::move(dev.value());
			_audioSampleCounter.store(0u, std::memory_order_release);

			auto audioStreamParams = _audioDevice->GetAudioStreamParams();
			_tickStreamParams = audioStreamParams;
			_ninjamMetronome.Configure(audioStreamParams.SampleRate);

			auto inLatency = (0u == audioStreamParams.InputLatency) ?
				_userConfig.Audio.LatencyIn :
				audioStreamParams.InputLatency;

			_channelMixer->SetParams(audio::ChannelMixerParams({
					_userConfig.AdcBufferDelay(inLatency) + audioStreamParams.BufSize,
					audio::ChannelMixer::DefaultBufferSize,
					audioStreamParams.NumInputChannels,
					audioStreamParams.NumOutputChannels }));

			auto stationsSnapshot = _audioStations.load(std::memory_order_acquire);
			if (stationsSnapshot)
			{
				for (auto& station : *stationsSnapshot)
				{
					if (station)
					{
						station->SetupBuffers(audioStreamParams.BufSize);
						station->SetSampleRate(static_cast<float>(audioStreamParams.SampleRate));
						station->SetNumAdcChannels(audioStreamParams.NumInputChannels);
						station->SetNumDacChannels(audioStreamParams.NumOutputChannels);
					}
				}
			}

			if (_ninjamController)
			{
				auto outLatency = (0u == audioStreamParams.OutputLatency) ?
					_userConfig.Audio.LatencyOut :
					audioStreamParams.OutputLatency;

				_ninjamController->SetAudioFormat(
					audioStreamParams.SampleRate,
					audioStreamParams.BufSize,
					audioStreamParams.NumInputChannels,
					audioStreamParams.NumOutputChannels,
					inLatency,
					outLatency);
			}

			_audioDevice->Start();
			_audioDevice->GetAudioStreamParams().PrintParams();
			return true;
		}
		return false;
	}

	void AudioHost::Close()
	{
		if (_audioDevice)
			_audioDevice->Stop();

		std::scoped_lock lock(_audioMutex);

		if (_ninjamController)
			_ninjamController->Stop();

		if (_audioDevice)
			_audioDevice->Stop();
	}

	void AudioHost::SetStations(std::shared_ptr<const std::vector<std::shared_ptr<Station>>> stations)
	{
		_audioStations.store(stations, std::memory_order_release);
	}

std::optional<NinjamTimingCommandReceipt> AudioHost::LastAppliedTimingCommand() const noexcept
{
	for (auto attempt = 0u; attempt < 2u; ++attempt)
	{
		const auto before = _lastAppliedTimingReceiptSequence.load(std::memory_order_acquire);
		if (before == 0u || (before & 1u) != 0u)
			return std::nullopt;

		const NinjamTimingCommandReceipt receipt{
			_lastAppliedTimingCommandSequence.load(std::memory_order_relaxed),
			_lastAppliedTimingCommandGeneration.load(std::memory_order_relaxed),
			_lastAppliedTimingCommandType.load(std::memory_order_relaxed),
			_lastAppliedTimingCommandPolicy.load(std::memory_order_relaxed),
			_lastAppliedTimingSceneCoordinate.load(std::memory_order_relaxed),
			_lastAppliedTimingDelta.load(std::memory_order_relaxed)
		};
		if (before == _lastAppliedTimingReceiptSequence.load(std::memory_order_acquire))
			return receipt;
	}

	return std::nullopt;
}

	int AudioHost::AudioCallback(void* outBuffer,
		void* inBuffer,
		unsigned int numSamps,
		double streamTime,
		RtAudioStreamStatus status,
		void* userData)
	{
		AudioHost* engine = (AudioHost*)userData;
		engine->_OnAudio((float*)inBuffer, (float*)outBuffer, numSamps, streamTime);
		return 0;
	}

	void AudioHost::_OnAudio(float* inBuf,
		float* outBuf,
		unsigned int numSamps,
		double streamTime)
	{
		const auto audioStreamParams = nullptr == _audioDevice ?
			audio::AudioStreamParams() : _audioDevice->GetAudioStreamParams();
		const auto blockStartSample = _audioSampleCounter.load(std::memory_order_relaxed);
		const auto stationsSnapshot = _audioStations.load(std::memory_order_acquire);
		static const std::vector<std::shared_ptr<Station>> emptyStations;
		const auto& stations = stationsSnapshot ? *stationsSnapshot : emptyStations;
		bool beginSyncPhaseMapAfterOffset = false;
		bool rebaseSyncPhaseMapAfterOffset = false;

		// Unified audio-boundary transport fan-out. Consume at most one coherent
		// command before any station playback advancement so the Timer and every
		// active local take apply the same signed correction in the same block.
		if (const auto command = _ninjamTimingCommandMailbox.Consume())
		{
			const auto hadSyncPhaseMap = _syncPhaseMap.IsActive();
			const auto timingClock = _timingClock.load(std::memory_order_acquire);
			long long stationDelta = command->PhaseDeltaSamps;
			auto policy = command->LocalFollowPolicy;
			std::uint64_t sceneCoordinate = command->SceneCoordinateSamps;
			unsigned long previousMasterLength = 0ul;
			unsigned int previousRemotePhase = 0u;
			const auto disablesSync = command->Type == ninjam::NinjamTimingCommandType::Invalidate
				|| (command->Type == ninjam::NinjamTimingCommandType::ReplaceTiming
					&& policy == ninjam::NinjamLocalFollowPolicy::NoSync);
			if (disablesSync || command->InvalidateSceneAnchors)
			{
				for (auto& station : stations)
					if (station && !station->IsRemote()) station->InvalidateSceneAnchors();
				_activeNinjamFollowPolicy = ninjam::NinjamLocalFollowPolicy::NoSync;
				_syncPhaseMap = {};
				if (timingClock)
					timingClock->ResetMusicalTransport();
				policy = ninjam::NinjamLocalFollowPolicy::NoSync;
			}
			if (!disablesSync && timingClock)
			{
				previousMasterLength = timingClock->SeedSourceLength();
				previousRemotePhase = timingClock->SampOffset();
				if (command->Type == ninjam::NinjamTimingCommandType::ReplaceTiming)
				{
					sceneCoordinate = timingClock->SceneSamplePos();
					_activeNinjamFollowPolicy = policy;
					// A replacement can arrive while an earlier remote map is active.
					// Restore its source-ruler position before calculating the next map.
					if (_syncPhaseMap.IsActive())
						for (auto& station : stations)
							if (station && !station->IsRemote()) station->RestoreSyncPhaseMap(sceneCoordinate);
				}
				else
				{
					policy = _activeNinjamFollowPolicy;
					sceneCoordinate = timingClock->SceneSamplePos();
					// EndMultiPlay advances local cursors at device rate. Before a
					// remote discipline command rebases the source-rate phase map,
					// restore that map at this exact boundary. Otherwise each take
					// captures a different one-block device-rate residue as its new
					// origin, which changes intentional audio/MIDI relative offsets.
					if (policy != ninjam::NinjamLocalFollowPolicy::NoSync
						&& _syncPhaseMap.IsActive())
						for (auto& station : stations)
							if (station && !station->IsRemote())
								station->RestoreSyncPhaseMap(sceneCoordinate);
				}
				utils::Timer::Command timerCommand;
				unsigned int musicalRemotePhase = timingClock->SampOffset();
				timerCommand.Generation = command->Generation;
				timerCommand.SeedLengthSamps = command->SeedLengthSamps;
				timerCommand.QuantiseSamps = command->QuantiseSamps;
				timerCommand.Quantisation = command->Quantisation;
				switch (command->Type)
				{
				case ninjam::NinjamTimingCommandType::ReplaceTiming:
				{
					const auto replacement = ninjam::ResolveBoundaryTimingReplacement(
						timingClock->SeedSourceLength(), timingClock->SampOffset(),
						static_cast<unsigned int>(command->SeedLengthSamps), command->AbsolutePhaseSamps,
						command->PhaseObservationSample, blockStartSample);
					timerCommand.Type = utils::Timer::CommandType::ReplaceTiming;
					timerCommand.PhaseDeltaSamps = static_cast<long long>(replacement.RemotePhaseSamps);
					musicalRemotePhase = replacement.RemotePhaseSamps;
					if (_syncPhaseMap.SourceLengthSamps == 0ul)
						_syncPhaseMap.SourceLengthSamps = previousMasterLength;
					const auto sourceLength = _syncPhaseMap.SourceLengthSamps;
					if (sourceLength > 0ul)
					{
						const auto sourceBefore = _syncPhaseMap.IsActive()
							? _syncPhaseMap.SourcePhaseAt(sceneCoordinate)
							: static_cast<unsigned long>(previousRemotePhase) % sourceLength;
						const auto sourceAfter = ninjam::SourcePhaseAtRemotePhase(replacement.RemotePhaseSamps,
							sourceLength, command->SeedLengthSamps);
						stationDelta = ninjam::SignedCircularDifference(static_cast<unsigned int>(sourceBefore),
							static_cast<unsigned int>(sourceAfter), static_cast<unsigned int>(sourceLength));
					}
					else
						stationDelta = 0;
					break;
				}
				case ninjam::NinjamTimingCommandType::Invalidate:
					timerCommand.Type = utils::Timer::CommandType::Invalidate;
					break;
				default:
					timerCommand.Type = utils::Timer::CommandType::PhaseCorrection;
					timerCommand.PhaseDeltaSamps = command->PhaseDeltaSamps;
					break;
				}
				if (command->Type == ninjam::NinjamTimingCommandType::ReplaceTiming)
					timingClock->ReanchorMusicalTransport(sceneCoordinate,
						musicalRemotePhase, command->SeedLengthSamps,
						command->BeatsPerInterval, command->TempoBpm,
						audioStreamParams.SampleRate);
				else if (command->Type == ninjam::NinjamTimingCommandType::JoinAlignment
					|| command->Type == ninjam::NinjamTimingCommandType::PhaseDiscipline)
					timingClock->ReanchorMusicalTransportCurrentGeometry(sceneCoordinate,
						static_cast<unsigned int>(ninjam::PositiveModulo(
							static_cast<std::int64_t>(timingClock->SampOffset()) + command->PhaseDeltaSamps,
							timingClock->SeedSourceLength())), audioStreamParams.SampleRate);
				timingClock->ApplyCommand(timerCommand);
				if (command->Type != ninjam::NinjamTimingCommandType::ReplaceTiming
					&& _syncPhaseMap.IsActive())
				{
					const auto sourceBefore = _syncPhaseMap.SourcePhaseAt(sceneCoordinate);
					const auto sourceAfter = ninjam::SourcePhaseAtRemotePhase(timingClock->SampOffset(),
						_syncPhaseMap.SourceLengthSamps, _syncPhaseMap.RemoteLengthSamps);
					stationDelta = ninjam::SignedCircularDifference(static_cast<unsigned int>(sourceBefore),
						static_cast<unsigned int>(sourceAfter), static_cast<unsigned int>(_syncPhaseMap.SourceLengthSamps));
				}
			}

			engine::LoopTake::TimingCorrectionReason reason =
				engine::LoopTake::TimingCorrectionReason::PhaseDiscipline;
			switch (command->Type)
			{
			case ninjam::NinjamTimingCommandType::ReplaceTiming:
				reason = engine::LoopTake::TimingCorrectionReason::TempoReplacement;
				break;
			case ninjam::NinjamTimingCommandType::JoinAlignment:
				reason = engine::LoopTake::TimingCorrectionReason::JoinAlignment;
				break;
			case ninjam::NinjamTimingCommandType::Invalidate:
				reason = engine::LoopTake::TimingCorrectionReason::Invalidation;
				break;
			default:
				reason = engine::LoopTake::TimingCorrectionReason::PhaseDiscipline;
				break;
			}
			if (!disablesSync)
			{
				for (auto& station : stations)
					if (station && !station->IsRemote()) station->ApplyTimingCommand(stationDelta,
						command->Generation, reason, policy, sceneCoordinate);
				if (timingClock && (command->Type == ninjam::NinjamTimingCommandType::ReplaceTiming
					|| command->Type == ninjam::NinjamTimingCommandType::JoinAlignment
					|| command->Type == ninjam::NinjamTimingCommandType::PhaseDiscipline))
				{
					if (command->Type == ninjam::NinjamTimingCommandType::ReplaceTiming)
					{
						_syncPhaseMap.RemoteLengthSamps = command->SeedLengthSamps;
					}
					const auto sourcePhase = ninjam::SourcePhaseAtRemotePhase(timingClock->SampOffset(),
						_syncPhaseMap.SourceLengthSamps, _syncPhaseMap.RemoteLengthSamps);
					const auto sourceCoordinate = hadSyncPhaseMap
						? _syncPhaseMap.SourceCoordinateAt(sceneCoordinate) + stationDelta
						: static_cast<std::int64_t>(sourcePhase);
					_syncPhaseMap.Rebase(sceneCoordinate, sourceCoordinate);
					beginSyncPhaseMapAfterOffset = !hadSyncPhaseMap;
					rebaseSyncPhaseMapAfterOffset = hadSyncPhaseMap;
				}
			}
			const auto writingReceipt = _lastAppliedTimingReceiptSequence.fetch_add(
				1u, std::memory_order_acq_rel) + 1u;
			_lastAppliedTimingCommandSequence.store(command->Sequence, std::memory_order_relaxed);
			_lastAppliedTimingCommandGeneration.store(command->Generation, std::memory_order_relaxed);
			_lastAppliedTimingCommandType.store(command->Type, std::memory_order_relaxed);
			_lastAppliedTimingCommandPolicy.store(policy, std::memory_order_relaxed);
			_lastAppliedTimingSceneCoordinate.store(sceneCoordinate, std::memory_order_relaxed);
			_lastAppliedTimingDelta.store(stationDelta, std::memory_order_relaxed);
			_lastAppliedTimingReceiptSequence.store(writingReceipt + 1u, std::memory_order_release);
		}

		if (const auto localTransportOffsetLoopFrac = _localTransportOffsetLoopFracMailbox.ConsumeLatest())
			_localTransportOffsetLoopFrac = localTransportOffsetLoopFrac.value();

		const auto timingClock = _timingClock.load(std::memory_order_acquire);
		if (timingClock)
			timingClock->AdvanceMusicalTransport();
		const auto masterLength = timingClock ? timingClock->SeedSourceLength() : 0ul;
		if (masterLength != _localTransportOffsetMasterLength)
			_localTransportOffsetMasterLength = masterLength;
		const auto localTransportOffsetTargetSamps = masterLength == 0ul ? 0 :
			static_cast<long long>(std::llround(_localTransportOffsetLoopFrac * static_cast<double>(masterLength)));
		if (localTransportOffsetTargetSamps != _localTransportOffsetTargetSamps)
		{
			_localTransportOffsetTargetSamps = localTransportOffsetTargetSamps;
			for (auto& station : stations)
			{
				if (station && !station->IsRemote())
					station->SetLocalTransportOffsetSamps(localTransportOffsetTargetSamps);
			}
		}
		// Offset is loop-local, so capture the new map only after every local take
		// has applied it. This is a single post-command fan-out, rather than a
		// speculative capture followed by an offset-dependent rebase.
		if (beginSyncPhaseMapAfterOffset)
			for (auto& station : stations)
				if (station && !station->IsRemote()) station->BeginSyncPhaseMap(_syncPhaseMap.SceneOriginSamps,
					_syncPhaseMap.SourceLengthSamps, _syncPhaseMap.RemoteLengthSamps,
					_syncPhaseMap.SourceCoordinateAtOrigin);
		else if (rebaseSyncPhaseMapAfterOffset)
			for (auto& station : stations)
				if (station && !station->IsRemote()) station->RebaseSyncPhaseMap(_syncPhaseMap.SceneOriginSamps,
					_syncPhaseMap.SourceLengthSamps, _syncPhaseMap.RemoteLengthSamps,
					_syncPhaseMap.SourceCoordinateAtOrigin);

		if (nullptr != inBuf)
		{
			for (auto channel = 0u; channel < audioStreamParams.NumInputChannels; ++channel)
			{
				float peak = 0.0f;
				for (auto sample = channel; sample < numSamps * audioStreamParams.NumInputChannels;
					sample += audioStreamParams.NumInputChannels)
					peak = std::max(peak, std::abs(inBuf[sample]));
				if (channel < _AdcPeakChannels)
					_adcPeaks[channel].store(peak, std::memory_order_relaxed);
			}

			auto inLatency = (0u == audioStreamParams.InputLatency) ?
				_userConfig.Audio.LatencyIn :
				audioStreamParams.InputLatency;

			_channelMixer->FromAdc(inBuf, audioStreamParams.NumInputChannels, numSamps);

			_channelMixer->InitPlay(0u, numSamps);
			_channelMixer->Source()->SetSourceType(Audible::AUDIOSOURCE_MONITOR);

			for (auto& station : stations)
			{
				if (station->IsRemote())
					continue;

				_channelMixer->WriteToSink(station, numSamps);
			}

			_channelMixer->InitPlay(_userConfig.AdcBufferDelay(inLatency), numSamps);
			_channelMixer->Source()->SetSourceType(Audible::AUDIOSOURCE_ADC);

			for (auto& station : stations)
			{
				if (station->IsRemote())
					continue;

				_channelMixer->WriteToSink(station, numSamps);

				station->SetSourceType(Audible::AUDIOSOURCE_MONITOR);
				station->OnBounce(numSamps, _userConfig, audioStreamParams);

				station->SetSourceType(Audible::AUDIOSOURCE_BOUNCE);
				station->OnBounce(numSamps, _userConfig, audioStreamParams);

				station->EndMultiWrite(numSamps, true, Audible::AUDIOSOURCE_BOUNCE);
			}
		}

		_channelMixer->Source()->EndMultiPlay(numSamps);

		_channelMixer->Sink()->Zero(numSamps, Audible::AUDIOSOURCE_LOOPS);

		auto ingestRemoteStation = [&](const std::shared_ptr<Station>& stationBase) {
			if (!stationBase || !stationBase->IsRemote())
				return;

			auto station = std::static_pointer_cast<StationRemote>(stationBase);
			if (!station || !station->IsConnectedRemote())
				return;
			if (!_ninjamController)
				return;

			// The decoded samples belong to the connection scratch buffers, so the
			// scoped connection use must remain alive through synchronous ingestion.
			auto connectionUse = _ninjamController->AcquireConnectionUse();
			const float* left = nullptr;
			const float* right = nullptr;
			unsigned int frameCount = 0u;
			if (connectionUse && connectionUse->ConsumeStereoPair(station->AssignedOutputChannel(), left, right, frameCount))
			{
				auto ingestFrames = frameCount < numSamps ? frameCount : numSamps;
				station->IngestStereoBlock(left, right, ingestFrames);
			}
		};

		ninjam::NinjamTiming liveTiming;
		if (_ninjamController)
		{
			const auto remoteTiming = _ninjamController->GetLiveTiming();
			// Anchor the observation to the Timer's block-start position (the Timer
			// is ticked at end-of-block, so it still reflects block start here). The
			// coordinator projects phase back to this anchor so job-scheduling delay
			// does not masquerade as phase error (§2.7).
			const auto anchorClock = _timingClock.load(std::memory_order_acquire);
			const auto localAnchor = anchorClock
				? static_cast<std::uint64_t>(anchorClock->AbsoluteSamplePos(
					static_cast<unsigned long>(blockStartSample)))
				: blockStartSample;
			liveTiming = ninjam::ToDeviceTiming(remoteTiming, remoteTiming.IsConnected,
				audioStreamParams.SampleRate, 0u, 0ul, ++_ninjamTimingObservationSequence, localAnchor,
				blockStartSample);
			_ninjamTimingMailbox.Publish(liveTiming);
		}

		if (_activeNinjamFollowPolicy != ninjam::NinjamLocalFollowPolicy::NoSync
			&& timingClock && _syncPhaseMap.IsActive())
			for (auto& station : stations)
				if (station && !station->IsRemote()) station->RestoreSyncPhaseMap(timingClock->SceneSamplePos());

		if (nullptr != outBuf)
		{
			std::fill(outBuf, outBuf + numSamps * audioStreamParams.NumOutputChannels, 0.0f);

			for (auto& station : stations)
			{
				station->Zero(numSamps, Audible::AUDIOSOURCE_LOOPS);
				ingestRemoteStation(station);
				station->WriteBlock(_channelMixer->Sink(), nullptr, 0, numSamps,
					static_cast<std::uint32_t>(blockStartSample));
				station->EndMultiPlay(numSamps);
			}

			_channelMixer->ToDac(outBuf, audioStreamParams.NumOutputChannels, numSamps);

			if (_ninjamController)
			{
				const auto metronomeEnabled = _ninjamMetronomeEnabled.load(std::memory_order_acquire);

				_ninjamController->ProcessExportBlock(outBuf,
					audioStreamParams.NumOutputChannels,
					inBuf,
					audioStreamParams.NumInputChannels,
					numSamps,
					audioStreamParams.SampleRate);

				if (metronomeEnabled && liveTiming.IsValid)
				{
					ninjam::NinjamMetronomeTimingInput timingInput;
					timingInput.intervalPositionSamps = liveTiming.IntervalPositionSamps;
					timingInput.intervalLengthSamps = liveTiming.IntervalLengthSamps;
					timingInput.bpm = liveTiming.Bpm;
					timingInput.bpi = liveTiming.Bpi;
					timingInput.deviceSampleRate = liveTiming.DeviceSampleRate;
					timingInput.outputLatencySamps = audioStreamParams.OutputLatency == 0u ?
						_userConfig.Audio.LatencyOut : audioStreamParams.OutputLatency;
					timingInput.numFrames = numSamps;

					const auto metronomeTiming = ninjam::NinjamMetronomeTiming::Compute(
						timingInput, _ninjamMetronomeTimingState);
					if (metronomeTiming.generationReset)
						_ninjamMetronome.Reset();
					_ninjamMetronome.Mix(outBuf, audioStreamParams.NumOutputChannels, numSamps, metronomeTiming);
				}
				else
				{
					_ninjamMetronomeTimingState = {};
					_ninjamMetronome.Reset();
				}
			}
			else
			{
				_ninjamMetronomeTimingState = {};
				_ninjamMetronome.Reset();
			}
		}
		else
		{
			for (auto& station : stations)
			{
				ingestRemoteStation(station);
				station->WriteBlock(_channelMixer->Sink(), nullptr, 0, numSamps,
					static_cast<std::uint32_t>(blockStartSample));
				station->EndMultiPlay(numSamps);
			}

			if (_ninjamController)
			{
				_ninjamController->ProcessExportBlock(nullptr,
					audioStreamParams.NumOutputChannels,
					inBuf,
					audioStreamParams.NumInputChannels,
					numSamps,
					audioStreamParams.SampleRate);
			}
		}

		_channelMixer->Sink()->EndMultiWrite(numSamps, true, Audible::AUDIOSOURCE_LOOPS);

		if (_tickCallback)
		{
			_tickCallback(Timer::GetTime(), numSamps, _tickUserConfig, _tickStreamParams);
		}

		_audioSampleCounter.store(blockStartSample + numSamps, std::memory_order_release);
		midi::PublishMidiClockAnchor(_midiClockAnchor,
			blockStartSample + numSamps,
			std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count());
	}
}
