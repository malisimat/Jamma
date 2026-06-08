#include "NinjamController.h"

#include <iostream>

using namespace engine;

void NinjamController::LoadConfig(const std::optional<io::JamFile::NinjamConfig>& config)
{
	_config = config;
	if (_config.has_value())
		_session.Start(_config.value());
	else
		_session.Stop();
}

void NinjamController::SetAudioFormat(unsigned int sampleRate,
	unsigned int blockSize,
	unsigned int numInputChannels,
	unsigned int numOutputChannels)
{
	_session.SetAudioFormat(sampleRate, blockSize, numInputChannels, numOutputChannels);
}

std::optional<io::NinjamRemoteSnapshot> NinjamController::Pump()
{
	auto snapshot = _session.Pump();
	if (snapshot.has_value())
	{
		std::scoped_lock lock(_pendingSnapshotMutex);
		_pendingSnapshot = snapshot;
	}
	return snapshot;
}

std::optional<io::NinjamRemoteSnapshot> NinjamController::TakePendingSnapshot()
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

void NinjamController::ProcessAudioBlock(const float* interleavedInput,
	unsigned int numFrames,
	unsigned int sampleRate)
{
	_session.ProcessAudioBlock(interleavedInput, numFrames, sampleRate);
}

bool NinjamController::ConsumeStereoPair(unsigned int outChannelLeft,
	const float*& left,
	const float*& right,
	unsigned int& numFrames) const
{
	return _session.ConsumeStereoPair(outChannelLeft, left, right, numFrames);
}