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
	// Read console input event-by-event so _chatInputStop is checked after
	// every key press and Stop() always returns promptly without waiting for
	// the user to press Enter.
	HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);

	if (hStdin == nullptr || hStdin == INVALID_HANDLE_VALUE)
	{
		std::cout << "[NINJAM] Chat input unavailable - no stdin handle" << std::endl;
		return;
	}

	std::string line;

	while (!_chatInputStop.load())
	{
		DWORD waitResult = WaitForSingleObject(hStdin, 100 /*ms*/);
		if (waitResult == WAIT_TIMEOUT)
			continue;

		if (waitResult == WAIT_FAILED)
		{
			DWORD error = GetLastError();
			std::cout << "[NINJAM] Chat input unavailable - stdin wait failed (" << error << ")" << std::endl;
			break;
		}

		if (waitResult != WAIT_OBJECT_0)
			break;

		if (_chatInputStop.load())
			break;

		DWORD numEvents = 0;
		if (!GetNumberOfConsoleInputEvents(hStdin, &numEvents) || numEvents == 0)
			continue;

		// Read up to 64 events at a time; loop back to check _chatInputStop
		// between batches so shutdown is never delayed more than one batch.
		INPUT_RECORD records[64];
		DWORD numRead = 0;
		if (!ReadConsoleInput(hStdin, records, sizeof(records) / sizeof(records[0]), &numRead))
			break;

		for (DWORD i = 0; i < numRead; ++i)
		{
			if (_chatInputStop.load())
				return;

			if (records[i].EventType != KEY_EVENT || !records[i].Event.KeyEvent.bKeyDown)
				continue;

			const char ch = records[i].Event.KeyEvent.uChar.AsciiChar;

			if (ch == '\r')
			{
				std::cout << std::endl;
				if (!line.empty())
				{
					if (_connection && _connection->IsConnected())
					{
						_connection->SendChat(line);
						std::cout << "[NINJAM] <you> " << line << std::endl;
					}
					else
					{
						std::cout << "[NINJAM] Not connected - message not sent" << std::endl;
					}
					line.clear();
				}
			}
			else if (ch == '\b')
			{
				if (!line.empty())
					line.pop_back();
			}
			else if (static_cast<unsigned char>(ch) >= 0x20)
			{
				line += ch;
			}
		}
	}
}
