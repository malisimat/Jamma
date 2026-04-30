#include "NinjamSession.h"

#include <iostream>
#include <windows.h>

using namespace engine;

NinjamSession::~NinjamSession()
{
	Stop();
}

void NinjamSession::Start(const io::JamFile::NinjamConfig& config)
{
	Stop();

	if (config.Host.empty() || config.User.empty())
		return;

	_connection = std::make_unique<io::NinjamConnection>(
		config.Host, config.User, config.Pass, config.WorkDir);

	std::cout << "[NINJAM] Auto-connect enabled from JAM config" << std::endl;
	std::cout << "[NINJAM] Type a message and press Enter to chat" << std::endl;
	_connection->Connect();

	_chatInputStop = false;
	_chatInputThread = std::thread([this]() { _ChatInputLoop(); });
}

void NinjamSession::Stop()
{
	_chatInputStop = true;
	if (_chatInputThread.joinable())
		_chatInputThread.join();

	if (_connection)
	{
		_connection->Disconnect();
		_connection.reset();
	}
}

bool NinjamSession::IsConnected() const noexcept
{
	return _connection && _connection->IsConnected();
}

std::optional<io::NinjamRemoteSnapshot> NinjamSession::Pump()
{
	if (!_connection)
		return std::nullopt;

	_connection->Pump();

	if (!_connection->IsConnected())
		return std::nullopt;

	return _connection->Snapshot();
}

void NinjamSession::SetAudioFormat(unsigned int sampleRate,
	unsigned int blockSize,
	unsigned int numInputChannels,
	unsigned int numOutputChannels)
{
	if (_connection)
		_connection->SetAudioFormat(sampleRate, blockSize, numInputChannels, numOutputChannels);
}

void NinjamSession::ProcessAudioBlock(const float* interleavedInput,
	unsigned int numFrames,
	unsigned int sampleRate)
{
	if (_connection)
		_connection->ProcessAudioBlock(interleavedInput, numFrames, sampleRate);
}

bool NinjamSession::ConsumeStereoPair(unsigned int outChannelLeft,
	const float*& left,
	const float*& right,
	unsigned int& numFrames) const
{
	if (!_connection)
		return false;

	return _connection->ConsumeStereoPair(outChannelLeft, left, right, numFrames);
}

void NinjamSession::_ChatInputLoop()
{
	// Poll stdin with a short timeout so the loop responds promptly to Stop().
	HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
	std::string line;

	while (!_chatInputStop.load())
	{
		DWORD waitResult = WaitForSingleObject(hStdin, 100 /*ms*/);
		if (waitResult != WAIT_OBJECT_0)
			continue;

		if (_chatInputStop.load())
			break;

		if (!std::getline(std::cin, line))
			break;

		if (_chatInputStop.load())
			break;

		if (line.empty())
			continue;

		// Writes from this thread and from the NJClient callback thread can
		// interleave on cout; individual lines remain intact thanks to endl
		// flushing, though output may occasionally overlap while typing.
		if (_connection && _connection->IsConnected())
		{
			_connection->SendChat(line);
			std::cout << "[NINJAM] <you> " << line << std::endl;
		}
		else
		{
			std::cout << "[NINJAM] Not connected - message not sent" << std::endl;
		}
	}
}
