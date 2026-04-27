#include "NinjamConnection.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <set>

namespace
{
	bool IsAuthFailure(const std::string& err)
	{
		return err.find("invalid login/password") != std::string::npos
			|| err.find("invalid credentials") != std::string::npos
			|| err.find("authentication") != std::string::npos;
	}
}

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
	_autoReconnect(false),
	_connectAttempts(0u),
	_nextRetryAt(std::chrono::steady_clock::now()),
	_connectStartedAt(),
	_retryDelayMin(std::chrono::milliseconds(1500)),
	_retryDelay(std::chrono::milliseconds(1500)),
	_retryDelayMax(std::chrono::milliseconds(30000)),
	_connectTimeout(std::chrono::seconds(20)),
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

bool NinjamConnection::_BeginConnectAttempt(std::chrono::steady_clock::time_point now)
{
	if (!_clientRaw)
	{
		_lastError = "NJClient unavailable";
		_state = ConnectionState::Failed;
		return false;
	}

	if (_host.empty() || _user.empty())
	{
		_lastError = "Host/user not configured";
		_state = ConnectionState::Failed;
		return false;
	}

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

	_lastError.clear();
	_connectAttempts += 1u;
	_connectStartedAt = now;
	_state = ConnectionState::Connecting;
	const auto connectUser = (_pass.empty() && !_user.starts_with("anonymous:"))
		? std::string("anonymous:") + _user
		: _user;
	std::cout << "[NINJAM] Connect attempt " << _connectAttempts
		<< " to " << _host << " as " << connectUser << std::endl;

	_clientRaw->Connect(_host.c_str(), connectUser.c_str(), _pass.c_str());
	_isConnected = false;
	return true;
}

void NinjamConnection::_ResetReconnectState(std::chrono::steady_clock::time_point now)
{
	_connectAttempts = 0u;
	_connectStartedAt = {};
	_retryDelay = _retryDelayMin;
	_nextRetryAt = now;
}

void NinjamConnection::_ScheduleRetry(std::chrono::steady_clock::time_point now)
{
	const auto retryDelay = _retryDelay;
	_nextRetryAt = now + retryDelay;
	auto nextDelayMs = std::min(_retryDelay.count() * 2, _retryDelayMax.count());
	_retryDelay = std::chrono::milliseconds(nextDelayMs);
	_state = ConnectionState::Retrying;
	_connectStartedAt = {};

	std::cout << "[NINJAM] Retrying in " << retryDelay.count() << " ms" << std::endl;
}

std::string NinjamConnection::_DescribeStatusError(int status) const
{
	auto err = std::string(_clientRaw && _clientRaw->GetErrorStr() ? _clientRaw->GetErrorStr() : "");
	if (!err.empty())
		return err;

	switch (status)
	{
	case NJClient::NJC_STATUS_INVALIDAUTH:
		return "Invalid credentials";
	case NJClient::NJC_STATUS_CANTCONNECT:
		return "Could not reach server";
	case NJClient::NJC_STATUS_DISCONNECTED:
		return "Disconnected";
	default:
		return "NINJAM connection error";
	}
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

	if (_state == ConnectionState::Connecting || _state == ConnectionState::Retrying)
		return true;

	if (_isConnected)
		return true;

	_autoReconnect = true;
	_ResetReconnectState(std::chrono::steady_clock::now());
	return _BeginConnectAttempt(std::chrono::steady_clock::now());
}

void NinjamConnection::Disconnect()
{
	std::scoped_lock lock(_connectionMutex);
	_autoReconnect = false;

	if (_clientRaw)
		_clientRaw->Disconnect();

	if (_isConnected)
		std::cout << "[NINJAM] Disconnected" << std::endl;

	_isConnected = false;
	_state = ConnectionState::Disconnected;
	_ResetReconnectState(std::chrono::steady_clock::now());
	_userOutputChannels.clear();
	_lastLoggedUserNames.clear();
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
	if (!_clientRaw)
		return;

	auto now = std::chrono::steady_clock::now();
	{
		std::scoped_lock lock(_connectionMutex);
		const auto hasActiveAttempt = _connectStartedAt.time_since_epoch().count() != 0;

		if (_autoReconnect)
		{
			if ((_state == ConnectionState::Connecting)
				&& hasActiveAttempt
				&& ((now - _connectStartedAt) >= _connectTimeout))
			{
				std::cout << "[NINJAM] Connect attempt timed out after "
					<< std::chrono::duration_cast<std::chrono::seconds>(_connectTimeout).count()
					<< "s, allowing DNS resolution to finish before retry" << std::endl;
				_lastError = "Connection timed out";
				_state = ConnectionState::Retrying;
				_nextRetryAt = now + _connectTimeout;
			}

			if ((_state == ConnectionState::Retrying)
				&& hasActiveAttempt
				&& (now >= _nextRetryAt))
			{
				std::cout << "[NINJAM] Restarting stalled connect attempt" << std::endl;
				_clientRaw->Disconnect();
				_isConnected = false;
				_ScheduleRetry(now);
				return;
			}

			if (((_state == ConnectionState::Disconnected)
					|| (_state == ConnectionState::Failed)
					|| ((_state == ConnectionState::Retrying) && !hasActiveAttempt))
				&& (now >= _nextRetryAt))
			{
				if (!_BeginConnectAttempt(now))
				{
					std::cout << "[NINJAM] Connection failed: " << _lastError << std::endl;
				}
			}
		}
		else if ((_state == ConnectionState::Disconnected)
			|| (_state == ConnectionState::Failed)
			|| (_state == ConnectionState::Retrying))
		{
			return;
		}
	}

	while (!_clientRaw->Run()) {}

	const auto status = _clientRaw->GetStatus();
	if (status == NJClient::NJC_STATUS_OK)
	{
		if (!_isConnected)
		{
			_isConnected = true;
			_state = ConnectionState::Connected;
			std::scoped_lock lock(_connectionMutex);
			_ResetReconnectState(now);
			_ConfigureLocalChannels();
			std::cout << "[NINJAM] Connected" << std::endl;
		}
	}
	else if (status == NJClient::NJC_STATUS_PRECONNECT)
	{
		if (_state != ConnectionState::Retrying)
			_state = ConnectionState::Connecting;
	}
	else
	{
		if (_isConnected || (_state == ConnectionState::Connecting) || (_state == ConnectionState::Retrying))
		{
			_lastError = _DescribeStatusError(status);
			std::cout << "[NINJAM] Connection failed: " << _lastError << std::endl;
		}

		_isConnected = false;
		const auto isAuthFailure = status == NJClient::NJC_STATUS_INVALIDAUTH || IsAuthFailure(_lastError);
		if (isAuthFailure)
		{
			_autoReconnect = false;
			_state = ConnectionState::Failed;
		}
		else if (_autoReconnect)
		{
			std::scoped_lock lock(_connectionMutex);
			_ScheduleRetry(now);
		}
		else
		{
			_state = ConnectionState::Failed;
		}
	}

	if (!_isConnected)
		return;

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
	std::vector<std::string> currentUserNames;
	currentUserNames.reserve(static_cast<size_t>(userCount));
	for (auto userIndex = 0; userIndex < userCount; userIndex++)
	{
		const auto* userNameC = _clientRaw->GetUserState(userIndex);
		const auto userName = std::string(userNameC ? userNameC : "");
		currentUserNames.push_back(userName);

		if (!userNameC || (*userNameC == '\0'))
			continue;

		NinjamRemoteUser user;
		user.UserName = userName;
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

	if (currentUserNames != _lastLoggedUserNames)
	{
		std::cout << "[NINJAM] Connected users (" << currentUserNames.size() << ")";

		if (!currentUserNames.empty())
		{
			std::cout << ": ";
			for (size_t i = 0; i < currentUserNames.size(); i++)
			{
				if (i > 0)
					std::cout << ", ";
				std::cout << currentUserNames[i];
			}
		}
		std::cout << std::endl;

		_lastLoggedUserNames = std::move(currentUserNames);
	}

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
