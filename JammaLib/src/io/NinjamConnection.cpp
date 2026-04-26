#include "NinjamConnection.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <set>

#include "njclient.h"
#include "../../include/Constants.h"

using namespace io;

NinjamConnection::NinjamConnection(std::string host,
	std::string user,
	std::string pass,
	std::string workDir) :
	_host(std::move(host)),
	_user(std::move(user)),
	_pass(std::move(pass)),
	_workDir(std::move(workDir)),
	_isConnected(false),
	_state(ConnectionState::Disconnected),
	_sampleRate(constants::DefaultSampleRate),
	_blockSize(constants::DefaultBufferSizeSamps),
	_numInputChannels(0),
	_numOutputChannels(2),
	_lastNumFrames(0),
	_outScratch(),
	_inScratch(),
	_outPtrs(),
	_inPtrs(),
	_userOutputChannels(),
	_snapshot(),
	_lastError(),
	_snapshotMutex(),
	_audioBufferMutex(),
	_connectionMutex(),
	_clientRaw(new NJClient())
{
}

NinjamConnection::~NinjamConnection()
{
	Disconnect();
	delete _clientRaw;
	_clientRaw = nullptr;
}

bool NinjamConnection::Connect()
{
	std::scoped_lock lock(_connectionMutex);

	if (!_clientRaw)
	{
		_lastError = "NJClient unavailable";
		_state = ConnectionState::Failed;
		std::cout << "[NINJAM] Connection failed: " << _lastError << std::endl;
		return false;
	}

	if (_host.empty() || _user.empty())
	{
		_lastError = "Host/user not configured";
		_state = ConnectionState::Failed;
		std::cout << "[NINJAM] Connection failed: " << _lastError << std::endl;
		return false;
	}

	_state = ConnectionState::Connecting;
	_lastError.clear();
	std::cout << "[NINJAM] Connecting to " << _host << " as " << _user << std::endl;

	_EnsureWorkDir();

	_clientRaw->LicenseAgreementCallback = [](void*, const char*) { return 1; };
	_clientRaw->config_savelocalaudio = -1;
	_clientRaw->config_remote_autochan = 0;
	_clientRaw->config_remote_autochan_nch = static_cast<int>(_numOutputChannels);

	if (!_workDir.empty())
	{
		std::vector<char> mutablePath(_workDir.begin(), _workDir.end());
		mutablePath.push_back('\0');
		_clientRaw->SetWorkDir(mutablePath.data());
	}

	_clientRaw->Connect(_host.c_str(), _user.c_str(), _pass.c_str());
	_isConnected = true;
	_state = ConnectionState::Connected;
	_ConfigureLocalChannels();
	std::cout << "[NINJAM] Connected" << std::endl;

	return true;
}

void NinjamConnection::Disconnect()
{
	std::scoped_lock lock(_connectionMutex);

	if (_clientRaw)
		_clientRaw->Disconnect();

	if (_isConnected)
		std::cout << "[NINJAM] Disconnected" << std::endl;

	_isConnected = false;
	_state = ConnectionState::Disconnected;
	_userOutputChannels.clear();
	_lastNumFrames = 0;

	std::scoped_lock snapshotLock(_snapshotMutex);
	_snapshot = {};
}

bool NinjamConnection::IsConnected() const noexcept
{
	return _isConnected;
}

NinjamConnection::ConnectionState NinjamConnection::State() const noexcept
{
	return _state.load();
}

std::string NinjamConnection::LastError() const
{
	std::scoped_lock lock(_connectionMutex);
	return _lastError;
}

void NinjamConnection::Pump()
{
	if (!_isConnected || !_clientRaw)
		return;

	while (!_clientRaw->Run()) {}
	_RefreshSnapshot();
}

void NinjamConnection::SetAudioFormat(unsigned int sampleRate,
	unsigned int blockSize,
	unsigned int numInputChannels,
	unsigned int numOutputChannels)
{
	_sampleRate = sampleRate > 0 ? sampleRate : constants::DefaultSampleRate;
	_blockSize = blockSize > 0 ? blockSize : constants::DefaultBufferSizeSamps;
	_numInputChannels = numInputChannels;
	_numOutputChannels = numOutputChannels > 0 ? numOutputChannels : 2u;

	_EnsureScratchBuffers(_blockSize);

	if (_clientRaw)
	{
		_clientRaw->config_remote_autochan_nch = static_cast<int>(_numOutputChannels);
		_ConfigureLocalChannels();
	}
}

void NinjamConnection::ProcessAudioBlock(const float* interleavedInput,
	unsigned int numFrames,
	unsigned int sampleRate)
{
	if (!_isConnected || !_clientRaw || numFrames == 0u)
		return;

	if (sampleRate > 0)
		_sampleRate = sampleRate;

	std::scoped_lock audioLock(_audioBufferMutex);

	// Scratch buffers are pre-allocated by SetAudioFormat.
	// If the buffers aren't ready or numFrames exceeds the pre-allocated size,
	// skip processing — callers must invoke SetAudioFormat before the audio stream starts.
	if (_outScratch.size() < _numOutputChannels
		|| (!_outScratch.empty() && numFrames > _outScratch[0].size())
		|| (_numInputChannels > 0
			&& (_inScratch.size() < _numInputChannels
				|| (!_inScratch.empty() && numFrames > _inScratch[0].size()))))
		return;

	for (auto chan = 0u; chan < _numOutputChannels; chan++)
		std::fill(_outScratch[chan].begin(), _outScratch[chan].begin() + numFrames, 0.0f);

	if (interleavedInput)
	{
		for (auto samp = 0u; samp < numFrames; samp++)
		{
			for (auto chan = 0u; chan < _numInputChannels; chan++)
				_inScratch[chan][samp] = interleavedInput[samp * _numInputChannels + chan];
		}
	}
	else
	{
		for (auto chan = 0u; chan < _numInputChannels; chan++)
			std::fill(_inScratch[chan].begin(), _inScratch[chan].begin() + numFrames, 0.0f);
	}

	_clientRaw->AudioProc(
		_numInputChannels > 0 ? _inPtrs.data() : nullptr,
		static_cast<int>(_numInputChannels),
		_numOutputChannels > 0 ? _outPtrs.data() : nullptr,
		static_cast<int>(_numOutputChannels),
		static_cast<int>(numFrames),
		static_cast<int>(_sampleRate));

	_lastNumFrames = numFrames;
}

NinjamRemoteSnapshot NinjamConnection::Snapshot() const
{
	std::scoped_lock lock(_snapshotMutex);
	return _snapshot;
}

bool NinjamConnection::ConsumeStereoPair(unsigned int outChannelLeft,
	const float*& left,
	const float*& right,
	unsigned int& numFrames) const
{
	left = nullptr;
	right = nullptr;
	numFrames = 0;

	if (!_isConnected || _lastNumFrames == 0u)
		return false;

	auto rightIndex = outChannelLeft + 1u;
	if ((outChannelLeft >= _numOutputChannels) || (rightIndex >= _numOutputChannels))
		return false;

	std::scoped_lock audioLock(_audioBufferMutex);
	left = _outScratch[outChannelLeft].data();
	right = _outScratch[rightIndex].data();
	numFrames = _lastNumFrames;
	return true;
}

void NinjamConnection::_EnsureWorkDir()
{
	if (_workDir.empty())
	{
		try
		{
			auto path = std::filesystem::temp_directory_path() / "Jamma" / "Ninjam";
			_workDir = path.string();
		}
		catch (...)
		{
			_workDir = ".\\ninjam-work";
		}
	}

	try
	{
		std::filesystem::create_directories(_workDir);
	}
	catch (...)
	{
	}
}

void NinjamConnection::_EnsureScratchBuffers(unsigned int numFrames)
{
	std::scoped_lock audioLock(_audioBufferMutex);

	if ((_outScratch.size() != _numOutputChannels) || (_inScratch.size() != _numInputChannels))
	{
		_outScratch.assign(_numOutputChannels, std::vector<float>(numFrames, 0.0f));
		_inScratch.assign(_numInputChannels, std::vector<float>(numFrames, 0.0f));
	}

	for (auto& channel : _outScratch)
	{
		if (channel.size() < numFrames)
			channel.resize(numFrames, 0.0f);
	}

	for (auto& channel : _inScratch)
	{
		if (channel.size() < numFrames)
			channel.resize(numFrames, 0.0f);
	}

	_outPtrs.resize(_numOutputChannels, nullptr);
	for (auto chan = 0u; chan < _numOutputChannels; chan++)
		_outPtrs[chan] = _outScratch[chan].data();

	_inPtrs.resize(_numInputChannels, nullptr);
	for (auto chan = 0u; chan < _numInputChannels; chan++)
		_inPtrs[chan] = _inScratch[chan].data();
}

void NinjamConnection::_ConfigureLocalChannels()
{
	if (!_isConnected || !_clientRaw)
		return;

	const auto maxLocalChannels = std::max(0, _clientRaw->GetMaxLocalChannels());
	const auto configuredChannels = std::min(static_cast<unsigned int>(maxLocalChannels), _numInputChannels);

	for (auto chan = 0u; chan < configuredChannels; chan++)
	{
		auto name = std::string("Jamma In ") + std::to_string(chan + 1u);
		_clientRaw->SetLocalChannelInfo(
			static_cast<int>(chan),
			name.c_str(),
			true,
			static_cast<int>(chan),
			true,
			96,
			true,
			true);
	}

	_clientRaw->NotifyServerOfChannelChange();
}

void NinjamConnection::_RefreshSnapshot()
{
	if (!_isConnected || !_clientRaw)
		return;

	NinjamRemoteSnapshot snapshot;

	int intervalPos = 0;
	int intervalLength = 0;
	_clientRaw->GetPosition(&intervalPos, &intervalLength);
	snapshot.IntervalPositionSamps = intervalPos > 0 ? static_cast<unsigned int>(intervalPos) : 0u;
	snapshot.IntervalLengthSamps = intervalLength > 0 ? static_cast<unsigned int>(intervalLength) : 0u;

	std::set<std::string> activeUsers;
	const auto userCount = _clientRaw->GetNumUsers();
	for (auto userIndex = 0; userIndex < userCount; userIndex++)
	{
		const auto* userNameC = _clientRaw->GetUserState(userIndex);
		if (!userNameC || (*userNameC == '\0'))
			continue;

		NinjamRemoteUser user;
		user.UserName = userNameC;
		activeUsers.insert(user.UserName);
		user.AssignedOutputChannel = _AssignOutputChannel(user.UserName);

		for (auto ordinal = 0;; ordinal++)
		{
			auto channelIndex = _clientRaw->EnumUserChannels(userIndex, ordinal);
			if (channelIndex < 0)
				break;

			bool subscribed = false;
			_clientRaw->GetUserChannelState(userIndex, channelIndex, &subscribed);

			_clientRaw->SetUserChannelState(
				userIndex,
				channelIndex,
				true,
				true,
				false,
				0.0f,
				false,
				0.0f,
				false,
				false,
				false,
				false,
				true,
				static_cast<int>(user.AssignedOutputChannel));

			bool subState = false;
			float vol = 0.0f;
			float pan = 0.0f;
			bool mute = false;
			bool solo = false;
			int outChannel = 0;
			int flags = 0;
			const auto* channelNameC = _clientRaw->GetUserChannelState(
				userIndex,
				channelIndex,
				&subState,
				&vol,
				&pan,
				&mute,
				&solo,
				&outChannel,
				&flags);

			NinjamRemoteChannel channel;
			channel.ChannelIndex = channelIndex;
			channel.Name = channelNameC ? channelNameC : "";
			channel.Subscribed = subState;
			channel.PeakLeft = _clientRaw->GetUserChannelPeak(userIndex, channelIndex, 0);
			channel.PeakRight = _clientRaw->GetUserChannelPeak(userIndex, channelIndex, 1);
			user.Channels.push_back(channel);
		}

		user.ChannelCount = static_cast<unsigned int>(user.Channels.size());
		snapshot.Users.push_back(std::move(user));
	}

	for (auto it = _userOutputChannels.begin(); it != _userOutputChannels.end();)
	{
		if (activeUsers.find(it->first) == activeUsers.end())
			it = _userOutputChannels.erase(it);
		else
			++it;
	}

	std::sort(snapshot.Users.begin(), snapshot.Users.end(), [](const NinjamRemoteUser& a, const NinjamRemoteUser& b) {
		return a.UserName < b.UserName;
		});

	std::scoped_lock lock(_snapshotMutex);
	_snapshot = std::move(snapshot);
}

unsigned int NinjamConnection::_AssignOutputChannel(const std::string& userName)
{
	auto found = _userOutputChannels.find(userName);
	if (found != _userOutputChannels.end())
		return found->second;

	std::set<unsigned int> usedChannels;
	for (const auto& pair : _userOutputChannels)
		usedChannels.insert(pair.second);

	for (auto outChannel = 0u; (outChannel + 1u) < _numOutputChannels; outChannel += 2u)
	{
		if (usedChannels.find(outChannel) == usedChannels.end())
		{
			_userOutputChannels[userName] = outChannel;
			return outChannel;
		}
	}

	_userOutputChannels[userName] = 0u;
	return 0u;
}
