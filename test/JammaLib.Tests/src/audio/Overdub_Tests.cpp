#include <algorithm>
#include <cmath>
#include <limits>
#include "gtest/gtest.h"
#include "actions/KeyAction.h"
#include "audio/AudioDevice.h"
#include "audio/ChannelMixer.h"
#include "engine/Loop.h"
#include "engine/LoopTake.h"
#include "engine/Station.h"
#include "engine/Trigger.h"
#include "io/UserConfig.h"

using actions::KeyAction;
using audio::AudioStreamParams;
using audio::ChannelMixer;
using audio::ChannelMixerParams;
using audio::MergeMixBehaviourParams;
using base::AudioWriteRequest;
using base::Audible;
using engine::LoopTake;
using engine::LoopTakeParams;
using engine::Station;
using engine::StationParams;
using engine::Trigger;
using engine::TriggerBinding;
using engine::TriggerParams;
using io::UserConfig;

namespace {

constexpr auto ActivateChar = 49u;
constexpr auto DitchChar = 50u;
constexpr auto OutputPathDelayBlocks = 2u;
constexpr auto MaxExtraSettleBlocks = 3u;

struct OverdubTestParams
{
	unsigned int BlockSize;
	unsigned int NumChans;
	unsigned int LoopBlocks;
	unsigned int PreStartBlocks;  // source loop blocks to advance before starting overdub
	int EndTriggerOffsetBlocks;   // signed offset on end trigger; 0 = on loop boundary

	unsigned int OverdubBlocks() const
	{
		auto n = static_cast<long long>(LoopBlocks) + EndTriggerOffsetBlocks;
		return static_cast<unsigned int>((std::max)(n, 1ll));
	}
};

void PrintTo(const OverdubTestParams& p, std::ostream* os)
{
	auto sign = (p.EndTriggerOffsetBlocks >= 0) ? "p" : "m";
	auto absOfs = (p.EndTriggerOffsetBlocks >= 0) ? p.EndTriggerOffsetBlocks : -p.EndTriggerOffsetBlocks;
	*os << "blk" << p.BlockSize
		<< "_ch" << p.NumChans
		<< "_loop" << p.LoopBlocks
		<< "_pre" << p.PreStartBlocks
		<< "_ofs" << sign << absOfs;
}

std::pair<UserConfig, AudioStreamParams> MakeAudioConfig(unsigned int numChans, unsigned int blockSize)
{
	UserConfig cfg;
	cfg.Audio.SampleRate = constants::DefaultSampleRate;
	cfg.Audio.BufSize = blockSize;
	cfg.Audio.NumBuffers = 1u;
	cfg.Audio.NumChannelsIn = numChans;
	cfg.Audio.NumChannelsOut = numChans;
	cfg.Audio.LatencyIn = 0u;
	cfg.Audio.LatencyOut = 0u;
	cfg.Trigger.PreDelay = 0u;

	AudioStreamParams streamParams{};
	streamParams.SampleRate = cfg.Audio.SampleRate;
	streamParams.BufSize = blockSize;
	streamParams.NumBuffers = cfg.Audio.NumBuffers;
	streamParams.NumInputChannels = numChans;
	streamParams.NumOutputChannels = numChans;
	streamParams.InputLatency = 0u;
	streamParams.OutputLatency = 0u;

	return { cfg, streamParams };
}

class Overdub : public ::testing::TestWithParam<OverdubTestParams> {};

std::shared_ptr<Station> MakeStation(unsigned int numChans)
{
	StationParams stationParams;
	stationParams.Size = { 200, 200 };
	stationParams.FadeSamps = constants::DefaultFadeSamps;

	MergeMixBehaviourParams mergeParams;
	auto mixerParams = Station::GetMixerParams(stationParams.Size, mergeParams);
	auto station = std::make_shared<Station>(stationParams, mixerParams);

	station->SetNumBusChannels(numChans);
	station->SetNumDacChannels(numChans);

	return station;
}

ChannelMixer MakeChannelMixer(unsigned int numChans, unsigned int bufSize)
{
	ChannelMixerParams params;
	params.InputBufferSize = bufSize;
	params.OutputBufferSize = bufSize;
	params.NumInputChannels = numChans;
	params.NumOutputChannels = numChans;
	return ChannelMixer(params);
}

float TestSample(unsigned int index, unsigned int multiplier = 17u)
{
	const auto wrapped = static_cast<int>(((index + 1u) * multiplier) % 2000u);
	return static_cast<float>(wrapped - 1000) / 1001.0f;
}

std::vector<float> MakeSeedData(unsigned long loopSamps)
{
	const auto total = constants::MaxLoopFadeSamps + loopSamps;
	std::vector<float> data(total, 0.0f);
	for (unsigned long i = constants::MaxLoopFadeSamps; i < total; i++)
		data[i] = TestSample(static_cast<unsigned int>(i - constants::MaxLoopFadeSamps));
	return data;
}

bool HasNonZero(const float* buf, unsigned int count)
{
	for (unsigned int i = 0; i < count; i++)
	{
		if (buf[i] != 0.0f)
			return true;
	}

	return false;
}

float MaxAbs(const float* buf, unsigned int count)
{
	auto maxVal = 0.0f;
	for (unsigned int i = 0; i < count; i++)
	{
		auto absVal = std::abs(buf[i]);
		if (absVal > maxVal)
			maxVal = absVal;
	}

	return maxVal;
}

int BestLag(const float* left,
	const float* right,
	unsigned int count,
	int maxLag,
	float& outMse)
{
	auto bestLag = 0;
	auto bestMse = (std::numeric_limits<float>::max)();

	for (auto lag = -maxLag; lag <= maxLag; lag++)
	{
		auto sqErr = 0.0;
		auto overlap = 0u;

		for (auto i = 0u; i < count; i++)
		{
			auto rightIndex = static_cast<int>(i) + lag;
			if ((rightIndex < 0) || (rightIndex >= static_cast<int>(count)))
				continue;

			auto diff = static_cast<double>(left[i]) - static_cast<double>(right[rightIndex]);
			sqErr += diff * diff;
			overlap++;
		}

		if (0u == overlap)
			continue;

		auto mse = static_cast<float>(sqErr / static_cast<double>(overlap));
		if (mse < bestMse)
		{
			bestMse = mse;
			bestLag = lag;
		}
	}

	outMse = bestMse;
	return bestLag;
}

void SetRackRoutes(base::ActionReceiver& receiver,
	const std::vector<std::pair<unsigned int, unsigned int>>& connections)
{
	actions::GuiAction action;
	action.ElementType = actions::GuiAction::ACTIONELEMENT_RACK;
	action.Index = 0;
	action.Data = actions::GuiAction::GuiConnections{ connections };
	receiver.OnAction(action);
}

void DrainCommitJobs(const std::shared_ptr<Station>& station)
{
	while (true)
	{
		auto jobs = station->CommitChanges();
		if (jobs.empty())
			return;

		for (auto& job : jobs)
		{
			auto receiver = job.Receiver.lock();
			if (receiver)
				receiver->OnAction(job);
		}
	}
}

std::shared_ptr<Trigger> MakeOverdubTrigger(unsigned int inputChannel)
{
	engine::DualBinding activate;
	activate.SetDown(TriggerBinding(engine::TRIGGER_KEY, ActivateChar, 1), true);
	activate.SetRelease(TriggerBinding(engine::TRIGGER_KEY, ActivateChar, 0), true);

	engine::DualBinding ditch;
	ditch.SetDown(TriggerBinding(engine::TRIGGER_KEY, DitchChar, 1), true);
	ditch.SetRelease(TriggerBinding(engine::TRIGGER_KEY, DitchChar, 0), true);

	TriggerParams triggerParams;
	triggerParams.Activate = { activate };
	triggerParams.Ditch = { ditch };
	triggerParams.InputChannels = { inputChannel };
	triggerParams.DebounceMs = 0u;

	return std::make_shared<Trigger>(triggerParams);
}

void SendKey(const std::shared_ptr<Station>& station,
	unsigned int keyChar,
	int keyType,
	const UserConfig& cfg,
	const AudioStreamParams& streamParams)
{
	KeyAction action;
	action.KeyChar = keyChar;
	action.KeyActionType = (keyType == KeyAction::KEY_DOWN) ?
		KeyAction::KEY_DOWN :
		KeyAction::KEY_UP;
	action.SetActionTime(engine::Timer::GetTime());
	action.SetUserConfig(cfg);
	action.SetAudioParams(streamParams);
	station->OnAction(action);
}

void StartOverdub(const std::shared_ptr<Station>& station,
	const UserConfig& cfg,
	const AudioStreamParams& streamParams)
{
	SendKey(station, DitchChar, KeyAction::KEY_DOWN, cfg, streamParams);
	SendKey(station, ActivateChar, KeyAction::KEY_DOWN, cfg, streamParams);
	SendKey(station, ActivateChar, KeyAction::KEY_UP, cfg, streamParams);
	SendKey(station, DitchChar, KeyAction::KEY_UP, cfg, streamParams);
	DrainCommitJobs(station);
}

void EndOverdub(const std::shared_ptr<Station>& station,
	const UserConfig& cfg,
	const AudioStreamParams& streamParams)
{
	SendKey(station, DitchChar, KeyAction::KEY_DOWN, cfg, streamParams);
	SendKey(station, ActivateChar, KeyAction::KEY_DOWN, cfg, streamParams);
	SendKey(station, ActivateChar, KeyAction::KEY_UP, cfg, streamParams);
	SendKey(station, DitchChar, KeyAction::KEY_UP, cfg, streamParams);
	DrainCommitJobs(station);
}

std::pair<std::shared_ptr<Station>, std::shared_ptr<LoopTake>> MakeSeedStation(
	unsigned int numChans,
	unsigned long loopLength,
	unsigned long playPos,
	const std::vector<float>& recordData)
{
	auto station = MakeStation(numChans);
	auto take = station->AddTake();
	take->Record({}, "test");
	auto loop = take->AddLoop(0u, "test");
	loop->Record();
	DrainCommitJobs(station);

	AudioWriteRequest writeReq;
	writeReq.samples = recordData.data();
	writeReq.numSamps = static_cast<unsigned int>(recordData.size());
	writeReq.stride = 1;
	writeReq.fadeCurrent = 0.0f;
	writeReq.fadeNew = 1.0f;
	writeReq.source = Audible::AUDIOSOURCE_ADC;
	loop->OnBlockWrite(writeReq, 0);
	loop->EndWrite(static_cast<unsigned int>(recordData.size()), true);

	take->Play(playPos, loopLength, 0u);
	return { station, take };
}

void ReadStationOutput(ChannelMixer& chanMixer,
	const std::shared_ptr<Station>& station,
	float* outBuf,
	unsigned int numChans,
	unsigned int numSamps)
{
	station->Zero(numSamps, Audible::AUDIOSOURCE_LOOPS);
	chanMixer.Sink()->Zero(numSamps, Audible::AUDIOSOURCE_LOOPS);
	station->WriteBlock(chanMixer.Sink(), nullptr, 0, numSamps);
	station->EndMultiPlay(numSamps);
	chanMixer.ToDac(outBuf, numChans, numSamps);
	chanMixer.Sink()->EndMultiWrite(numSamps, true, Audible::AUDIOSOURCE_LOOPS);
}

void AdvancePlayback(ChannelMixer& chanMixer,
	const std::shared_ptr<Station>& station,
	unsigned int numChans,
	unsigned int blockSize,
	unsigned int numBlocks)
{
	std::vector<float> outBuf(numChans * blockSize, 0.0f);
	for (auto block = 0u; block < numBlocks; block++)
		ReadStationOutput(chanMixer, station, outBuf.data(), numChans, blockSize);
}

void SimulateAudioCallback(ChannelMixer& chanMixer,
	const std::shared_ptr<Station>& station,
	float* inBuf,
	float* outBuf,
	unsigned int numChans,
	unsigned int numSamps,
	const UserConfig& cfg,
	const AudioStreamParams& streamParams)
{
	auto inLatency = (0u == streamParams.InputLatency) ?
		cfg.Audio.LatencyIn :
		streamParams.InputLatency;

	chanMixer.FromAdc(inBuf, numChans, numSamps);
	chanMixer.InitPlay(0u, numSamps);
	chanMixer.Source()->SetSourceType(Audible::AUDIOSOURCE_MONITOR);
	chanMixer.WriteToSink(station, numSamps);

	chanMixer.InitPlay(cfg.AdcBufferDelay(inLatency), numSamps);
	chanMixer.Source()->SetSourceType(Audible::AUDIOSOURCE_ADC);
	chanMixer.WriteToSink(station, numSamps);

	station->SetSourceType(Audible::AUDIOSOURCE_MONITOR);
	station->OnBounce(numSamps, cfg, streamParams);

	station->SetSourceType(Audible::AUDIOSOURCE_BOUNCE);
	station->OnBounce(numSamps, cfg, streamParams);
	station->EndMultiWrite(numSamps, true, Audible::AUDIOSOURCE_BOUNCE);

	chanMixer.Source()->EndMultiPlay(numSamps);

	std::fill(outBuf, outBuf + (numChans * numSamps), 0.0f);
	station->Zero(numSamps, Audible::AUDIOSOURCE_LOOPS);
	chanMixer.Sink()->Zero(numSamps, Audible::AUDIOSOURCE_LOOPS);
	station->WriteBlock(chanMixer.Sink(), nullptr, 0, numSamps);
	station->EndMultiPlay(numSamps);
	chanMixer.ToDac(outBuf, numChans, numSamps);
	chanMixer.Sink()->EndMultiWrite(numSamps, true, Audible::AUDIOSOURCE_LOOPS);

	station->OnTick(engine::Timer::GetTime(), numSamps, cfg, streamParams);
	DrainCommitJobs(station);
}

void SimulateRemainingRecordTail(ChannelMixer& chanMixer,
	const std::shared_ptr<Station>& station,
	unsigned int numChans,
	unsigned int blockSize,
	unsigned int totalSamps,
	const UserConfig& cfg,
	const AudioStreamParams& streamParams)
{
	std::vector<float> inBuf(numChans * blockSize, 0.0f);
	std::vector<float> outBuf(numChans * blockSize, 0.0f);
	auto sampsLeft = totalSamps;

	while (sampsLeft > 0u)
	{
		auto sampsThisBlock = (blockSize < sampsLeft) ? blockSize : sampsLeft;
		SimulateAudioCallback(chanMixer,
			station,
			inBuf.data(),
			outBuf.data(),
			numChans,
			sampsThisBlock,
			cfg,
			streamParams);
		sampsLeft -= sampsThisBlock;
	}
}

} // namespace

TEST_P(Overdub, BounceGeneratesNonZeroOutput)
{
	const auto& p = GetParam();
	const auto overdubBlocks = p.OverdubBlocks();
	const unsigned long loopSamps = static_cast<unsigned long>(p.LoopBlocks) * p.BlockSize;
	const unsigned long overdubSamps = static_cast<unsigned long>(overdubBlocks) * p.BlockSize;

	auto [cfg, streamParams] = MakeAudioConfig(p.NumChans, p.BlockSize);
	auto seedData = MakeSeedData(loopSamps);
	auto [station, sourceTake] = MakeSeedStation(p.NumChans, loopSamps, constants::MaxLoopFadeSamps, seedData);
	station->AddTrigger(MakeOverdubTrigger(0u));
	DrainCommitJobs(station);

	auto preMixer = MakeChannelMixer(p.NumChans, constants::MaxBlockSize);
	AdvancePlayback(preMixer, station, p.NumChans, p.BlockSize, p.PreStartBlocks);

	StartOverdub(station, cfg, streamParams);
	ASSERT_EQ(2u, station->NumTakes());

	auto targetTake = station->GetLoopTakes().back();
	ASSERT_NE(sourceTake->Id(), targetTake->Id());
	ASSERT_EQ(LoopTake::STATE_OVERDUBBING, targetTake->TakeState());
	ASSERT_EQ(1u, targetTake->GetLoops().size());

	auto callbackMixer = MakeChannelMixer(p.NumChans, constants::MaxBlockSize);
	std::vector<float> inBuf(p.NumChans * p.BlockSize, 0.0f);
	std::vector<float> outBuf(p.NumChans * p.BlockSize, 0.0f);

	for (auto block = 0u; block < overdubBlocks; block++)
		SimulateAudioCallback(callbackMixer, station, inBuf.data(), outBuf.data(), p.NumChans, p.BlockSize, cfg, streamParams);

	ASSERT_EQ(overdubSamps, targetTake->NumRecordedSamps());
	ASSERT_EQ(LoopTake::STATE_OVERDUBBING, targetTake->TakeState());

	EndOverdub(station, cfg, streamParams);
	ASSERT_TRUE(sourceTake->IsMuted());
	ASSERT_EQ(LoopTake::STATE_OVERDUBBINGRECORDING, targetTake->TakeState());
	ASSERT_EQ(overdubSamps, targetTake->NumRecordedSamps());

	SimulateRemainingRecordTail(callbackMixer, station, p.NumChans, p.BlockSize, constants::MaxLoopFadeSamps, cfg, streamParams);

	EXPECT_EQ(overdubSamps + constants::MaxLoopFadeSamps, targetTake->NumRecordedSamps());
	EXPECT_EQ(LoopTake::STATE_PLAYING, targetTake->TakeState());

	sourceTake->UnMute();
	targetTake->UnMute();
	SetRackRoutes(*sourceTake, { {0u, 0u} });
	SetRackRoutes(*targetTake, { {0u, 1u} });
	SetRackRoutes(*station, { {0u, 0u}, {1u, 1u} });
	DrainCommitJobs(station);

	auto readMixer = MakeChannelMixer(p.NumChans, constants::MaxBlockSize);
	bool foundNonZero = false;
	float maxRight = 0.0f;
	std::optional<unsigned int> firstNonZeroBlock;
	const auto probeBlocks = OutputPathDelayBlocks + (std::max)(p.LoopBlocks, overdubBlocks) + 32u;
	for (auto block = 0u; block < probeBlocks; block++)
	{
		std::vector<float> buf(p.NumChans * p.BlockSize, 0.0f);
		ReadStationOutput(readMixer, station, buf.data(), p.NumChans, p.BlockSize);

		std::vector<float> rightBuf(p.BlockSize);
		for (auto s = 0u; s < p.BlockSize; s++)
			rightBuf[s] = buf[s * p.NumChans + 1u];

		auto peak = MaxAbs(rightBuf.data(), p.BlockSize);
		if (peak > maxRight) maxRight = peak;

		if (HasNonZero(rightBuf.data(), p.BlockSize))
		{
			foundNonZero = true;
			firstNonZeroBlock = block;
			break;
		}
	}

	if (!foundNonZero)
		ADD_FAILURE()
			<< "No non-zero output from overdubbed loop"
			<< " (maxRight=" << maxRight
			<< ", loopBlocks=" << p.LoopBlocks
			<< ", overdubBlocks=" << overdubBlocks
			<< ", preStartBlocks=" << p.PreStartBlocks
			<< ", endTriggerOffset=" << p.EndTriggerOffsetBlocks
			<< ", firstNonZeroBlock=" << (firstNonZeroBlock.has_value() ? std::to_string(firstNonZeroBlock.value()) : std::string("none"))
			<< ")";
}

TEST_P(Overdub, BounceRecordsEqualLengthLoopAndMatchesSourceAtOutput)
{
	const auto& p = GetParam();
	const auto overdubBlocks = p.OverdubBlocks();
	const auto matchBlocks = (std::min)(p.LoopBlocks, overdubBlocks);
	const unsigned long loopSamps = static_cast<unsigned long>(p.LoopBlocks) * p.BlockSize;
	const unsigned long overdubSamps = static_cast<unsigned long>(overdubBlocks) * p.BlockSize;

	auto [cfg, streamParams] = MakeAudioConfig(p.NumChans, p.BlockSize);
	auto seedData = MakeSeedData(loopSamps);
	auto [station, sourceTake] = MakeSeedStation(p.NumChans, loopSamps, constants::MaxLoopFadeSamps, seedData);
	station->AddTrigger(MakeOverdubTrigger(0u));
	DrainCommitJobs(station);

	auto preMixer = MakeChannelMixer(p.NumChans, constants::MaxBlockSize);
	AdvancePlayback(preMixer, station, p.NumChans, p.BlockSize, p.PreStartBlocks);

	StartOverdub(station, cfg, streamParams);
	ASSERT_EQ(2u, station->NumTakes());

	auto targetTake = station->GetLoopTakes().back();
	ASSERT_NE(sourceTake->Id(), targetTake->Id());
	ASSERT_EQ(LoopTake::STATE_OVERDUBBING, targetTake->TakeState());
	ASSERT_EQ(1u, targetTake->GetLoops().size());

	auto callbackMixer = MakeChannelMixer(p.NumChans, constants::MaxBlockSize);
	std::vector<float> inBuf(p.NumChans * p.BlockSize, 0.0f);
	std::vector<float> outBuf(p.NumChans * p.BlockSize, 0.0f);

	for (auto block = 0u; block < overdubBlocks; block++)
		SimulateAudioCallback(callbackMixer, station, inBuf.data(), outBuf.data(), p.NumChans, p.BlockSize, cfg, streamParams);

	ASSERT_EQ(overdubSamps, targetTake->NumRecordedSamps());
	ASSERT_EQ(LoopTake::STATE_OVERDUBBING, targetTake->TakeState());

	EndOverdub(station, cfg, streamParams);
	ASSERT_TRUE(sourceTake->IsMuted());
	ASSERT_EQ(LoopTake::STATE_OVERDUBBINGRECORDING, targetTake->TakeState());
	ASSERT_EQ(overdubSamps, targetTake->NumRecordedSamps());

	SimulateRemainingRecordTail(callbackMixer, station, p.NumChans, p.BlockSize, constants::MaxLoopFadeSamps, cfg, streamParams);

	EXPECT_EQ(overdubSamps + constants::MaxLoopFadeSamps, targetTake->NumRecordedSamps());
	EXPECT_EQ(LoopTake::STATE_PLAYING, targetTake->TakeState());

	sourceTake->UnMute();
	targetTake->UnMute();
	SetRackRoutes(*sourceTake, { {0u, 0u} });
	SetRackRoutes(*targetTake, { {0u, 1u} });
	SetRackRoutes(*station, { {0u, 0u}, {1u, 1u} });
	DrainCommitJobs(station);

	std::optional<size_t> firstMismatch;
	std::optional<int> bestLagAtMismatch;
	std::optional<float> bestLagMse;
	std::optional<unsigned int> steadyStartBlock;
	auto readMixer = MakeChannelMixer(p.NumChans, constants::MaxBlockSize);
	bool foundSteadyStart = false;
	bool leftObserved = false;
	bool rightObserved = false;
	unsigned int comparedBlocks = 0u;
	for (auto block = 0u; block < OutputPathDelayBlocks + matchBlocks + 32u; block++)
	{
		std::vector<float> buf(p.NumChans * p.BlockSize, 0.0f);
		ReadStationOutput(readMixer, station, buf.data(), p.NumChans, p.BlockSize);

		std::vector<float> leftBuf(p.BlockSize), rightBuf(p.BlockSize);
		for (auto s = 0u; s < p.BlockSize; s++)
		{
			leftBuf[s] = buf[s * p.NumChans + 0u];
			rightBuf[s] = buf[s * p.NumChans + 1u];
		}

		leftObserved = leftObserved || HasNonZero(leftBuf.data(), p.BlockSize);
		rightObserved = rightObserved || HasNonZero(rightBuf.data(), p.BlockSize);
		if (!HasNonZero(leftBuf.data(), p.BlockSize) || !HasNonZero(rightBuf.data(), p.BlockSize))
			continue;

		auto blockMatches = true;
		for (auto s = 0u; s < p.BlockSize; s++)
		{
			if (std::abs(leftBuf[s] - rightBuf[s]) > 1e-6f)
			{
				blockMatches = false;
				if (foundSteadyStart)
				{
					firstMismatch = static_cast<size_t>(block * p.BlockSize) + s;
					float mse = 0.0f;
					bestLagAtMismatch = BestLag(leftBuf.data(), rightBuf.data(), p.BlockSize, 256, mse);
					bestLagMse = mse;
				}
				break;
			}
		}

		if (!foundSteadyStart)
		{
			if (!blockMatches) continue;
			foundSteadyStart = true;
			steadyStartBlock = block;
			comparedBlocks = 1u;
		}
		else
		{
			if (!blockMatches) break;
			comparedBlocks++;
		}

		if (comparedBlocks >= matchBlocks)
			break;
	}

	if (!foundSteadyStart)
		ADD_FAILURE()
			<< "Source and overdub channels never both non-zero simultaneously"
			<< " (leftObserved=" << leftObserved << ", rightObserved=" << rightObserved << ")";
	else if (steadyStartBlock.value() > (OutputPathDelayBlocks + MaxExtraSettleBlocks))
		ADD_FAILURE()
			<< "Alignment locked too late at block " << steadyStartBlock.value()
			<< " (max=" << (OutputPathDelayBlocks + MaxExtraSettleBlocks)
			<< ", loopBlocks=" << p.LoopBlocks
			<< ", overdubBlocks=" << overdubBlocks
			<< ", preStartBlocks=" << p.PreStartBlocks
			<< ", endTriggerOffset=" << p.EndTriggerOffsetBlocks << ")";
	else if (comparedBlocks != matchBlocks)
		ADD_FAILURE()
			<< "Matched " << comparedBlocks << " blocks, expected " << matchBlocks
			<< " (steadyStart=" << steadyStartBlock.value_or(0) << ")";

	if (firstMismatch.has_value())
		ADD_FAILURE()
			<< "Mismatch at sample " << firstMismatch.value()
			<< " (bestLag=" << bestLagAtMismatch.value_or(0)
			<< ", bestLagMse=" << bestLagMse.value_or(0.0f) << ")";
}

INSTANTIATE_TEST_SUITE_P(
	Overdub,
	Overdub,
	::testing::Values(
		// { BlockSize, NumChans, LoopBlocks, PreStartBlocks, EndTriggerOffsetBlocks }
		// Constraint: LoopBlocks * BlockSize >= MaxLoopFadeSamps (= 70000) so the
		// seed loop content region starts before the loop end in the buffer.
		OverdubTestParams{ 512u, 2u, 137u,   0u, 0 },  // overdub at loop start
		OverdubTestParams{ 512u, 2u, 137u, 205u, 0 },  // overdub after 1.5 source loops
		OverdubTestParams{ 512u, 2u, 137u, 342u, 0 },  // overdub after 2.5 source loops
		OverdubTestParams{ 512u, 2u, 200u, 500u, 0 }   // longer loop, 2.5x pre-play
	),
	[](const ::testing::TestParamInfo<OverdubTestParams>& info) {
		const auto& p = info.param;
		auto sign = (p.EndTriggerOffsetBlocks >= 0) ? std::string("p") : std::string("m");
		auto absOfs = (p.EndTriggerOffsetBlocks >= 0) ? p.EndTriggerOffsetBlocks : -p.EndTriggerOffsetBlocks;
		return "blk" + std::to_string(p.BlockSize)
			+ "_ch" + std::to_string(p.NumChans)
			+ "_loop" + std::to_string(p.LoopBlocks)
			+ "_pre" + std::to_string(p.PreStartBlocks)
			+ "_ofs" + sign + std::to_string(absOfs);
	});