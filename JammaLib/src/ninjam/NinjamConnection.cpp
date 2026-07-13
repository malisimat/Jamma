
#include "NinjamConnection.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <limits>
#include <set>
#include "njclient.h"
#include "../../include/Constants.h"

namespace
{
	constexpr unsigned int kMinimumNinjamOutputChannels = 4u;
	constexpr unsigned int kReservedMonitorOutputChannels = 2u;

	bool IsAuthFailure(const std::string& err)
	{
		return err.find("invalid login/password") != std::string::npos
			|| err.find("invalid credentials") != std::string::npos
			|| err.find("authentication") != std::string::npos;
	}

	bool EqualsIgnoreCase(const std::string& lhs, const std::string& rhs)
	{
		if (lhs.size() != rhs.size())
			return false;

		for (size_t i = 0; i < lhs.size(); i++)
		{
			if (std::tolower(static_cast<unsigned char>(lhs[i])) !=
				std::tolower(static_cast<unsigned char>(rhs[i])))
				return false;
		}

		return true;
	}

	std::string DescribeStatusError(NJClient* client, int status)
	{
		auto err = std::string(client && client->GetErrorStr() ? client->GetErrorStr() : "");
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
}

using namespace ninjam;

NinjamConnection::NinjamConnection(std::string host,
	std::string user,
	std::string pass,
	std::string workDir) :
	_host(std::move(host)),
	_user(std::move(user)),
	_pass(std::move(pass)),
	_workDir(std::move(workDir)),
	_sampleRate(constants::DefaultSampleRate),
	_blockSize(constants::DefaultBufferSizeSamps),
	_lanePacking(NinjamLanePacking{}),
	_client(std::make_unique<NJClient>())
{
}

NinjamLanePacking NinjamConnection::_ResolveLanePacking() const
{
	NinjamLanePacking packing{};

	auto slotLimit = static_cast<unsigned int>(DefaultLocalChannelSlotLimit);
	auto fromFallback = true;

	if (_isConnected && _client)
	{
		const auto runtimeLimit = _client->GetMaxLocalChannels();
		if (runtimeLimit > 0)
		{
			slotLimit = static_cast<unsigned int>(runtimeLimit);
			fromFallback = false;
		}
	}

	const auto dacPairs = _numOutputChannels / 2u;
	const auto adcPairs = _numInputChannels / 2u;
	const auto totalPairs = dacPairs + adcPairs;

	packing.DacPairs = static_cast<std::uint16_t>(std::min(dacPairs,
		static_cast<unsigned int>(std::numeric_limits<std::uint16_t>::max())));
	packing.AdcPairs = static_cast<std::uint16_t>(std::min(adcPairs,
		static_cast<unsigned int>(std::numeric_limits<std::uint16_t>::max())));
	packing.FromFallback = fromFallback ? 1u : 0u;
	packing.SlotLimit = static_cast<std::uint16_t>(std::min(slotLimit,
		static_cast<unsigned int>(std::numeric_limits<std::uint16_t>::max())));

	if (slotLimit == 0u)
	{
		packing.LaneCount = 0u;
		packing.Modulo = 0u;
		return packing;
	}

	if (totalPairs <= slotLimit)
	{
		packing.LaneCount = static_cast<std::uint16_t>(std::min(totalPairs,
			static_cast<unsigned int>(std::numeric_limits<std::uint16_t>::max())));
		packing.Modulo = 0u;
		return packing;
	}

	packing.LaneCount = static_cast<std::uint16_t>(std::min(slotLimit,
		static_cast<unsigned int>(std::numeric_limits<std::uint16_t>::max())));
	packing.Modulo = 1u;
	return packing;
}

unsigned int NinjamConnection::_InputScratchChannelCapacity() const noexcept
{
	const auto dacPairs = _numOutputChannels / 2u;
	const auto adcPairs = _numInputChannels / 2u;
	return (dacPairs + adcPairs) * 2u;
}

void NinjamConnection::_RefreshLanePacking()
{
	const auto packing = _ResolveLanePacking();
	const auto previous = _lanePacking.load(std::memory_order_acquire);

	const auto changed = (previous.LaneCount != packing.LaneCount)
		|| (previous.DacPairs != packing.DacPairs)
		|| (previous.AdcPairs != packing.AdcPairs)
		|| (previous.Modulo != packing.Modulo)
		|| (previous.FromFallback != packing.FromFallback)
		|| (previous.SlotLimit != packing.SlotLimit);

	if (!changed)
		return;

	_lanePacking.store(packing, std::memory_order_release);

	std::cout << "[NINJAM] Local lane packing: lanes=" << packing.LaneCount
		<< " mode=" << (packing.Modulo ? "modulo" : "direct")
		<< " dacPairs=" << packing.DacPairs
		<< " adcPairs=" << packing.AdcPairs
		<< " slotLimit=" << packing.SlotLimit
		<< " source=" << (packing.FromFallback ? "fallback" : "runtime")
		<< std::endl;

	if (packing.LaneCount == 0u)
	{
		std::cout << "[NINJAM] Local lane budget is 0; publishing no local channels" << std::endl;
	}
	else if (packing.Modulo)
	{
		std::cout << "[NINJAM] Modulo lane packing active because DAC+ADC pairs exceed slot budget" << std::endl;
	}

	_ApplyLocalChannels();
}

bool NinjamConnection::_StartConnectAttempt(std::chrono::steady_clock::time_point now)
{
	if (!_client)
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

	_client->LicenseAgreementCallback = [](void*, const char*) { return 1; };
	_client->config_savelocalaudio = -1;
	_client->config_remote_autochan = 0;
	_client->config_remote_autochan_nch = static_cast<int>(_numOutputChannels);

	if (!_workDir.empty())
	{
		std::vector<char> mutablePath(_workDir.begin(), _workDir.end());
		mutablePath.push_back('\0');
		_client->SetWorkDir(mutablePath.data());
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

	_client->ChatMessage_Callback = &NinjamConnection::_OnChatMessage;
	_client->ChatMessage_User = this;

	_client->Connect(_host.c_str(), connectUser.c_str(), _pass.c_str());
	_isConnected = false;
	return true;
}

bool NinjamConnection::_HasActiveConnectAttempt() const noexcept
{
	return _connectStartedAt.time_since_epoch().count() != 0;
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

NinjamConnection::~NinjamConnection() { Disconnect(); }

bool NinjamConnection::Connect()
{
	std::scoped_lock lock(_connectionMutex);

	if (_state == ConnectionState::Connecting || _state == ConnectionState::Retrying)
		return true;

	if (_isConnected)
		return true;

	_autoReconnect = true;
	_ResetReconnectState(std::chrono::steady_clock::now());
	return _StartConnectAttempt(std::chrono::steady_clock::now());
}

void NinjamConnection::Disconnect()
{
	std::scoped_lock lock(_connectionMutex);
	_autoReconnect = false;

	if (_client)
		_client->Disconnect();

	if (_isConnected)
		std::cout << "[NINJAM] Disconnected" << std::endl;

	_isConnected = false;
	_state = ConnectionState::Disconnected;
	_ResetReconnectState(std::chrono::steady_clock::now());
	_userOutputChannels.clear();
	_lastLoggedUsers.clear();
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
	if (!_client)
		return;

	auto now = std::chrono::steady_clock::now();
	{
		std::scoped_lock lock(_connectionMutex);
		const auto hasActiveAttempt = _HasActiveConnectAttempt();

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
				_client->Disconnect();
				_isConnected = false;
				_ScheduleRetry(now);
				return;
			}

			if (((_state == ConnectionState::Disconnected)
					|| (_state == ConnectionState::Failed)
					|| ((_state == ConnectionState::Retrying) && !hasActiveAttempt))
				&& (now >= _nextRetryAt))
			{
				if (!_StartConnectAttempt(now))
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

	while (!_client->Run()) {}

	const auto status = _client->GetStatus();
	if (status == NJClient::NJC_STATUS_OK)
	{
		if (!_isConnected)
		{
			_isConnected = true;
			_state = ConnectionState::Connected;
			std::scoped_lock lock(_connectionMutex);
			_ResetReconnectState(now);
			std::cout << "[NINJAM] Connected" << std::endl;
		}

		_RefreshLanePacking();
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
			_lastError = DescribeStatusError(_client.get(), status);
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

	_UpdateSnapshot();
}

void NinjamConnection::SetAudioFormat(unsigned int sampleRate,
	unsigned int blockSize,
	unsigned int numInputChannels,
	unsigned int numOutputChannels,
	unsigned int inLatencySamps,
	unsigned int outLatencySamps)
{
	_sampleRate = sampleRate > 0 ? sampleRate : constants::DefaultSampleRate;
	_blockSize = blockSize > 0 ? blockSize : constants::DefaultBufferSizeSamps;
	_numInputChannels = numInputChannels;
	_numDacPhysicalChannels = numOutputChannels;
	_numOutputChannels = std::max(
		numOutputChannels > 0 ? numOutputChannels : 2u,
		kMinimumNinjamOutputChannels);
	_inLatencySamps = inLatencySamps;
	_outLatencySamps = outLatencySamps;
	_ResizeScratchBuffers(_blockSize, _InputScratchChannelCapacity());
	_ResizeExportDelayLines();

	_RefreshLanePacking();

	if (_client)
	{
		_client->config_remote_autochan_nch = static_cast<int>(_numOutputChannels);
	}
}

void NinjamConnection::_ResizeExportDelayLines()
{
	// Sized once here (never in the audio callback) so ProcessExportBlock stays
	// allocation-free -- same guardrail as _ResizeScratchBuffers (see
	// doc/build-notes and doc/ninjam-live-loop-latency-sync-planC.md §5/step 1).
	const auto delayLineSize = constants::MaxNinjamIntervalSamps + constants::MaxBlockSize;
	const auto channelsChanged = (_dacDelayLines.size() != _numDacPhysicalChannels)
		|| (_adcDelayLines.size() != _numInputChannels);

	if (channelsChanged)
	{
		_dacDelayLines.clear();
		_dacDelayLines.reserve(_numDacPhysicalChannels);
		for (auto chan = 0u; chan < _numDacPhysicalChannels; chan++)
			_dacDelayLines.push_back(std::make_shared<audio::AudioBuffer>(delayLineSize));

		_adcDelayLines.clear();
		_adcDelayLines.reserve(_numInputChannels);
		for (auto chan = 0u; chan < _numInputChannels; chan++)
			_adcDelayLines.push_back(std::make_shared<audio::AudioBuffer>(delayLineSize));

		_exportTimingState = {};

		// _exportTick ("n" in the K_dac/K_adc formula) must stay in lockstep
		// with the delay lines' own internal write cursors -- only reset it
		// here, paired with brand-new (writeIndex==0) buffers. If the
		// channel count is unchanged, the existing buffers' write cursors are
		// untouched, so _exportTick must be left alone too (see
		// doc/ninjam-live-loop-latency-sync-planC.md §3.1).
		_exportTick = 0u;
	}

	_delayedDacInterleaved.assign(static_cast<size_t>(_blockSize) * _numDacPhysicalChannels, 0.0f);
	_delayedAdcInterleaved.assign(static_cast<size_t>(_blockSize) * _numInputChannels, 0.0f);
	_dacDelayTemp.assign(_blockSize, 0.0f);
	_adcDelayTemp.assign(_blockSize, 0.0f);
	_exportSilenceScratch.assign(_blockSize, 0.0f);
}

void NinjamConnection::_WriteExportDelayLine(std::vector<std::shared_ptr<audio::AudioBuffer>>& lines,
	const float* interleaved,
	unsigned int numChannels,
	unsigned int numFrames,
	std::vector<float>& silenceScratch)
{
	if (lines.size() != numChannels || numFrames == 0u)
		return;

	if (!interleaved && numFrames > silenceScratch.size())
		return;

	if (!interleaved)
		std::fill(silenceScratch.begin(), silenceScratch.begin() + numFrames, 0.0f);

	for (auto chan = 0u; chan < numChannels; chan++)
	{
		base::AudioWriteRequest request;
		request.numSamps = numFrames;
		request.fadeCurrent = 0.0f;
		request.fadeNew = 1.0f;

		if (interleaved)
		{
			request.samples = &interleaved[chan];
			request.stride = numChannels;
		}
		else
		{
			request.samples = silenceScratch.data();
			request.stride = 1;
		}

		lines[chan]->OnBlockWrite(request, 0);
		lines[chan]->EndWrite(numFrames, true);
	}
}

void NinjamConnection::_ReadExportDelayLine(std::vector<std::shared_ptr<audio::AudioBuffer>>& lines,
	unsigned int delaySamps,
	unsigned int numFrames,
	std::vector<float>& delayedInterleaved,
	std::vector<float>& tempBuf)
{
	const auto numChannels = static_cast<unsigned int>(lines.size());
	if (numChannels == 0u || numFrames == 0u || numFrames > tempBuf.size()
		|| (static_cast<size_t>(numFrames) * numChannels) > delayedInterleaved.size())
		return;

	std::fill(delayedInterleaved.begin(), delayedInterleaved.begin() + (static_cast<size_t>(numFrames) * numChannels), 0.0f);

	for (auto chan = 0u; chan < numChannels; chan++)
	{
		auto& buf = lines[chan];

		// Still priming after a generation reset: not enough history yet for
		// this block's K, so leave this channel silent rather than read
		// stale/undefined content (planC §3.1).
		if (buf->SampsRecorded() < delaySamps)
			continue;

		buf->Delay(delaySamps);
		const auto* data = buf->PlaybackRead(tempBuf.data(), numFrames);

		for (auto samp = 0u; samp < numFrames; samp++)
			delayedInterleaved[static_cast<size_t>(samp) * numChannels + chan] = data[samp];
	}
}

unsigned int NinjamConnection::ExportAnomalyCount() const noexcept
{
	return _exportAnomalyCount.load(std::memory_order_relaxed);
}

void NinjamConnection::ProcessExportBlock(const float* interleavedDacOutput,
	unsigned int numDacChannels,
	const float* interleavedAdcInput,
	unsigned int numAdcChannels,
	unsigned int numFrames,
	unsigned int sampleRate)
{
	if (!_isConnected || !_client || numFrames == 0u)
		return;

	if (sampleRate > 0)
		_sampleRate = sampleRate;

	// Scratch buffers are pre-allocated by SetAudioFormat.
	// If the buffers aren't ready or numFrames exceeds the pre-allocated size,
	// skip processing — callers must invoke SetAudioFormat before the audio stream starts.
	if (_outScratch.size() < _numOutputChannels
		|| (!_outScratch.empty() && numFrames > _outScratch[0].size())
		|| (!_inScratch.empty() && numFrames > _inScratch[0].size()))
		return;

	for (auto chan = 0u; chan < _numOutputChannels; chan++)
		std::fill(_outScratch[chan].begin(), _outScratch[chan].begin() + numFrames, 0.0f);

	const float* dacForPacking = interleavedDacOutput;
	const float* adcForPacking = interleavedAdcInput;

	const auto useCompensation = ExportLatencyCompensationEnabled
		&& (_dacDelayLines.size() == numDacChannels)
		&& (_adcDelayLines.size() == numAdcChannels)
		&& (numFrames <= _dacDelayTemp.size())
		&& (numFrames <= _adcDelayTemp.size());

	if (useCompensation)
	{
		// Write this block's physical DAC/ADC content into the delay lines
		// first, then read NJClient's live interval position as late as
		// possible (immediately before AudioProc) so K is as fresh as
		// possible — see doc/ninjam-live-loop-latency-sync-planC.md §1/§3.
		_WriteExportDelayLine(_dacDelayLines, interleavedDacOutput, numDacChannels, numFrames, _exportSilenceScratch);
		_WriteExportDelayLine(_adcDelayLines, interleavedAdcInput, numAdcChannels, numFrames, _exportSilenceScratch);

		int posInt = 0;
		int lengthInt = 0;
		_client->GetPosition(&posInt, &lengthInt);

		ExportLaneTimingInput timingInput;
		timingInput.n = _exportTick;
		timingInput.numFrames = numFrames;
		timingInput.pos = posInt > 0 ? static_cast<unsigned int>(posInt) : 0u;
		timingInput.length = lengthInt > 0 ? static_cast<unsigned int>(lengthInt) : 0u;
		// TODO(latency): inLatencySamps/outLatencySamps are hardware latency
		// only. Loop-driven VST latency (Loop::CurrentVstLatencySamps()) is
		// not yet folded into outLatencySamps here -- see
		// doc/ninjam-live-loop-latency-sync-planC.md §2/§7.
		timingInput.inLatencySamps = _inLatencySamps;
		timingInput.outLatencySamps = _outLatencySamps;

		const auto timing = ExportLaneTiming::Compute(timingInput, _exportTimingState);
		_exportTick += numFrames;

		if (timing.generationReset)
		{
			for (auto& buf : _dacDelayLines)
				buf->Reset();
			for (auto& buf : _adcDelayLines)
				buf->Reset();

			if (timing.anomalyDetected)
				_exportAnomalyCount.fetch_add(1u, std::memory_order_relaxed);
		}

		if (timing.valid)
		{
			_ReadExportDelayLine(_dacDelayLines, timing.dacDelaySamps, numFrames, _delayedDacInterleaved, _dacDelayTemp);
			_ReadExportDelayLine(_adcDelayLines, timing.adcDelaySamps, numFrames, _delayedAdcInterleaved, _adcDelayTemp);
			dacForPacking = _delayedDacInterleaved.data();
			adcForPacking = _delayedAdcInterleaved.data();
		}
		else
		{
			// No NJClient timing yet — mute rather than send uncompensated
			// (and therefore out-of-phase) content.
			dacForPacking = nullptr;
			adcForPacking = nullptr;
		}
	}

	const auto packing = _lanePacking.load(std::memory_order_acquire);
	const auto laneCount = static_cast<unsigned int>(packing.LaneCount);
	const auto laneChannelCount = laneCount * 2u;
	int localInputChannelCount = 0;

	if ((laneChannelCount > 0u)
		&& (_inScratch.size() >= laneChannelCount)
		&& !_inScratch.empty())
	{
		for (auto chan = 0u; chan < laneChannelCount; chan++)
			std::fill(_inScratch[chan].begin(), _inScratch[chan].begin() + numFrames, 0.0f);

		if (dacForPacking && (numDacChannels >= 2u))
		{
			const auto dacPairs = std::min(static_cast<unsigned int>(packing.DacPairs), numDacChannels / 2u);
			for (auto pair = 0u; pair < dacPairs; pair++)
			{
				auto lane = packing.Modulo ? (pair % laneCount) : pair;
				if (lane >= laneCount)
					continue;

				const auto srcLeft = pair * 2u;
				const auto srcRight = srcLeft + 1u;
				const auto dstLeft = lane * 2u;
				const auto dstRight = dstLeft + 1u;

				for (auto samp = 0u; samp < numFrames; samp++)
				{
					auto srcBase = samp * numDacChannels;
					_inScratch[dstLeft][samp] += dacForPacking[srcBase + srcLeft];
					_inScratch[dstRight][samp] += dacForPacking[srcBase + srcRight];
				}
			}
		}

		if (adcForPacking && (numAdcChannels >= 2u))
		{
			const auto adcPairs = std::min(static_cast<unsigned int>(packing.AdcPairs), numAdcChannels / 2u);
			for (auto pair = 0u; pair < adcPairs; pair++)
			{
				auto lane = packing.Modulo ? (pair % laneCount) : (static_cast<unsigned int>(packing.DacPairs) + pair);
				if (lane >= laneCount)
					continue;

				const auto srcLeft = pair * 2u;
				const auto srcRight = srcLeft + 1u;
				const auto dstLeft = lane * 2u;
				const auto dstRight = dstLeft + 1u;

				for (auto samp = 0u; samp < numFrames; samp++)
				{
					auto srcBase = samp * numAdcChannels;
					_inScratch[dstLeft][samp] += adcForPacking[srcBase + srcLeft];
					_inScratch[dstRight][samp] += adcForPacking[srcBase + srcRight];
				}
			}
		}

		for (auto chan = 0u; chan < laneChannelCount; chan++)
			_inPtrs[chan] = _inScratch[chan].data();

		localInputChannelCount = static_cast<int>(laneChannelCount);
	}

	_client->AudioProc(
		localInputChannelCount > 0 ? _inPtrs.data() : nullptr,
		localInputChannelCount,
		_numOutputChannels > 0 ? _outPtrs.data() : nullptr,
		static_cast<int>(_numOutputChannels),
		static_cast<int>(numFrames),
		static_cast<int>(_sampleRate));

	_lastNumFrames = numFrames;
}

NinjamLiveTiming NinjamConnection::GetLiveTiming() const noexcept
{
	NinjamLiveTiming timing;
	if (!_isConnected || !_client)
		return timing;

	int intervalPosition = 0;
	int intervalLength = 0;
	_client->GetPosition(&intervalPosition, &intervalLength);

	timing.intervalPositionSamps = intervalPosition >= 0 ? static_cast<unsigned int>(intervalPosition) : 0u;
	timing.intervalLengthSamps = intervalLength > 0 ? static_cast<unsigned int>(intervalLength) : 0u;
	timing.sampleRate = static_cast<unsigned int>(std::max(0, _client->GetSampleRate()));
	timing.bpm = _client->GetActualBPM();
	timing.bpi = static_cast<unsigned int>(std::max(0, _client->GetBPI()));
	timing.valid = timing.intervalLengthSamps > 0u && timing.sampleRate > 0u
		&& timing.bpm > 0.0f && timing.bpi > 0u;
	return timing;
}

NinjamRemoteSnapshot NinjamConnection::Snapshot() const
{
	std::scoped_lock lock(_snapshotMutex);
	return _snapshot;
}

bool NinjamConnection::RequestServerTempo(float bpm, int bpi)
{
	if (bpm <= 0.0f || bpi <= 0)
		return false;

	const auto bpmVal = std::to_string(static_cast<int>(bpm + 0.5f));
	const auto bpiVal = std::to_string(bpi);

	// Admin form: honoured immediately if the connected user has admin privileges.
	auto adminBpmCmd = std::string("/bpm ") + bpmVal;
	auto adminBpiCmd = std::string("/bpi ") + bpiVal;

	// Vote form: non-admin path; the server counts votes across all participants
	// and applies the change once a majority (>50%) agrees.
	auto voteBpmCmd = std::string("!vote bpm ") + bpmVal;
	auto voteBpiCmd = std::string("!vote bpi ") + bpiVal;

	{
		// Guard NJClient usage against concurrent Disconnect/Pump calls.
		std::scoped_lock lock(_connectionMutex);

		if (!_isConnected || !_client)
			return false;

		// Send both forms: admin command applies immediately for privileged users;
		// vote command is the automatic fallback for non-admins.  Server vote-progress
		// and confirmation announcements arrive as server MSG messages, which
		// _OnChatMessage prints to the console as "[NINJAM] ** <text>".
		_client->ChatMessage_Send("MSG", adminBpmCmd.c_str());
		_client->ChatMessage_Send("MSG", adminBpiCmd.c_str());
		_client->ChatMessage_Send("MSG", voteBpmCmd.c_str());
		_client->ChatMessage_Send("MSG", voteBpiCmd.c_str());
	}

	std::cout << "[NINJAM] Requested server tempo bpm=" << bpmVal
		<< " bpi=" << bpiVal
		<< " (admin + vote)" << std::endl;
	return true;
}

bool NinjamConnection::ConsumeStereoPair(unsigned int outChannelLeft,
	const float*& left,
	const float*& right,
	unsigned int& numFrames) const
{
	left = nullptr;
	right = nullptr;
	numFrames = 0;

	const auto lastFrames = _lastNumFrames.load();
	if (!_isConnected || lastFrames == 0u)
		return false;

	auto rightIndex = outChannelLeft + 1u;
	if ((outChannelLeft >= _numOutputChannels) || (rightIndex >= _numOutputChannels))
		return false;

	left = _outScratch[outChannelLeft].data();
	right = _outScratch[rightIndex].data();
	numFrames = _lastNumFrames.load();
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

void NinjamConnection::_ResizeScratchBuffers(unsigned int numFrames,
	unsigned int numInputScratchChannels)
{
	if ((_outScratch.size() != _numOutputChannels) || (_inScratch.size() != numInputScratchChannels))
	{
		_outScratch.assign(_numOutputChannels, std::vector<float>(numFrames, 0.0f));
		_inScratch.assign(numInputScratchChannels, std::vector<float>(numFrames, 0.0f));
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

	_inPtrs.resize(numInputScratchChannels, nullptr);
	for (auto chan = 0u; chan < numInputScratchChannels; chan++)
		_inPtrs[chan] = _inScratch[chan].data();
}

void NinjamConnection::_ApplyLocalChannels()
{
	if (!_isConnected || !_client)
		return;

	const auto packing = _lanePacking.load(std::memory_order_acquire);

	for (auto existingChannel = _client->EnumLocalChannels(0);
		existingChannel >= 0;
		existingChannel = _client->EnumLocalChannels(0))
	{
		_client->DeleteLocalChannel(existingChannel);
	}

	for (auto lane = 0u; lane < static_cast<unsigned int>(packing.LaneCount); lane++)
	{
		const auto sourceChannel = static_cast<int>((lane * 2u) | 1024u);
		auto name = std::string("Jamma Mix ") + std::to_string(lane + 1u);

		if (!packing.Modulo)
		{
			if (lane < static_cast<unsigned int>(packing.DacPairs))
			{
				const auto baseChannel = lane * 2u;
				name = std::string("Jamma Out ") + std::to_string(baseChannel + 1u)
					+ "/" + std::to_string(baseChannel + 2u);
			}
			else
			{
				const auto adcPairIndex = lane - static_cast<unsigned int>(packing.DacPairs);
				const auto baseChannel = adcPairIndex * 2u;
				name = std::string("Jamma In ") + std::to_string(baseChannel + 1u)
					+ "/" + std::to_string(baseChannel + 2u);
			}
		}

		_client->SetLocalChannelInfo(
			static_cast<int>(lane),
			name.c_str(),
			true,
			sourceChannel,
			true,
			96,
			true,
			true);

		std::cout << "[NINJAM] Local channel " << lane << ": " << name << std::endl;
	}

	_client->NotifyServerOfChannelChange();
}

void NinjamConnection::_UpdateSnapshot()
{
	if (!_isConnected || !_client)
		return;

	NinjamRemoteSnapshot snapshot;

	int intervalPos = 0;
	int intervalLength = 0;
	_client->GetPosition(&intervalPos, &intervalLength);
	snapshot.IntervalPositionSamps = intervalPos > 0 ? static_cast<unsigned int>(intervalPos) : 0u;
	snapshot.IntervalLengthSamps = intervalLength > 0 ? static_cast<unsigned int>(intervalLength) : 0u;
	snapshot.SampleRate = static_cast<unsigned int>(std::max(0, _client->GetSampleRate()));
	snapshot.Bpm = _client->GetActualBPM();
	snapshot.Bpi = _client->GetBPI();
	// Bounds-check against constants::MinPlausibleNinjamBpm/Bpi rather than a
	// bare positivity check: njclient briefly reports a nonsensical placeholder
	// tempo (e.g. bpm=2646, bpi=1) for the short window before the server's
	// real CONFIG_CHANGE_NOTIFY has been parsed, and that placeholder still
	// satisfies "> 0".
	snapshot.HasTiming = (snapshot.Bpm >= constants::MinPlausibleNinjamBpm)
		&& (snapshot.Bpm <= constants::MaxPlausibleNinjamBpm)
		&& (snapshot.Bpi >= constants::MinPlausibleNinjamBpi)
		&& (snapshot.Bpi <= constants::MaxPlausibleNinjamBpi)
		&& (snapshot.SampleRate > 0u);
	const auto localUserName = std::string(_client->GetUser() ? _client->GetUser() : "");

	std::set<std::string> activeUsers;
	const auto userCount = _client->GetNumUsers();
	std::vector<std::string> currentUserNames;
	currentUserNames.reserve(static_cast<size_t>(userCount));
	for (auto userIndex = 0; userIndex < userCount; userIndex++)
	{
		const auto* userNameC = _client->GetUserState(userIndex);
		const auto userName = std::string(userNameC ? userNameC : "");
		currentUserNames.push_back(userName);

		if (!userNameC || (*userNameC == '\0'))
			continue;

		if (!localUserName.empty() && EqualsIgnoreCase(userName, localUserName))
			continue;

		NinjamRemoteUser user;
		user.UserName = userName;
		activeUsers.insert(user.UserName);
		user.AssignedOutputChannel = _AssignOutputChannel(user.UserName);

		for (auto ordinal = 0;; ordinal++)
		{
			auto channelIndex = _client->EnumUserChannels(userIndex, ordinal);
			if (channelIndex < 0)
				break;

			bool subscribed = false;
			_client->GetUserChannelState(userIndex, channelIndex, &subscribed);

			_client->SetUserChannelState(
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
			const auto* channelNameC = _client->GetUserChannelState(
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
			channel.PeakLeft = _client->GetUserChannelPeak(userIndex, channelIndex, 0);
			channel.PeakRight = _client->GetUserChannelPeak(userIndex, channelIndex, 1);
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

	if (currentUserNames != _lastLoggedUsers)
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

		_lastLoggedUsers = std::move(currentUserNames);
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

	for (auto outChannel = kReservedMonitorOutputChannels;
		(outChannel + 1u) < _numOutputChannels;
		outChannel += 2u)
	{
		if (usedChannels.find(outChannel) == usedChannels.end())
		{
			_userOutputChannels[userName] = outChannel;
			return outChannel;
		}
	}

	const auto fallbackChannel = (_numOutputChannels > kReservedMonitorOutputChannels + 1u) ?
		kReservedMonitorOutputChannels :
		0u;
	_userOutputChannels[userName] = fallbackChannel;
	return fallbackChannel;
}

void NinjamConnection::SendChat(const std::string& message)
{
	// Serialize with Disconnect() so we cannot call ChatMessage_Send
	// concurrently with or after NJClient::Disconnect().
	std::scoped_lock lock(_connectionMutex);

	if (!_isConnected || !_client || message.empty())
		return;

	_client->ChatMessage_Send("MSG", message.c_str());
}

void NinjamConnection::_OnChatMessage(void* userData,
	NJClient* /*inst*/,
	const char** parms,
	int nparms)
{
	// userData is the NinjamConnection instance; reserved for future use.
	(void)userData;

	if (!parms || nparms < 1 || !parms[0])
		return;

	const std::string type(parms[0]);

	if (type == "MSG")
	{
		const auto* user = (nparms > 1 && parms[1]) ? parms[1] : "";
		const auto* text = (nparms > 2 && parms[2]) ? parms[2] : "";
		if (*user != '\0')
			std::cout << "[NINJAM] <" << user << "> " << text << std::endl;
		else
			std::cout << "[NINJAM] ** " << text << std::endl;
	}
	else if (type == "PRIVMSG")
	{
		const auto* user = (nparms > 1 && parms[1]) ? parms[1] : "";
		const auto* text = (nparms > 2 && parms[2]) ? parms[2] : "";
		std::cout << "[NINJAM] (private) <" << user << "> " << text << std::endl;
	}
	else if (type == "TOPIC")
	{
		const auto* user = (nparms > 1 && parms[1]) ? parms[1] : "";
		const auto* text = (nparms > 2 && parms[2]) ? parms[2] : "";
		if (*user != '\0')
			std::cout << "[NINJAM] Topic set by <" << user << ">: " << text << std::endl;
		else
			std::cout << "[NINJAM] Topic: " << text << std::endl;
	}
	else if (type == "JOIN")
	{
		const auto* user = (nparms > 1 && parms[1]) ? parms[1] : "";
		std::cout << "[NINJAM] --> " << user << " joined" << std::endl;
	}
	else if (type == "PART")
	{
		const auto* user = (nparms > 1 && parms[1]) ? parms[1] : "";
		std::cout << "[NINJAM] <-- " << user << " left" << std::endl;
	}
}
