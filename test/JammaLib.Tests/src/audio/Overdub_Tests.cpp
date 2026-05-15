#include <cmath>
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

TEST(Overdub, BounceGeneratesNonZeroOutput)
{
	const unsigned int numChans = 2u;
	const unsigned int blockSize = 512u;
	const unsigned long loopLength =
		static_cast<unsigned long>(((constants::MaxLoopFadeSamps / blockSize) + 1u) * blockSize);
	const unsigned int captureBlocks = static_cast<unsigned int>(loopLength / blockSize);
	const unsigned long totalRecord = constants::MaxLoopFadeSamps + loopLength;

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

	std::vector<float> recordData(totalRecord, 0.0f);
	for (unsigned long i = constants::MaxLoopFadeSamps; i < totalRecord; i++)
		recordData[i] = TestSample(static_cast<unsigned int>(i - constants::MaxLoopFadeSamps));

	auto [station, sourceTake] = MakeSeedStation(
		numChans,
		loopLength,
		constants::MaxLoopFadeSamps,
		recordData);
	auto trigger = MakeOverdubTrigger(0u);
	station->AddTrigger(trigger);
	DrainCommitJobs(station);

	StartOverdub(station, cfg, streamParams);
	ASSERT_EQ(2u, station->NumTakes());

	auto targetTake = station->GetLoopTakes().back();
	ASSERT_NE(sourceTake->Id(), targetTake->Id());
	ASSERT_EQ(LoopTake::STATE_OVERDUBBING, targetTake->TakeState());
	ASSERT_EQ(1u, targetTake->GetLoops().size());

	auto callbackMixer = MakeChannelMixer(numChans, constants::MaxBlockSize);
	std::vector<float> inBuf(numChans * blockSize, 0.0f);
	std::vector<float> outBuf(numChans * blockSize, 0.0f);

	for (unsigned int block = 0; block < captureBlocks; block++)
	{
		SimulateAudioCallback(callbackMixer,
			station,
			inBuf.data(),
			outBuf.data(),
			numChans,
			blockSize,
			cfg,
			streamParams);
	}

	ASSERT_EQ(loopLength, targetTake->NumRecordedSamps());
	ASSERT_EQ(LoopTake::STATE_OVERDUBBING, targetTake->TakeState());

	EndOverdub(station, cfg, streamParams);
	ASSERT_TRUE(sourceTake->IsMuted());
	ASSERT_EQ(LoopTake::STATE_OVERDUBBINGRECORDING, targetTake->TakeState());
	ASSERT_EQ(loopLength, targetTake->NumRecordedSamps());

	SimulateRemainingRecordTail(callbackMixer,
		station,
		numChans,
		blockSize,
		constants::MaxLoopFadeSamps,
		cfg,
		streamParams);

	EXPECT_EQ(loopLength + constants::MaxLoopFadeSamps, targetTake->NumRecordedSamps());
	EXPECT_EQ(LoopTake::STATE_PLAYING, targetTake->TakeState());

	sourceTake->UnMute();
	targetTake->UnMute();
	SetRackRoutes(*sourceTake, { {0u, 0u} });
	SetRackRoutes(*targetTake, { {0u, 1u} });
	SetRackRoutes(*station, { {0u, 0u}, {1u, 1u} });
	DrainCommitJobs(station);

	auto readMixer = MakeChannelMixer(numChans, constants::MaxBlockSize);
	bool foundNonZero = false;
	bool leftObserved = false;
	float maxRight = 0.0f;
	std::optional<unsigned int> firstRightNonZeroBlock;
	for (unsigned int block = 0; block < OutputPathDelayBlocks + captureBlocks + 32u; block++)
	{
		std::vector<float> compareBuf(numChans * blockSize, 0.0f);
		ReadStationOutput(readMixer, station, compareBuf.data(), numChans, blockSize);

		std::vector<float> leftBuf(blockSize, 0.0f);
		std::vector<float> rightBuf(blockSize, 0.0f);
		for (unsigned int sample = 0; sample < blockSize; sample++)
		{
			leftBuf[sample] = compareBuf[(sample * numChans) + 0u];
			rightBuf[sample] = compareBuf[(sample * numChans) + 1u];
		}

		leftObserved = leftObserved || HasNonZero(leftBuf.data(), blockSize);
		auto blockMaxRight = MaxAbs(rightBuf.data(), blockSize);
		if (blockMaxRight > maxRight)
			maxRight = blockMaxRight;

		if (HasNonZero(rightBuf.data(), blockSize))
		{
			foundNonZero = true;
			firstRightNonZeroBlock = block;
			break;
		}
	}

	if (!foundNonZero)
		ADD_FAILURE() << "Did not observe overdubbed loop playback becoming non-zero"
			<< " (leftObserved=" << leftObserved
			<< ", maxRight=" << maxRight
			<< ", loopLength=" << loopLength
			<< ", captureBlocks=" << captureBlocks
			<< ", rightFirstBlock=" << (firstRightNonZeroBlock.has_value() ? std::to_string(firstRightNonZeroBlock.value()) : std::string("none"))
			<< ")";
}

TEST(Overdub, BounceRecordsEqualLengthLoopAndMatchesSourceAtOutput)
{
	const unsigned int numChans = 2u;
	const unsigned int blockSize = 512u;
	const unsigned long loopLength =
		static_cast<unsigned long>(((constants::MaxLoopFadeSamps / blockSize) + 1u) * blockSize);
	const unsigned int captureBlocks = static_cast<unsigned int>(loopLength / blockSize);
	const unsigned long totalRecord = constants::MaxLoopFadeSamps + loopLength;

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

	std::vector<float> recordData(totalRecord, 0.0f);
	for (unsigned long i = constants::MaxLoopFadeSamps; i < totalRecord; i++)
		recordData[i] = TestSample(static_cast<unsigned int>(i - constants::MaxLoopFadeSamps));

	auto [station, sourceTake] = MakeSeedStation(
		numChans,
		loopLength,
		constants::MaxLoopFadeSamps,
		recordData);
	auto trigger = MakeOverdubTrigger(0u);
	station->AddTrigger(trigger);
	DrainCommitJobs(station);

	StartOverdub(station, cfg, streamParams);
	ASSERT_EQ(2u, station->NumTakes());

	auto targetTake = station->GetLoopTakes().back();
	ASSERT_NE(sourceTake->Id(), targetTake->Id());
	ASSERT_EQ(LoopTake::STATE_OVERDUBBING, targetTake->TakeState());
	ASSERT_EQ(1u, targetTake->GetLoops().size());

	auto callbackMixer = MakeChannelMixer(numChans, constants::MaxBlockSize);
	std::vector<float> inBuf(numChans * blockSize, 0.0f);
	std::vector<float> outBuf(numChans * blockSize, 0.0f);

	for (unsigned int block = 0; block < captureBlocks; block++)
	{
		SimulateAudioCallback(callbackMixer,
			station,
			inBuf.data(),
			outBuf.data(),
			numChans,
			blockSize,
			cfg,
			streamParams);
	}

	ASSERT_EQ(loopLength, targetTake->NumRecordedSamps());
	ASSERT_EQ(LoopTake::STATE_OVERDUBBING, targetTake->TakeState());

	EndOverdub(station, cfg, streamParams);
	ASSERT_TRUE(sourceTake->IsMuted());
	ASSERT_EQ(LoopTake::STATE_OVERDUBBINGRECORDING, targetTake->TakeState());
	ASSERT_EQ(loopLength, targetTake->NumRecordedSamps());

	SimulateRemainingRecordTail(callbackMixer,
		station,
		numChans,
		blockSize,
		constants::MaxLoopFadeSamps,
		cfg,
		streamParams);

	EXPECT_EQ(loopLength + constants::MaxLoopFadeSamps, targetTake->NumRecordedSamps());
	EXPECT_EQ(LoopTake::STATE_PLAYING, targetTake->TakeState());

	sourceTake->UnMute();
	targetTake->UnMute();
	SetRackRoutes(*sourceTake, { {0u, 0u} });
	SetRackRoutes(*targetTake, { {0u, 1u} });
	SetRackRoutes(*station, { {0u, 0u}, {1u, 1u} });
	DrainCommitJobs(station);

	std::optional<size_t> firstMismatch;
	auto readMixer = MakeChannelMixer(numChans, constants::MaxBlockSize);
	bool startedComparing = false;
	bool leftObserved = false;
	bool rightObserved = false;
	unsigned int comparedBlocks = 0u;
	for (unsigned int block = 0; block < OutputPathDelayBlocks + captureBlocks + 32u; block++)
	{
		std::vector<float> compareBuf(numChans * blockSize, 0.0f);
		ReadStationOutput(readMixer, station, compareBuf.data(), numChans, blockSize);

		std::vector<float> leftBuf(blockSize, 0.0f);
		std::vector<float> rightBuf(blockSize, 0.0f);
		for (unsigned int sample = 0; sample < blockSize; sample++)
		{
			leftBuf[sample] = compareBuf[(sample * numChans) + 0u];
			rightBuf[sample] = compareBuf[(sample * numChans) + 1u];
		}

		leftObserved = leftObserved || HasNonZero(leftBuf.data(), blockSize);
		rightObserved = rightObserved || HasNonZero(rightBuf.data(), blockSize);

		if (!startedComparing)
		{
			if (!leftObserved || !rightObserved)
				continue;

			startedComparing = true;
		}

		if (comparedBlocks >= captureBlocks)
			break;

		if (!HasNonZero(leftBuf.data(), blockSize) || !HasNonZero(rightBuf.data(), blockSize))
			continue;

		for (unsigned int sample = 0; sample < blockSize; sample++)
		{
			auto sampleIndex = static_cast<size_t>(comparedBlocks * blockSize) + sample;
			auto left = leftBuf[sample];
			auto right = rightBuf[sample];
			if (std::abs(left - right) > 1e-6f)
			{
				firstMismatch = sampleIndex;
				break;
			}
		}

		if (firstMismatch.has_value())
			break;

		comparedBlocks++;
	}

	if (!startedComparing)
		ADD_FAILURE() << "Did not observe both routed channels becoming non-zero after unmuting the source take and rerouting source/overdub takes to opposite channels"
			<< " (leftObserved=" << leftObserved << ", rightObserved=" << rightObserved << ")";
	else if (comparedBlocks != captureBlocks)
		ADD_FAILURE() << "Only compared " << comparedBlocks << " stereo blocks after rerouting; expected " << captureBlocks;

	if (firstMismatch.has_value())
	{
		ADD_FAILURE() << "Mismatch at output sample " << firstMismatch.value()
			<< " between source left and overdub right channels";
	}
}