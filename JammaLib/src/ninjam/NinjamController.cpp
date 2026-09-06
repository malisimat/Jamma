#include "NinjamController.h"

#include <iostream>

using namespace ninjam;

void NinjamController::LoadConfig(const std::optional<io::JamFile::NinjamConfig>& config)
{
	_config = config;
	if (_config.has_value())
	{
		_session.Start(_config.value());
	}
	else
	{
		_session.Stop();
		std::scoped_lock lock(_pendingSnapshotMutex);
		_pendingSnapshot.reset();
	}
}

void NinjamController::SetAudioFormat(unsigned int sampleRate,
	unsigned int blockSize,
	unsigned int numInputChannels,
	unsigned int numOutputChannels,
	unsigned int inLatencySamps,
	unsigned int outLatencySamps)
{
	_session.SetAudioFormat(sampleRate, blockSize, numInputChannels, numOutputChannels, inLatencySamps, outLatencySamps);
}

NinjamSessionPumpResult NinjamController::Pump()
{
	auto result = _session.Pump();
	ApplySessionPumpResult(result);
	return result;
}

void NinjamController::ApplySessionPumpResult(const NinjamSessionPumpResult& result)
{
	if (result.TimingStatus.Changed && !result.TimingStatus.IsAvailable)
	{
		std::scoped_lock lock(_pendingSnapshotMutex);
		_pendingSnapshot.reset();
		return;
	}
	if (result.Snapshot.has_value())
	{
		std::scoped_lock lock(_pendingSnapshotMutex);
		_pendingSnapshot = result.Snapshot;
	}
}

std::optional<ninjam::NinjamRemoteSnapshot> NinjamController::TakePendingSnapshot()
{
	std::scoped_lock lock(_pendingSnapshotMutex);
	if (!_pendingSnapshot.has_value())
		return std::nullopt;

	auto snapshot = std::move(_pendingSnapshot);
	_pendingSnapshot.reset();
	return snapshot;
}

void NinjamController::SendChat(const std::string& msg)
{
	_session.SendChat(msg);
}

void NinjamController::Connect(const std::string& host)
{
	io::JamFile::NinjamConfig config;
	if (_config.has_value())
		config = _config.value();

	config.Host = host;
	_config = config;

	std::cout << "[NINJAM] Connecting to " << host << std::endl;
	_session.Start(config);
}

void NinjamController::Disconnect()
{
	std::cout << "[NINJAM] Disconnecting" << std::endl;
	_session.Stop();
	std::scoped_lock lock(_pendingSnapshotMutex);
	_pendingSnapshot.reset();
}

void NinjamController::Stop()
{
	_session.Stop();
	std::scoped_lock lock(_pendingSnapshotMutex);
	_pendingSnapshot.reset();
}

NinjamRemoteTiming NinjamController::ProcessExportBlock(const float* interleavedDacOutput,
	unsigned int numDacChannels,
	const float* interleavedAdcInput,
	unsigned int numAdcChannels,
	unsigned int numFrames,
	unsigned int sampleRate,
	std::uint64_t audioBlockStartSample)
{
	return _session.ProcessExportBlock(interleavedDacOutput,
		numDacChannels,
		interleavedAdcInput,
		numAdcChannels,
		numFrames,
		sampleRate,
		audioBlockStartSample);
}

NinjamConnectionUse NinjamController::AcquireConnectionUse() const noexcept
{
	return NinjamConnectionUse(_session);
}
