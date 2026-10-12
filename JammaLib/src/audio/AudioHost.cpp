// Sole block-boundary timing applicator; job-side policy arrives as complete values,
// while Station/LoopTake/Loop retain entity-specific geometry and phase.
#include "stdafx.h"
#include "AudioHost.h"
#include "../engine/RigSnapshot.h"
#include "../ninjam/NinjamLoopAlignment.h"
#include "../utils/Timer.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <thread>

using namespace engine;
using namespace utils;

namespace audio
{
	void AudioHost::LocalTransportOffsetLoopFracMailbox::Publish(
		double normalizedLoopFrac) noexcept
	{
		const auto writingSequence = _sequence.fetch_add(1u, std::memory_order_acq_rel) + 1u;
		_normalizedLoopFrac.store(normalizedLoopFrac, std::memory_order_relaxed);
		_sequence.store(writingSequence + 1u, std::memory_order_release);
		_hasPublication.store(true, std::memory_order_release);
	}

	std::optional<double> AudioHost::LocalTransportOffsetLoopFracMailbox::ConsumeLatest() noexcept
	{
		if (!_hasPublication.load(std::memory_order_acquire))
			return std::nullopt;

		for (unsigned int attempt = 0u; attempt < _MaxReadAttempts; ++attempt)
		{
			const auto before = _sequence.load(std::memory_order_acquire);
			if ((before & 1u) != 0u)
				continue;
			if (before == _consumedSequence)
				return std::nullopt;

			const auto normalizedLoopFrac = _normalizedLoopFrac.load(std::memory_order_relaxed);
			const auto after = _sequence.load(std::memory_order_acquire);
			if (before == after)
			{
				_consumedSequence = before;
				return normalizedLoopFrac;
			}
		}

		return std::nullopt;
	}

	void AudioHost::PublishLocalTransportOffsetLoopFrac(double normalizedLoopFrac) noexcept
	{
		_localTransportOffsetLoopFracMailbox.Publish(normalizedLoopFrac);
	}

	std::uint64_t AudioHost::LocalMasterTimingMailbox::Publish(
		const LocalMasterTimingRequest& request) noexcept
	{
		const auto writing = _sequence.fetch_add(1u, std::memory_order_seq_cst) + 1u;
		_seedSamps.store(request.SeedSamps, std::memory_order_seq_cst);
		_masterLengthSamps.store(request.MasterLengthSamps, std::memory_order_seq_cst);
		_type.store(request.Type, std::memory_order_seq_cst);
		_targetTake.store(request.TargetTake, std::memory_order_seq_cst);
		_targetLoop.store(request.TargetLoop, std::memory_order_seq_cst);
		_sequence.store(writing + 1u, std::memory_order_seq_cst);
		return writing + 1u;
	}

	std::optional<AudioHost::LocalMasterTimingRequest>
	AudioHost::LocalMasterTimingMailbox::ConsumeLatest() noexcept
	{
		for (unsigned int attempt = 0u; attempt < 4u; ++attempt)
		{
			const auto before = _sequence.load(std::memory_order_seq_cst);
			if (before == 0u || before == _consumedSequence) return std::nullopt;
			if ((before & 1u) != 0u) continue;
			LocalMasterTimingRequest request;
			request.Version = before;
			request.SeedSamps = _seedSamps.load(std::memory_order_seq_cst);
			request.MasterLengthSamps = _masterLengthSamps.load(std::memory_order_seq_cst);
			request.Type = _type.load(std::memory_order_seq_cst);
			request.TargetTake = _targetTake.load(std::memory_order_seq_cst);
			request.TargetLoop = _targetLoop.load(std::memory_order_seq_cst);
			if (before == _sequence.load(std::memory_order_seq_cst))
			{
				return request;
			}
		}
		return std::nullopt;
	}

	std::uint64_t AudioHost::PublishLocalMasterTiming(unsigned int seedSamps,
		std::uint32_t masterLengthSamps, utils::Timer::QuantisationType type,
		const engine::LoopTake* targetTake, const engine::Loop* targetLoop) noexcept
	{
		return _localMasterTimingMailbox.Publish({ 0u, seedSamps, masterLengthSamps,
			type, targetTake, targetLoop });
	}

	std::optional<AudioHost::LocalMasterTimingReceipt>
	AudioHost::LastPreparedLocalMasterTiming() const noexcept
	{
		for (unsigned int attempt = 0u; attempt < 4u; ++attempt)
		{
			const auto before = _localMasterReceiptSequence.load(std::memory_order_seq_cst);
			if ((before & 1u) != 0u) continue;
			LocalMasterTimingReceipt receipt{
				_localMasterReceiptVersion.load(std::memory_order_seq_cst),
				_localMasterReceiptOldAbsolute.load(std::memory_order_seq_cst),
				_localMasterReceiptNewPhase.load(std::memory_order_seq_cst),
				_localMasterReceiptNewLength.load(std::memory_order_seq_cst) };
			if (before == _localMasterReceiptSequence.load(std::memory_order_seq_cst))
				return receipt.Version == 0u ? std::nullopt : std::optional{ receipt };
		}
		return std::nullopt;
	}

	void AudioHost::ApplyLocalMasterTimingAtAudioBoundary(
		const std::vector<std::shared_ptr<engine::Station>>& stations,
		utils::Timer* timingClock) noexcept
	{
		if (!timingClock) return;
		if (const auto request = _localMasterTimingMailbox.ConsumeLatest(); request)
		{
			if (request->Version == _localMasterCancelledVersion.load(std::memory_order_seq_cst))
				_localMasterTimingMailbox.Acknowledge(request->Version);
			else if (request->SeedSamps > 0u && request->MasterLengthSamps > 0u)
			{
			std::optional<std::uint32_t> phase;
			for (const auto& station : stations)
			{
				if (!station || station->IsRemote()) continue;
				phase = station->MasterPhaseAtAudioBoundary(request->TargetTake,
					request->TargetLoop, request->MasterLengthSamps);
				if (phase) break;
			}
			if (phase)
			{
				_preparedLocalMasterTiming = *request;
				_preparedLocalMasterPhase = *phase;
				_preparedLocalMasterSceneSample = timingClock->SceneSamplePos();
				_localMasterTimingMailbox.Acknowledge(request->Version);
				const auto writing = _localMasterReceiptSequence.fetch_add(1u, std::memory_order_seq_cst) + 1u;
				_localMasterReceiptVersion.store(request->Version, std::memory_order_seq_cst);
				_localMasterReceiptOldAbsolute.store(timingClock->AbsoluteSamplePos(), std::memory_order_seq_cst);
				_localMasterReceiptNewPhase.store(*phase, std::memory_order_seq_cst);
				_localMasterReceiptNewLength.store(request->MasterLengthSamps, std::memory_order_seq_cst);
				_localMasterReceiptSequence.store(writing + 1u, std::memory_order_seq_cst);
			}
		}
		}
		const auto& prepared = _preparedLocalMasterTiming;
		if (prepared.Version == 0u ||
			_localMasterCancelledVersion.load(std::memory_order_seq_cst) == prepared.Version ||
			_localMasterCommitVersion.load(std::memory_order_seq_cst) != prepared.Version) return;
		// The selected master may have been removed after phase capture.
		bool targetPublished = false;
		for (const auto& station : stations)
			if (station && !station->IsRemote() &&
				station->MasterPhaseAtAudioBoundary(prepared.TargetTake,
					prepared.TargetLoop, prepared.MasterLengthSamps))
				targetPublished = true;
		if (!targetPublished) return;
		// Preflight the exact published playback membership before changing any grid.
		bool claimed = true;
		for (const auto& station : stations)
			if (station && !station->IsRemote() &&
				!station->TryClaimPreparedMidiQuantisationAtAudioBoundary())
				claimed = false;
		if (!claimed)
		{
			for (const auto& station : stations)
				if (station && !station->IsRemote())
					station->ReleasePreparedMidiQuantisationClaimAtAudioBoundary();
			_localMasterPreparationRetryVersion.store(prepared.Version, std::memory_order_seq_cst);
			return;
		}
		const auto elapsed = timingClock->SceneSamplePos() - _preparedLocalMasterSceneSample;
		const auto phase = static_cast<std::uint32_t>((_preparedLocalMasterPhase + elapsed)
			% prepared.MasterLengthSamps);
		for (const auto& station : stations)
			if (station && !station->IsRemote()) station->ActivatePreparedMidiQuantisationAtAudioBoundary();
		timingClock->SetQuantisation(prepared.SeedSamps, prepared.Type);
		timingClock->SetSeedSourceLength(prepared.MasterLengthSamps);
		timingClock->InitialiseAbsoluteSamplePos(phase);
		for (const auto& station : stations)
			if (station && !station->IsRemote())
				station->ReleasePreparedMidiQuantisationClaimAtAudioBoundary();
		_localMasterActivatedVersion.store(prepared.Version, std::memory_order_seq_cst);
		_preparedLocalMasterTiming.Version = 0u;
	}

	AudioHost::AudioHost(io::UserConfig userConfig) :
		_userConfig(userConfig),
		_tickUserConfig(_userConfig),
		_channelMixer(std::make_shared<audio::ChannelMixer>(audio::ChannelMixerParams{}))
	{
	}

	AudioHost::~AudioHost()
	{
		// AudioDevice must prevent further callback invocations before this host
		// is destroyed. A failed TryStop closes engine admission and drains the
		// current callback, but only the backend can guarantee no later userdata call.
		Close();
	}

	bool AudioHost::Init(std::shared_ptr<ninjam::NinjamController> ninjamController,
		TickCallback tickCallback, bool generatedRig, const AsioInventory* inventory)
	{
		std::scoped_lock lock(_audioMutex);
		_lastInitReplacedStream = false;
		if (_audioDevice)
		{
			// Admission protects engine fields; only a successful backend stop
			// permits replacing the driver and its output-buffer geometry.
			if (!_audioDevice->TryStop()) return false;
			_lastInitReplacedStream = true;
			while ((_callbackAdmission.load(std::memory_order_acquire) & 1u) != 0u)
				std::this_thread::yield();
			_lastAudioStations.reset();
			_lastAudioTimingClock.reset();
			_audioAppliedRigSnapshot.reset();
			_audioDevice.reset();
			_publishedStreamParams.store({}, std::memory_order_release);
		}
		if ((_callbackAdmission.load(std::memory_order_acquire) & _CallbackAdmissionClosed) != 0u &&
			!CallbackAdmissionIsQuiescent()) return false;
		_lastInitReplacedStream = true;

		_ninjamController = ninjamController;
		_tickCallback = tickCallback;

		_audioSampleCounter.store(0u, std::memory_order_release);
		_audioCallbackHeartbeat.store(0u, std::memory_order_release);
		const auto prepareForStart = [this](const AudioStreamParams& audioStreamParams)
		{
			_preparedStreamParams = audioStreamParams;
			_callbackOutputChannels.store(audioStreamParams.NumOutputChannels, std::memory_order_release);
			_ninjamMetronome.Configure(audioStreamParams.SampleRate);

			auto inLatency = (0u == audioStreamParams.InputLatency) ?
				_userConfig.Audio.LatencyIn :
				audioStreamParams.InputLatency;

			_channelMixer->SetParams(audio::ChannelMixerParams({
					_userConfig.AdcBufferDelay(inLatency) + audioStreamParams.BufSize,
					audio::ChannelMixer::DefaultBufferSize,
					audioStreamParams.NumInputChannels,
					audioStreamParams.NumOutputChannels }));

			auto stationsSnapshot = _audioStations.load();
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

		};
		auto dev = audio::AudioDevice::Open(AudioHost::AudioCallback,
			[](RtAudioError::Type type, const std::string& err) { std::cout << "[" << type << " RtAudio Error] " << err << std::endl; },
			_userConfig.Audio, this, generatedRig, inventory, &_asioOpenReport, prepareForStart);
		if (!dev)
		{
			_publishedStreamParams.store({}, std::memory_order_release);
			return false;
		}
		_audioDevice = std::move(*dev);
		_publishedStreamParams.store(std::make_shared<const AudioStreamParams>(
			_audioDevice->GetAudioStreamParams()), std::memory_order_release);
		return true;
	}

	void AudioHost::Close()
	{
		CloseCallbackAdmission();
		const bool deviceStopped = !_audioDevice || _audioDevice->TryStop();
		// Closing admission prevents another engine callback from entering. Wait
		// for an already admitted callback before Scene releases plugin state,
		// even when the backend could not confirm that its stream stopped.
		while (!CallbackAdmissionIsQuiescent())
			std::this_thread::yield();

		std::scoped_lock lock(_audioMutex);

		if (deviceStopped && _ninjamController)
			_ninjamController->Stop();

		{
			std::scoped_lock retainedLock(_retainedRigSnapshotsMutex);
			if (deviceStopped && CallbackAdmissionIsQuiescent())
			{
				_pendingRigSnapshot.store(nullptr);
				_audioAppliedRigSnapshot.reset();
				_lastAudioStations.reset();
				_lastAudioTimingClock.reset();
				_retainedRigSnapshots.clear();
				_pendingRigSnapshot.collect();
				_rigTriggerTransitionCandidate.collect();
				_audioStations.collect();
				_timingClock.collect();
			}
		}
	}

	void AudioHost::SetStations(std::shared_ptr<const std::vector<std::shared_ptr<Station>>> stations)
	{
		std::scoped_lock lock(_retainedRigSnapshotsMutex);
		_audioStations.store(stations);
	}

	void AudioHost::PublishPendingRigSnapshot(std::shared_ptr<const engine::RigSnapshot> snapshot)
	{
		std::scoped_lock lock(_retainedRigSnapshotsMutex);
		if (snapshot)
			_retainedRigSnapshots.push_back(snapshot);
		_pendingRigSnapshot.store(std::move(snapshot));
	}

	void AudioHost::RequestRigTriggerTransition(std::uint64_t candidateRevision,
		std::shared_ptr<const engine::RigSnapshot> acceptedSnapshot,
		std::shared_ptr<const engine::RigSnapshot> candidateSnapshot)
	{
		_transitionReadyRigRevision.store(0u, std::memory_order_release);
		_rejectedRigRevision.store(0u, std::memory_order_release);
		_rigTriggerTransitionAcceptedRevision.store(
			acceptedSnapshot ? acceptedSnapshot->Revision : 0u, std::memory_order_release);
		std::scoped_lock lock(_retainedRigSnapshotsMutex);
		_rigTriggerTransitionCandidate.store(std::move(candidateSnapshot));
		_rigTriggerTransitionRequestRevision.store(candidateRevision, std::memory_order_release);
	}

	void AudioHost::ClearRigTriggerTransition()
	{
		_rigTriggerTransitionRequestRevision.store(0u, std::memory_order_release);
		_rigTriggerTransitionAcceptedRevision.store(0u, std::memory_order_release);
		std::scoped_lock lock(_retainedRigSnapshotsMutex);
		_rigTriggerTransitionCandidate.store({});
	}

	void AudioHost::ReleaseRigSnapshotsBefore(std::uint64_t revision)
	{
		{
			std::scoped_lock lock(_retainedRigSnapshotsMutex);
			_retainedRigSnapshots.erase(std::remove_if(_retainedRigSnapshots.begin(),
				_retainedRigSnapshots.end(),
				[revision](const std::shared_ptr<const engine::RigSnapshot>& snapshot)
				{
					return snapshot && snapshot->Graph.Revision < revision;
				}), _retainedRigSnapshots.end());
		}
		CollectRetiredSnapshots();
	}

	void AudioHost::CollectRetiredSnapshots()
	{
		std::scoped_lock lock(_retainedRigSnapshotsMutex);
		_pendingRigSnapshot.collect();
		_rigTriggerTransitionCandidate.collect();
		_audioStations.collect();
		_timingClock.collect();
	}

	void AudioHost::ApplyPendingRigSnapshotAtAudioBoundary() noexcept
	{
		auto snapshot = _pendingRigSnapshot.borrow();
		if (!snapshot)
			return;
		if (snapshot->Graph.Revision <= _audioRigRevision)
			return;

		for (const auto& routeChange : snapshot->TriggerRouteUpdates)
		{
			if (routeChange.CandidateIndex < snapshot->Triggers.size())
			{
				auto& trigger = snapshot->Triggers[routeChange.CandidateIndex];
				trigger.Instance->ApplyCaptureRouting(trigger.Receiver, trigger.InputChannels,
					trigger.MidiInputDevices, trigger.MidiInputMode,
					trigger.OverdubMixer, trigger.OverdubWriter);
			}
		}

		_audioRigRevision = snapshot->Graph.Revision;
		_audioAppliedRigSnapshot = std::move(snapshot);
		_appliedRigRevision.store(_audioRigRevision, std::memory_order_release);
	}

	void AudioHost::PublishRigTriggerTransitionAtAudioBoundary() noexcept
	{
		const auto candidateRevision = _rigTriggerTransitionRequestRevision.load(std::memory_order_acquire);
		if (candidateRevision == 0u ||
			_rigTriggerTransitionAcceptedRevision.load(std::memory_order_acquire) != _audioRigRevision)
			return;
		auto snapshot = _pendingRigSnapshot.borrow();
		if (!snapshot || snapshot->Revision != _audioRigRevision)
			return;

		auto candidate = _rigTriggerTransitionCandidate.borrow();
		if (!candidate || candidate->Revision != candidateRevision)
			return;
		for (const auto& retired : candidate->TriggerReplacementChecks)
		{
			const auto& trigger = retired.AcceptedInstance;
			if (trigger && !trigger->CanEditRouting())
			{
				_rejectedRigRevision.store(candidateRevision, std::memory_order_release);
				return;
			}
		}
		for (const auto& routeChange : candidate->TriggerRouteUpdates)
		{
			const auto& trigger = routeChange.Instance;
			if (trigger && !trigger->CanApplyCaptureRouting(routeChange.ReceiverChanged))
			{
				_rejectedRigRevision.store(candidateRevision, std::memory_order_release);
				return;
			}
		}
		_transitionReadyRigRevision.store(candidateRevision, std::memory_order_release);
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

std::optional<ninjam::NinjamDesiredTimingReceipt> AudioHost::LastAppliedDesiredTiming() const noexcept
{
	for (auto attempt = 0u; attempt < 2u; ++attempt)
	{
		const auto before = _lastAppliedTimingReceiptSequence.load(std::memory_order_acquire);
		if (before == 0u || (before & 1u) != 0u)
			return std::nullopt;

		const ninjam::NinjamDesiredTimingReceipt receipt{
			_lastAppliedDesiredVersion.load(std::memory_order_relaxed),
			_lastAppliedSessionEpoch.load(std::memory_order_relaxed),
			_lastAppliedDesiredGeneration.load(std::memory_order_relaxed),
			_lastAppliedDesiredIntent.load(std::memory_order_relaxed),
			_lastAppliedDesiredPolicy.load(std::memory_order_relaxed),
			_lastAppliedTimingSceneCoordinate.load(std::memory_order_relaxed),
			_lastAppliedLocalSourceCorrectionSamps.load(std::memory_order_relaxed)
		};
		if (before == _lastAppliedTimingReceiptSequence.load(std::memory_order_acquire))
			return receipt;
	}

	return std::nullopt;
}

bool AudioHost::ApplyDesiredTimingAtAudioBoundary(std::uint64_t blockStartSample,
	unsigned int sampleRate) noexcept
{
	// A failed bounded read must leave the desired state eligible for the next block.
	const auto stationsSnapshot = _audioStations.borrow();
	const auto timingClock = _timingClock.borrow();
	if (!stationsSnapshot.ReadSucceeded() || !timingClock.ReadSucceeded())
		return false;
	return ApplyDesiredTimingAtAudioBoundary(blockStartSample, sampleRate,
		stationsSnapshot.get(), timingClock.get());
}

bool AudioHost::ApplyDesiredTimingAtAudioBoundary(std::uint64_t blockStartSample,
	unsigned int sampleRate,
	const std::vector<std::shared_ptr<engine::Station>>* stations,
	utils::Timer* timingClock) noexcept
{
	const auto desiredValue = _ninjamDesiredTimingMailbox.ReadLatest();
	_mappedSourceHandledAtCommandBoundary = false;
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
		_captureMappedSourceAnchorsAfterOffset = false;
		if (timingClock)
		{
			utils::Timer::Command reset;
			reset.Type = utils::Timer::CommandType::Invalidate;
			timingClock->ApplyCommand(reset);
			timingClock->ResetMusicalTransport();
		}
	}

	long long remoteMasterCorrectionSamps = 0;
	long long localSourceCorrectionSamps = 0;
	std::uint64_t sceneCoordinate = timingClock ? timingClock->SceneSamplePos() : 0u;
	if (!noSync && timingClock && desired.RemoteMasterIntervalLengthSamps > 0ul
		&& desired.HasRemotePhaseDeviceSample)
	{
		const auto geometryChanged = !_appliedNinjamTiming.HasRemoteTiming
			|| desired.RemoteMasterIntervalLengthSamps != _appliedNinjamTiming.RemoteMasterIntervalLengthSamps
			|| desired.RemoteGridStepSamps != _appliedNinjamTiming.RemoteGridStepSamps
			|| desired.BeatsPerInterval != _appliedNinjamTiming.BeatsPerInterval
			|| desired.TempoBpm != _appliedNinjamTiming.TempoBpm
			|| desired.Quantisation != _appliedNinjamTiming.Quantisation;
		const auto hadSyncPhaseMap = _syncPhaseMap.IsActive();
		const auto previousMasterLength = timingClock->SeedSourceLength();
		const auto previousRemotePhase = timingClock->SampOffset();
		const auto mappedSourceBefore = hadSyncPhaseMap
			? RestoreMappedSourceAtScene(stations, sceneCoordinate)
			: std::optional<std::int64_t>{};
		_mappedSourceHandledAtCommandBoundary = true;

		const auto replacement = ninjam::ResolveBoundaryTimingReplacement(
			timingClock->SeedSourceLength(), timingClock->SampOffset(),
			static_cast<unsigned int>(desired.RemoteMasterIntervalLengthSamps), desired.RemoteMasterPhaseSamps,
			std::optional<std::uint64_t>{ desired.RemotePhaseDeviceSample }, blockStartSample);
		utils::Timer::Command timerCommand;
		timerCommand.Generation = desired.Generation;
		timerCommand.SeedLengthSamps = desired.RemoteMasterIntervalLengthSamps;
		timerCommand.QuantiseSamps = desired.RemoteGridStepSamps;
		timerCommand.Quantisation = desired.Quantisation;
		if (geometryChanged)
		{
			timerCommand.Type = utils::Timer::CommandType::ReplaceTiming;
			timerCommand.PhaseDeltaSamps = static_cast<long long>(replacement.RemoteMasterPhaseSamps);
			if (_syncPhaseMap.SourceLengthSamps == 0ul)
			{
				_syncPhaseMap.SourceLengthSamps = previousMasterLength > 0ul
					? previousMasterLength : desired.RemoteMasterIntervalLengthSamps;
			}
			const auto sourceLength = _syncPhaseMap.SourceLengthSamps;
			if (sourceLength > 0ul)
			{
				const auto sourceBefore = mappedSourceBefore.has_value()
					? static_cast<unsigned long>(ninjam::PositiveModulo(
						mappedSourceBefore.value(), sourceLength))
					: static_cast<unsigned long>(previousRemotePhase) % sourceLength;
				const auto sourceAfter = ninjam::SourcePhaseAtRemotePhase(replacement.RemoteMasterPhaseSamps,
					sourceLength, desired.RemoteMasterIntervalLengthSamps);
				localSourceCorrectionSamps = ninjam::SignedCircularDifference(static_cast<unsigned int>(sourceBefore),
					static_cast<unsigned int>(sourceAfter), static_cast<unsigned int>(sourceLength));
			}
		}
		else
		{
			timerCommand.Type = utils::Timer::CommandType::PhaseCorrection;
			remoteMasterCorrectionSamps = ninjam::SignedCircularDifference(
				timingClock->SampOffset(), replacement.RemoteMasterPhaseSamps,
				static_cast<unsigned int>(desired.RemoteMasterIntervalLengthSamps));
			timerCommand.PhaseDeltaSamps = remoteMasterCorrectionSamps;
			localSourceCorrectionSamps = remoteMasterCorrectionSamps;
		}

		_activeNinjamFollowPolicy = desired.LocalFollowPolicy;
		if (geometryChanged)
			timingClock->ReanchorMusicalTransport(sceneCoordinate, replacement.RemoteMasterPhaseSamps,
				desired.RemoteMasterIntervalLengthSamps, desired.BeatsPerInterval, desired.TempoBpm, sampleRate);
		else
			timingClock->ReanchorMusicalTransportCurrentGeometry(sceneCoordinate,
				replacement.RemoteMasterPhaseSamps, sampleRate);
		timingClock->ApplyCommand(timerCommand);
		if (!geometryChanged && mappedSourceBefore.has_value())
		{
			const auto sourceBefore = static_cast<unsigned long>(ninjam::PositiveModulo(
				mappedSourceBefore.value(), _syncPhaseMap.SourceLengthSamps));
			const auto sourceAfter = ninjam::SourcePhaseAtRemotePhase(timingClock->SampOffset(),
				_syncPhaseMap.SourceLengthSamps, _syncPhaseMap.RemoteLengthSamps);
			localSourceCorrectionSamps = ninjam::SignedCircularDifference(static_cast<unsigned int>(sourceBefore),
				static_cast<unsigned int>(sourceAfter), static_cast<unsigned int>(_syncPhaseMap.SourceLengthSamps));
		}

		if (stations)
			for (const auto& station : *stations)
				if (station && !station->IsRemote()) station->ApplyAcceptedTimingCorrection(
					localSourceCorrectionSamps, desired.Generation, geometryChanged || !hadSyncPhaseMap);

		if (geometryChanged)
			_syncPhaseMap.RemoteLengthSamps = desired.RemoteMasterIntervalLengthSamps;
		const auto sourcePhase = ninjam::SourcePhaseAtRemotePhase(timingClock->SampOffset(),
			_syncPhaseMap.SourceLengthSamps, _syncPhaseMap.RemoteLengthSamps);
		const auto sourceCoordinate = mappedSourceBefore.has_value()
			? mappedSourceBefore.value() + localSourceCorrectionSamps
			: static_cast<std::int64_t>(sourcePhase);
		_syncPhaseMap.Rebase(sceneCoordinate, sourceCoordinate);
		_captureMappedSourceAnchorsAfterOffset = !hadSyncPhaseMap;
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
	_lastAppliedLocalSourceCorrectionSamps.store(localSourceCorrectionSamps, std::memory_order_relaxed);
	_lastAppliedTimingReceiptSequence.store(writingReceipt + 1u, std::memory_order_release);
	return true;
}

std::optional<std::int64_t> AudioHost::RestoreMappedSourceAtScene(
	const std::vector<std::shared_ptr<engine::Station>>* stations,
	std::uint64_t sceneCoordinateSamps) noexcept
{
	if (!_syncPhaseMap.IsActive())
		return std::nullopt;
	const auto sourceCoordinate = _syncPhaseMap.SourceCoordinateAt(sceneCoordinateSamps);
	if (stations)
		for (const auto& station : *stations)
			if (station && !station->IsRemote())
				station->RestoreMappedSourceCoordinate(sourceCoordinate);
	return sourceCoordinate;
}

void AudioHost::CaptureMappedSourceAnchorsAfterOffset(
	const std::vector<std::shared_ptr<engine::Station>>& stations) noexcept
{
	if (!_captureMappedSourceAnchorsAfterOffset)
		return;
	for (const auto& station : stations)
		if (station && !station->IsRemote())
			station->CaptureMappedSourceAnchors(_syncPhaseMap.SourceCoordinateAtOrigin);
	_captureMappedSourceAnchorsAfterOffset = false;
}

void AudioHost::PublishAudioCallbackProgress(std::uint64_t blockStartSample,
	unsigned int numSamps) noexcept
{
	const auto nextSample = blockStartSample + numSamps;
	_audioSampleCounter.store(nextSample, std::memory_order_release);
	midi::PublishMidiClockAnchor(_midiClockAnchor, nextSample,
		std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());
}

	void AudioHost::ApplyLocalTransportOffsetAtAudioBoundary(
		const std::vector<std::shared_ptr<engine::Station>>& stations,
		utils::Timer* timingClock) noexcept
	{
		if (const auto localTransportOffsetLoopFrac = _localTransportOffsetLoopFracMailbox.ConsumeLatest())
			_localTransportOffsetLoopFrac = localTransportOffsetLoopFrac.value();

		const auto masterLength = timingClock ? timingClock->SeedSourceLength() : 0ul;
		if (masterLength != _localTransportOffsetMasterLength)
			_localTransportOffsetMasterLength = masterLength;
		const auto localTransportOffsetTargetSamps = masterLength == 0ul ? 0 :
			static_cast<long long>(std::llround(
				_localTransportOffsetLoopFrac * static_cast<double>(masterLength)));
		if (localTransportOffsetTargetSamps == _localTransportOffsetTargetSamps)
			return;

		_localTransportOffsetTargetSamps = localTransportOffsetTargetSamps;
		for (const auto& station : stations)
			if (station && !station->IsRemote())
				station->SetLocalTransportOffsetSamps(localTransportOffsetTargetSamps);
	}

	void AudioHost::CloseCallbackAdmission() noexcept
	{
		_callbackAdmission.fetch_or(_CallbackAdmissionClosed, std::memory_order_acq_rel);
	}

	bool AudioHost::CallbackAdmissionIsQuiescent() const noexcept
	{
		return _callbackAdmission.load(std::memory_order_acquire) == _CallbackAdmissionClosed;
	}

	bool AudioHost::ReopenCallbackAdmission() noexcept
	{
		auto expected = _CallbackAdmissionClosed;
		return _callbackAdmission.compare_exchange_strong(expected, 0u,
			std::memory_order_release, std::memory_order_relaxed);
	}

	bool AudioHost::ApplyPendingRigWhileQuiescent() noexcept
	{
		if (!CallbackAdmissionIsQuiescent()) return false;
		ApplyPendingRigSnapshotAtAudioBoundary();
		return true;
	}

	bool AudioHost::TryEnterCallback() noexcept
	{
		std::uint32_t expected = 0u;
		return _callbackAdmission.compare_exchange_strong(expected, 1u,
			std::memory_order_acquire, std::memory_order_relaxed);
	}

	void AudioHost::LeaveCallback() noexcept
	{
		_callbackAdmission.fetch_sub(1u, std::memory_order_release);
	}

	int AudioHost::AudioCallback(void* outBuffer,
		void* inBuffer,
		unsigned int numSamps,
		double streamTime,
		RtAudioStreamStatus status,
		void* userData)
	{
		AudioHost* engine = (AudioHost*)userData;
		if (!engine->TryEnterCallback())
		{
			if (outBuffer)
				std::fill_n(static_cast<float*>(outBuffer),
					static_cast<std::size_t>(numSamps) *
					engine->_callbackOutputChannels.load(std::memory_order_acquire), 0.0f);
			return 0;
		}
		CallbackLease callbackLease(*engine);
		engine->_OnAudio((float*)inBuffer, (float*)outBuffer, numSamps, streamTime);
		return 0;
	}

	void AudioHost::_OnAudio(float* inBuf,
		float* outBuf,
		unsigned int numSamps,
		double streamTime)
	{
		auto stationsSnapshot = _audioStations.borrow();
		auto timingClockSnapshot = _timingClock.borrow();
		if (stationsSnapshot.ReadSucceeded() && timingClockSnapshot.ReadSucceeded())
		{
			_lastAudioStations = std::move(stationsSnapshot);
			_lastAudioTimingClock = std::move(timingClockSnapshot);
		}

		_mappedSourceHandledAtCommandBoundary = false;
		_audioCallbackHeartbeat.fetch_add(1u, std::memory_order_relaxed);
		const auto blockStartSample = _audioSampleCounter.load(std::memory_order_relaxed);
		if (!_lastAudioStations.ReadSucceeded() || !_lastAudioTimingClock.ReadSucceeded())
		{
			if (outBuf)
				std::fill_n(outBuf, static_cast<std::size_t>(numSamps) *
					_callbackOutputChannels.load(std::memory_order_acquire), 0.0f);
			if (_lastAudioTimingClock)
				_lastAudioTimingClock->AdvanceMusicalTransport();
			if (_tickCallback)
				_tickCallback(Timer::GetTime(), numSamps,
					_tickUserConfig, _preparedStreamParams);
			PublishAudioCallbackProgress(blockStartSample, numSamps);
			return;
		}

		ApplyPendingRigSnapshotAtAudioBoundary();
		const auto& audioStreamParams = *_preparedStreamParams;
		const auto* stationsSnapshotValue = _lastAudioStations.get();
		const auto& stations = stationsSnapshotValue ? *stationsSnapshotValue : _emptyStations;
		auto* timingClock = _lastAudioTimingClock.get();
		if (timingClock)
			for (const auto& station : stations)
				if (station && !station->IsRemote()) station->ApplyFirstMasterRecordingAtAudioBoundary(*timingClock);
		ApplyLocalMasterTimingAtAudioBoundary(stations, timingClock);
		ApplyDesiredTimingAtAudioBoundary(blockStartSample, audioStreamParams.SampleRate,
			stationsSnapshotValue, timingClock);

		if (timingClock)
			timingClock->AdvanceMusicalTransport();
		ApplyLocalTransportOffsetAtAudioBoundary(stations, timingClock);
		// Offset is loop-local, so capture entity anchors only after every local take
		// has applied it. This is a single post-command fan-out, rather than a
		// speculative capture before the offset is known.
		CaptureMappedSourceAnchorsAfterOffset(stations);

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
				station->OnBounce(numSamps, _userConfig, _preparedStreamParams);

				station->SetSourceType(Audible::AUDIOSOURCE_BOUNCE);
				station->OnBounce(numSamps, _userConfig, _preparedStreamParams);

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
					liveTiming.LocalMasterAbsoluteSampleAtObservation = localTransport->AbsoluteSamplePos;
				}
				_ninjamTimingMailbox.Publish(liveTiming);
			};

		if (_activeNinjamFollowPolicy != ninjam::NinjamLocalFollowPolicy::NoSync
			&& timingClock && !_mappedSourceHandledAtCommandBoundary)
			RestoreMappedSourceAtScene(&stations, timingClock->SceneSamplePos());

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
					timingInput.IntervalPositionSamps = liveTiming.IntervalPositionSamps;
					timingInput.IntervalLengthSamps = liveTiming.IntervalLengthSamps;
					timingInput.Bpm = liveTiming.Bpm;
					timingInput.Bpi = liveTiming.Bpi;
					timingInput.DeviceSampleRate = liveTiming.DeviceSampleRate;
					timingInput.OutputLatencySamps = audioStreamParams.OutputLatency == 0u ?
						_userConfig.Audio.LatencyOut : audioStreamParams.OutputLatency;
					timingInput.NumFrames = numSamps;

					const auto metronomeTiming = ninjam::NinjamMetronomeTiming::Compute(
						timingInput, _ninjamMetronomeTimingState);
					if (metronomeTiming.GenerationReset)
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

		if (_audioAppliedRigSnapshot)
		{
			for (const auto& trigger : _audioAppliedRigSnapshot->Triggers)
			{
				if (trigger.Instance)
					trigger.Instance->OnTick(Timer::GetTime(), numSamps,
						_tickUserConfig, _preparedStreamParams, _audioRigRevision);
			}
		}

		if (_tickCallback)
		{
			_tickCallback(Timer::GetTime(), numSamps, _tickUserConfig, _preparedStreamParams);
		}
		for (const auto& station : stations)
			if (station) station->AcknowledgeAudioBoundary();
		// Decide after Trigger::OnTick so this transition uses the completed boundary's state.
		PublishRigTriggerTransitionAtAudioBoundary();

		PublishAudioCallbackProgress(blockStartSample, numSamps);
	}
}
