#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <optional>
#include "gtest/gtest.h"
#include "actions/KeyAction.h"
#include "audio/AudioDevice.h"
#include "audio/ChannelMixer.h"
#include "engine/Loop.h"
#include "engine/LoopTake.h"
#include "engine/Station.h"
#include "engine/Timer.h"
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
using engine::Station;
using engine::StationParams;
using engine::Timer;
using engine::Trigger;
using engine::TriggerBinding;
using engine::TriggerParams;
using io::UserConfig;

namespace {

constexpr auto ActivateChar = 49u;
constexpr auto DitchChar = 50u;
constexpr auto OutputPathDelayBlocks = 2u;
constexpr auto MaxExtraSettleBlocks = 3u;
constexpr auto MinAlignedVerificationBlocks = 24u;

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

struct OverdubSession
{
	OverdubTestParams Params{};
	UserConfig Cfg{};
	AudioStreamParams StreamParams{};
	std::shared_ptr<Station> Station;
	std::shared_ptr<LoopTake> SourceTake;
	std::shared_ptr<LoopTake> TargetTake;
	unsigned int OverdubBlocks = 0u;
	unsigned long LoopSamps = 0ul;
	unsigned long OverdubSamps = 0ul;
};

OverdubSession CreateOverdubSession(
	const OverdubTestParams& p,
	const std::function<void(const std::shared_ptr<Station>&)>& stationSetup = {})
{
	OverdubSession session;
	session.Params = p;
	session.OverdubBlocks = p.OverdubBlocks();
	session.LoopSamps = static_cast<unsigned long>(p.LoopBlocks) * p.BlockSize;
	session.OverdubSamps = static_cast<unsigned long>(session.OverdubBlocks) * p.BlockSize;

	auto [cfg, streamParams] = MakeAudioConfig(p.NumChans, p.BlockSize);
	session.Cfg = cfg;
	session.StreamParams = streamParams;

	auto seedData = MakeSeedData(session.LoopSamps);
	auto [station, sourceTake] = MakeSeedStation(p.NumChans,
		session.LoopSamps,
		constants::MaxLoopFadeSamps,
		seedData);
	session.Station = station;
	session.SourceTake = sourceTake;

	session.Station->AddTrigger(MakeOverdubTrigger(0u));
	if (stationSetup)
		stationSetup(session.Station);
	DrainCommitJobs(session.Station);

	auto preMixer = MakeChannelMixer(p.NumChans, constants::MaxBlockSize);
	AdvancePlayback(preMixer, session.Station, p.NumChans, p.BlockSize, p.PreStartBlocks);

	StartOverdub(session.Station, session.Cfg, session.StreamParams);
	if (session.Station->NumTakes() >= 2u)
		session.TargetTake = session.Station->GetLoopTakes().back();

	return session;
}

void AssertOverdubStarted(const OverdubSession& session)
{
	ASSERT_NE(nullptr, session.Station);
	ASSERT_NE(nullptr, session.SourceTake);
	ASSERT_NE(nullptr, session.TargetTake);
	ASSERT_EQ(2u, session.Station->NumTakes());
	ASSERT_NE(session.SourceTake->Id(), session.TargetTake->Id());
	ASSERT_EQ(LoopTake::STATE_OVERDUBBING, session.TargetTake->TakeState());
	ASSERT_EQ(1u, session.TargetTake->GetLoops().size());
}

void RunOverdubAndTail(OverdubSession& session, unsigned int tailSamps)
{
	auto callbackMixer = MakeChannelMixer(session.Params.NumChans, constants::MaxBlockSize);
	std::vector<float> inBuf(session.Params.NumChans * session.Params.BlockSize, 0.0f);
	std::vector<float> outBuf(session.Params.NumChans * session.Params.BlockSize, 0.0f);

	for (auto block = 0u; block < session.OverdubBlocks; block++)
	{
		SimulateAudioCallback(callbackMixer,
			session.Station,
			inBuf.data(),
			outBuf.data(),
			session.Params.NumChans,
			session.Params.BlockSize,
			session.Cfg,
			session.StreamParams);
	}

	ASSERT_EQ(session.OverdubSamps, session.TargetTake->NumRecordedSamps());
	ASSERT_EQ(LoopTake::STATE_OVERDUBBING, session.TargetTake->TakeState());

	EndOverdub(session.Station, session.Cfg, session.StreamParams);
	ASSERT_TRUE(session.SourceTake->IsMuted());
	ASSERT_EQ(LoopTake::STATE_OVERDUBBINGRECORDING, session.TargetTake->TakeState());
	ASSERT_EQ(session.OverdubSamps, session.TargetTake->NumRecordedSamps());

	SimulateRemainingRecordTail(callbackMixer,
		session.Station,
		session.Params.NumChans,
		session.Params.BlockSize,
		tailSamps,
		session.Cfg,
		session.StreamParams);

	ASSERT_EQ(LoopTake::STATE_PLAYING, session.TargetTake->TakeState());
}

void ExpectStandardTailLength(const OverdubSession& session)
{
	EXPECT_EQ(session.OverdubSamps + constants::MaxLoopFadeSamps,
		session.TargetTake->NumRecordedSamps());
}

void RouteSourceAndOverdubToStereo(const OverdubSession& session)
{
	session.SourceTake->UnMute();
	session.TargetTake->UnMute();
	SetRackRoutes(*session.SourceTake, { {0u, 0u} });
	SetRackRoutes(*session.TargetTake, { {0u, 1u} });
	SetRackRoutes(*session.Station, { {0u, 0u}, {1u, 1u} });
	DrainCommitJobs(session.Station);
}

struct RightChannelProbeResult
{
	bool FoundNonZero = false;
	float MaxAbsValue = 0.0f;
	std::optional<unsigned int> FirstNonZeroBlock;
};

RightChannelProbeResult ProbeRightChannelForSignal(
	const OverdubSession& session,
	unsigned int probeBlocks)
{
	RightChannelProbeResult result;
	auto readMixer = MakeChannelMixer(session.Params.NumChans, constants::MaxBlockSize);
	std::vector<float> buf(session.Params.NumChans * session.Params.BlockSize, 0.0f);

	for (auto block = 0u; block < probeBlocks; block++)
	{
		ReadStationOutput(readMixer,
			session.Station,
			buf.data(),
			session.Params.NumChans,
			session.Params.BlockSize);

		auto hasNonZero = false;
		for (auto s = 0u; s < session.Params.BlockSize; s++)
		{
			auto sample = buf[s * session.Params.NumChans + 1u];
			auto peak = std::abs(sample);
			if (peak > result.MaxAbsValue)
				result.MaxAbsValue = peak;
			if (sample != 0.0f)
				hasNonZero = true;
		}

		if (hasNonZero)
		{
			result.FoundNonZero = true;
			result.FirstNonZeroBlock = block;
			break;
		}
	}

	return result;
}

struct StereoAlignmentProbeResult
{
	bool FoundSteadyStart = false;
	bool LeftObserved = false;
	bool RightObserved = false;
	unsigned int ComparedBlocks = 0u;
	std::optional<unsigned int> SteadyStartBlock;
	std::optional<size_t> FirstMismatch;
	std::optional<int> BestLagAtMismatch;
	std::optional<float> BestLagMse;
};

StereoAlignmentProbeResult ProbeStereoAlignment(
	const OverdubSession& session,
	unsigned int requiredAlignedBlocks,
	unsigned int probeBlocks,
	bool computeLagAtMismatch)
{
	StereoAlignmentProbeResult result;
	auto readMixer = MakeChannelMixer(session.Params.NumChans, constants::MaxBlockSize);
	std::vector<float> buf(session.Params.NumChans * session.Params.BlockSize, 0.0f);
	std::vector<float> leftBuf(session.Params.BlockSize, 0.0f);
	std::vector<float> rightBuf(session.Params.BlockSize, 0.0f);

	for (auto block = 0u; block < probeBlocks; block++)
	{
		ReadStationOutput(readMixer,
			session.Station,
			buf.data(),
			session.Params.NumChans,
			session.Params.BlockSize);

		for (auto s = 0u; s < session.Params.BlockSize; s++)
		{
			leftBuf[s] = buf[s * session.Params.NumChans + 0u];
			rightBuf[s] = buf[s * session.Params.NumChans + 1u];
		}

		auto leftHasSignal = HasNonZero(leftBuf.data(), session.Params.BlockSize);
		auto rightHasSignal = HasNonZero(rightBuf.data(), session.Params.BlockSize);
		result.LeftObserved = result.LeftObserved || leftHasSignal;
		result.RightObserved = result.RightObserved || rightHasSignal;
		if (!leftHasSignal || !rightHasSignal)
			continue;

		auto blockMatches = true;
		for (auto s = 0u; s < session.Params.BlockSize; s++)
		{
			if (std::abs(leftBuf[s] - rightBuf[s]) <= 1e-6f)
				continue;

			blockMatches = false;
			if (result.FoundSteadyStart)
			{
				result.FirstMismatch = static_cast<size_t>(block) * session.Params.BlockSize + s;
				if (computeLagAtMismatch)
				{
					float mse = 0.0f;
					result.BestLagAtMismatch = BestLag(leftBuf.data(), rightBuf.data(), session.Params.BlockSize, 256, mse);
					result.BestLagMse = mse;
				}
			}
			break;
		}

		if (!result.FoundSteadyStart)
		{
			if (!blockMatches)
				continue;
			result.FoundSteadyStart = true;
			result.SteadyStartBlock = block;
			result.ComparedBlocks = 1u;
		}
		else
		{
			if (!blockMatches)
				break;
			result.ComparedBlocks++;
		}

		if (result.ComparedBlocks >= requiredAlignedBlocks)
			break;
	}

	return result;
}

} // namespace

TEST_P(Overdub, BounceProducesNonZeroAndAlignedOutput)
{
	const auto& p = GetParam();
	auto session = CreateOverdubSession(p);
	AssertOverdubStarted(session);
	RunOverdubAndTail(session, constants::MaxLoopFadeSamps);
	ExpectStandardTailLength(session);
	RouteSourceAndOverdubToStereo(session);

	const auto probeBlocks = OutputPathDelayBlocks + MaxExtraSettleBlocks + 64u;
	auto probe = ProbeRightChannelForSignal(session, probeBlocks);
	if (!probe.FoundNonZero)
		ADD_FAILURE()
			<< "No non-zero output from overdubbed loop"
			<< " (maxRight=" << probe.MaxAbsValue
			<< ", loopBlocks=" << p.LoopBlocks
			<< ", overdubBlocks=" << session.OverdubBlocks
			<< ", preStartBlocks=" << p.PreStartBlocks
			<< ", endTriggerOffset=" << p.EndTriggerOffsetBlocks
			<< ", firstNonZeroBlock=" << (probe.FirstNonZeroBlock.has_value() ? std::to_string(probe.FirstNonZeroBlock.value()) : std::string("none"))
			<< ")";

	const auto matchBlocks = (std::min)(p.LoopBlocks, session.OverdubBlocks);
	const auto requiredAlignedBlocks = (std::min)(matchBlocks, MinAlignedVerificationBlocks);
	const auto alignmentProbeBlocks = OutputPathDelayBlocks + requiredAlignedBlocks + 32u;
	auto alignment = ProbeStereoAlignment(session, requiredAlignedBlocks, alignmentProbeBlocks, true);

	if (!alignment.FoundSteadyStart)
		ADD_FAILURE()
			<< "Source and overdub channels never both non-zero simultaneously"
			<< " (leftObserved=" << alignment.LeftObserved
			<< ", rightObserved=" << alignment.RightObserved << ")";
	else if (alignment.SteadyStartBlock.value() > (OutputPathDelayBlocks + MaxExtraSettleBlocks))
		ADD_FAILURE()
			<< "Alignment locked too late at block " << alignment.SteadyStartBlock.value()
			<< " (max=" << (OutputPathDelayBlocks + MaxExtraSettleBlocks)
			<< ", loopBlocks=" << p.LoopBlocks
			<< ", overdubBlocks=" << session.OverdubBlocks
			<< ", preStartBlocks=" << p.PreStartBlocks
			<< ", endTriggerOffset=" << p.EndTriggerOffsetBlocks << ")";
	else if (alignment.ComparedBlocks != requiredAlignedBlocks)
		ADD_FAILURE()
			<< "Matched " << alignment.ComparedBlocks << " blocks, expected " << requiredAlignedBlocks
			<< " (steadyStart=" << alignment.SteadyStartBlock.value_or(0) << ")";

	if (alignment.FirstMismatch.has_value())
		ADD_FAILURE()
			<< "Mismatch at sample " << alignment.FirstMismatch.value()
			<< " (bestLag=" << alignment.BestLagAtMismatch.value_or(0)
			<< ", bestLagMse=" << alignment.BestLagMse.value_or(0.0f) << ")";
}

INSTANTIATE_TEST_SUITE_P(
	Overdub,
	Overdub,
	::testing::Values(
		// { BlockSize, NumChans, LoopBlocks, PreStartBlocks, EndTriggerOffsetBlocks }
		// Constraint: LoopBlocks * BlockSize >= MaxLoopFadeSamps (= 70000) so the
		// seed loop content region starts before the loop end in the buffer.
		OverdubTestParams{ 512u, 2u, 137u,   0u, 0 },  // overdub at loop start
		OverdubTestParams{ 512u, 2u, 137u, 150u, 0 }   // overdub after crossing one source-loop boundary
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

// ── Quantisation error sign end-to-end tests ────────────────────────────────
// These tests pre-seed the station clock with QUANTISE_POWER so that
// QuantiseLength is called when the overdub end-trigger fires.  A non-zero
// trigger offset exercises the error-sign path: positive error (late trigger)
// must shift the overdub play position forward by |error| so the two loops
// stay in phase during playback.
//
// BEFORE the fix:  POWER returns an inverted error sign, placing the overdub
// play start near the *end* of the loop → audible offset of ~(loopLength - 2*|error|).
// AFTER the fix:   play position is set correctly and both channels align.

TEST(Overdub, SeededClock_LateTrigger_LoopAlignedWithSource)
{
	const OverdubTestParams p{ 512u, 2u, 137u, 0u, 1 };
	const auto loopSamps = static_cast<unsigned long>(p.LoopBlocks) * p.BlockSize;

	// Pre-seed clock: grain = loopSamps, QUANTISE_POWER.
	// When the late end-trigger fires, QuantiseLength(overdubSamps) returns
	// (loopSamps, +blockSize) after fix, or (loopSamps, -blockSize) before fix.
	auto session = CreateOverdubSession(p, [loopSamps](const std::shared_ptr<Station>& station) {
		auto clock = std::make_shared<Timer>();
		clock->SetQuantisation(static_cast<unsigned int>(loopSamps), Timer::QUANTISE_POWER);
		station->SetClock(clock);
	});
	AssertOverdubStarted(session);

	// Tail: simulate enough samples to cover both BUG (endRecordSamps = MaxFade + blockSize)
	// and FIXED (endRecordSamps = MaxFade - blockSize) cases.
	// BUG endRecordSamps = MaxFade + blockSize = 70512 → needs 138 blocks of 512.
	// Use MaxFade + 2*blockSize to guarantee completion in either case.
	const auto tailSamps = constants::MaxLoopFadeSamps + 2u * p.BlockSize;
	RunOverdubAndTail(session, tailSamps);

	// After fix: total recorded ≈ loopSamps + MaxLoopFadeSamps (within 1 block overshoot
	// due to block-granular endRecordSampCount checking).
	// Before fix (wrong error sign): endRecordSamps is ~2×blockSize larger, so total
	// recorded overshoots by ~2×blockSize beyond loopSamps + MaxLoopFadeSamps.
	EXPECT_LE(session.TargetTake->NumRecordedSamps(),
		loopSamps + constants::MaxLoopFadeSamps + static_cast<unsigned long>(p.BlockSize));

	// Alignment check: route source → left, overdub → right; verify channels match.
	RouteSourceAndOverdubToStereo(session);

	const auto probeBlocks = OutputPathDelayBlocks + p.LoopBlocks + MaxExtraSettleBlocks + 32u;
	auto alignment = ProbeStereoAlignment(session, 16u, probeBlocks, false);

	if (!alignment.FoundSteadyStart)
		ADD_FAILURE() << "Source and overdub channels never aligned (late trigger test)";
	else if (alignment.FirstMismatch.has_value())
		ADD_FAILURE() << "Alignment mismatch at sample " << alignment.FirstMismatch.value()
			<< " after " << alignment.ComparedBlocks << " aligned blocks";
}