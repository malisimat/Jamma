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

	void AudioHost::PublishDesiredTiming(const ninjam::NinjamDesiredTransportState& desired)
	{
		std::scoped_lock lock(_ninjamDesiredTimingPublishMutex);
		const auto published = _ninjamDesiredTimingMailbox.ReadLatest();
		if (published.has_value()
			&& (published->SessionEpoch > desired.SessionEpoch
				|| (published->SessionEpoch == desired.SessionEpoch
					&& published->Version >= desired.Version)))
		{
			return;
		}
		_ninjamDesiredTimingMailbox.Publish(desired);
	}

std::optional<NinjamDesiredTimingReceipt> AudioHost::LastAppliedDesiredTiming() const noexcept
{
	for (auto attempt = 0u; attempt < 2u; ++attempt)
	{
		const auto before = _lastAppliedTimingReceiptSequence.load(std::memory_order_acquire);
		if (before == 0u || (before & 1u) != 0u)
			return std::nullopt;

		const NinjamDesiredTimingReceipt receipt{
			_lastAppliedDesiredVersion.load(std::memory_order_relaxed),
			_lastAppliedSessionEpoch.load(std::memory_order_relaxed),
			_lastAppliedDesiredGeneration.load(std::memory_order_relaxed),
			_lastAppliedDesiredIntent.load(std::memory_order_relaxed),
			_lastAppliedDesiredPolicy.load(std::memory_order_relaxed),
			_lastAppliedTimingSceneCoordinate.load(std::memory_order_relaxed),
			_lastAppliedTimingDelta.load(std::memory_order_relaxed)
		};
		if (before == _lastAppliedTimingReceiptSequence.load(std::memory_order_acquire))
			return receipt;
	}

	return std::nullopt;
}

bool AudioHost::ApplyDesiredTimingAtAudioBoundary(std::uint64_t blockStartSample,
	unsigned int sampleRate) noexcept
{
	const auto desiredValue = _ninjamDesiredTimingMailbox.ReadLatest();
	if (!desiredValue.has_value() || desiredValue->Version == 0u)
		return false;
	const auto& desired = desiredValue.value();
	const auto epochChanged = desired.SessionEpoch != _appliedNinjamTiming.SessionEpoch;
	if (!epochChanged && desired.Version <= _appliedNinjamTiming.Version)
		return false;
	if (_appliedNinjamTiming.SessionEpoch != 0u
		&& desired.SessionEpoch < _appliedNinjamTiming.SessionEpoch)
		return false;
	if (!epochChanged && desired.LocalFollowPolicy != ninjam::NinjamLocalFollowPolicy::NoSync
		&& desired.Generation <= _appliedNinjamTiming.Generation)
		return false;

	const auto stationsSnapshot = _audioStations.load(std::memory_order_acquire);
	const auto* stations = stationsSnapshot.get();
	const auto timingClock = _timingClock.load(std::memory_order_acquire);
	const auto noSync = desired.LocalFollowPolicy == ninjam::NinjamLocalFollowPolicy::NoSync
		|| !desired.HasRemoteTiming;
	const auto resetAuthority = epochChanged || noSync;
	if (resetAuthority)
	{
		if (stations)
			for (const auto& station : *stations)
				if (station && !station->IsRemote()) station->ResetTimingEpoch();
		_activeNinjamFollowPolicy = ninjam::NinjamLocalFollowPolicy::NoSync;
		_syncPhaseMap = {};
		if (timingClock)
		{
			utils::Timer::Command reset;
			reset.Type = utils::Timer::CommandType::Invalidate;
			timingClock->ApplyCommand(reset);
			timingClock->ResetMusicalTransport();
		}
	}

	long long stationDelta = 0;
	std::uint64_t sceneCoordinate = timingClock ? timingClock->SceneSamplePos() : 0u;
	if (!noSync && timingClock && desired.IntervalLengthSamps > 0ul
		&& desired.HasObservationSample)
	{
		const auto geometryChanged = !_appliedNinjamTiming.HasRemoteTiming
			|| desired.IntervalLengthSamps != _appliedNinjamTiming.IntervalLengthSamps
			|| desired.GrainSamps != _appliedNinjamTiming.GrainSamps
			|| desired.BeatsPerInterval != _appliedNinjamTiming.BeatsPerInterval
			|| desired.TempoBpm != _appliedNinjamTiming.TempoBpm
			|| desired.Quantisation != _appliedNinjamTiming.Quantisation;
		const auto hadSyncPhaseMap = _syncPhaseMap.IsActive();
		const auto previousMasterLength = timingClock->SeedSourceLength();
		const auto previousRemotePhase = timingClock->SampOffset();
		if (_syncPhaseMap.IsActive() && stations)
			for (const auto& station : *stations)
				if (station && !station->IsRemote()) station->RestoreSyncPhaseMap(sceneCoordinate);

		const auto replacement = ninjam::ResolveBoundaryTimingReplacement(
			timingClock->SeedSourceLength(), timingClock->SampOffset(),
			static_cast<unsigned int>(desired.IntervalLengthSamps), desired.RemotePhaseSamps,
			std::optional<std::uint64_t>{ desired.ObservationSample }, blockStartSample);
		utils::Timer::Command timerCommand;
		timerCommand.Generation = desired.Generation;
		timerCommand.SeedLengthSamps = desired.IntervalLengthSamps;
		timerCommand.QuantiseSamps = desired.GrainSamps;
		timerCommand.Quantisation = desired.Quantisation;
		if (geometryChanged)
		{
			timerCommand.Type = utils::Timer::CommandType::ReplaceTiming;
			timerCommand.PhaseDeltaSamps = static_cast<long long>(replacement.RemotePhaseSamps);
			if (_syncPhaseMap.SourceLengthSamps == 0ul)
			{
				_syncPhaseMap.SourceLengthSamps = previousMasterLength > 0ul
					? previousMasterLength : desired.IntervalLengthSamps;
			}
			const auto sourceLength = _syncPhaseMap.SourceLengthSamps;
			if (sourceLength > 0ul)
			{
				const auto sourceBefore = _syncPhaseMap.IsActive()
					? _syncPhaseMap.SourcePhaseAt(sceneCoordinate)
					: static_cast<unsigned long>(previousRemotePhase) % sourceLength;
				const auto sourceAfter = ninjam::SourcePhaseAtRemotePhase(replacement.RemotePhaseSamps,
					sourceLength, desired.IntervalLengthSamps);
				stationDelta = ninjam::SignedCircularDifference(static_cast<unsigned int>(sourceBefore),
					static_cast<unsigned int>(sourceAfter), static_cast<unsigned int>(sourceLength));
			}
		}
		else
		{
			timerCommand.Type = utils::Timer::CommandType::PhaseCorrection;
			timerCommand.PhaseDeltaSamps = ninjam::SignedCircularDifference(
				timingClock->SampOffset(), replacement.RemotePhaseSamps,
				static_cast<unsigned int>(desired.IntervalLengthSamps));
			stationDelta = timerCommand.PhaseDeltaSamps;
		}

		_activeNinjamFollowPolicy = desired.LocalFollowPolicy;
		if (geometryChanged)
			timingClock->ReanchorMusicalTransport(sceneCoordinate, replacement.RemotePhaseSamps,
				desired.IntervalLengthSamps, desired.BeatsPerInterval, desired.TempoBpm, sampleRate);
		else
			timingClock->ReanchorMusicalTransportCurrentGeometry(sceneCoordinate,
				replacement.RemotePhaseSamps, sampleRate);
		timingClock->ApplyCommand(timerCommand);
		if (!geometryChanged && _syncPhaseMap.IsActive())
		{
			const auto sourceBefore = _syncPhaseMap.SourcePhaseAt(sceneCoordinate);
			const auto sourceAfter = ninjam::SourcePhaseAtRemotePhase(timingClock->SampOffset(),
				_syncPhaseMap.SourceLengthSamps, _syncPhaseMap.RemoteLengthSamps);
			stationDelta = ninjam::SignedCircularDifference(static_cast<unsigned int>(sourceBefore),
				static_cast<unsigned int>(sourceAfter), static_cast<unsigned int>(_syncPhaseMap.SourceLengthSamps));
		}

		const auto reason = geometryChanged
			? engine::LoopTake::TimingCorrectionReason::TempoReplacement
			: desired.Intent == ninjam::NinjamDesiredTimingIntent::JoinAlignment
				? engine::LoopTake::TimingCorrectionReason::JoinAlignment
				: engine::LoopTake::TimingCorrectionReason::PhaseDiscipline;
		if (stations)
			for (const auto& station : *stations)
				if (station && !station->IsRemote()) station->ApplyTimingCommand(stationDelta,
					desired.Generation, reason, desired.LocalFollowPolicy, sceneCoordinate);

		if (geometryChanged)
			_syncPhaseMap.RemoteLengthSamps = desired.IntervalLengthSamps;
		const auto sourcePhase = ninjam::SourcePhaseAtRemotePhase(timingClock->SampOffset(),
			_syncPhaseMap.SourceLengthSamps, _syncPhaseMap.RemoteLengthSamps);
		const auto sourceCoordinate = hadSyncPhaseMap
			? _syncPhaseMap.SourceCoordinateAt(sceneCoordinate) + stationDelta
			: static_cast<std::int64_t>(sourcePhase);
		_syncPhaseMap.Rebase(sceneCoordinate, sourceCoordinate);
		_beginSyncPhaseMapAfterOffset = !hadSyncPhaseMap;
		_rebaseSyncPhaseMapAfterOffset = hadSyncPhaseMap;
	}

	_appliedNinjamTiming = desired;
	const auto writingReceipt = _lastAppliedTimingReceiptSequence.fetch_add(
		1u, std::memory_order_acq_rel) + 1u;
	_lastAppliedDesiredVersion.store(desired.Version, std::memory_order_relaxed);
	_lastAppliedSessionEpoch.store(desired.SessionEpoch, std::memory_order_relaxed);
	_lastAppliedDesiredGeneration.store(desired.Generation, std::memory_order_relaxed);
	_lastAppliedDesiredIntent.store(desired.Intent, std::memory_order_relaxed);
	_lastAppliedDesiredPolicy.store(desired.LocalFollowPolicy, std::memory_order_relaxed);
	_lastAppliedTimingSceneCoordinate.store(sceneCoordinate, std::memory_order_relaxed);
	_lastAppliedTimingDelta.store(stationDelta, std::memory_order_relaxed);
	_lastAppliedTimingReceiptSequence.store(writingReceipt + 1u, std::memory_order_release);
	return true;
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
		_beginSyncPhaseMapAfterOffset = false;
		_rebaseSyncPhaseMapAfterOffset = false;
		ApplyDesiredTimingAtAudioBoundary(blockStartSample, audioStreamParams.SampleRate);

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
		if (_beginSyncPhaseMapAfterOffset)
			for (auto& station : stations)
				if (station && !station->IsRemote()) station->BeginSyncPhaseMap(_syncPhaseMap.SceneOriginSamps,
					_syncPhaseMap.SourceLengthSamps, _syncPhaseMap.RemoteLengthSamps,
					_syncPhaseMap.SourceCoordinateAtOrigin);
		else if (_rebaseSyncPhaseMapAfterOffset)
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

		const auto localTransport = timingClock
			? std::optional<utils::Timer::TransportObservation>{ timingClock->ObserveTransport() }
			: std::nullopt;
		ninjam::NinjamTiming liveTiming;
		const auto publishNinjamTiming = [&](const ninjam::NinjamRemoteTiming& remoteTiming)
			{
				liveTiming = ninjam::ProjectTimingToAudioSample(ninjam::ToDeviceTiming(remoteTiming,
					remoteTiming.IsConnected, audioStreamParams.SampleRate, 0u, 0ul,
					++_ninjamTimingObservationSequence), blockStartSample);
				if (localTransport.has_value())
				{
					liveTiming.HasLocalTransport = true;
					liveTiming.LocalTransport.MasterLengthSamps = localTransport->MasterLengthSamps;
					liveTiming.LocalTransport.MasterPhaseSamps = localTransport->MasterPhaseSamps;
					liveTiming.LocalTransport.LoopCount = localTransport->LoopCount;
					liveTiming.LocalTransport.AbsoluteSamplePos = localTransport->AbsoluteSamplePos;
					liveTiming.LocalTransport.SceneSamplePos = localTransport->SceneSamplePos;
					liveTiming.LocalBlockStartSample = localTransport->AbsoluteSamplePos;
				}
				_ninjamTimingMailbox.Publish(liveTiming);
			};

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

				const auto remoteTiming = _ninjamController->ProcessExportBlock(outBuf,
					audioStreamParams.NumOutputChannels,
					inBuf,
					audioStreamParams.NumInputChannels,
					numSamps,
					audioStreamParams.SampleRate,
					blockStartSample);
				publishNinjamTiming(remoteTiming);

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
				const auto remoteTiming = _ninjamController->ProcessExportBlock(nullptr,
					audioStreamParams.NumOutputChannels,
					inBuf,
					audioStreamParams.NumInputChannels,
					numSamps,
					audioStreamParams.SampleRate,
					blockStartSample);
				publishNinjamTiming(remoteTiming);
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
