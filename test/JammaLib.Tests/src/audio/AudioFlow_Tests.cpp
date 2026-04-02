
#include "gtest/gtest.h"
#include "resources/ResourceLib.h"
#include "engine/Station.h"
#include "engine/LoopTake.h"
#include "engine/Loop.h"
#include "audio/ChannelMixer.h"
#include "audio/AudioMixer.h"

using engine::Station;
using engine::StationParams;
using engine::LoopTake;
using engine::LoopTakeParams;
using engine::Loop;
using engine::LoopParams;
using audio::ChannelMixer;
using audio::ChannelMixerParams;
using audio::AudioMixerParams;
using audio::MergeMixBehaviourParams;
using audio::WireMixBehaviourParams;
using base::Audible;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

// Creates a Station managed by shared_ptr (required for shared_from_this).
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

// Creates a LoopTake managed by shared_ptr.
std::shared_ptr<LoopTake> MakeTake()
{
	LoopTakeParams takeParams;
	takeParams.Size = { 100, 100 };
	takeParams.FadeSamps = constants::DefaultFadeSamps;

	MergeMixBehaviourParams mergeParams;
	auto mixerParams = LoopTake::GetMixerParams(takeParams.Size, mergeParams);
	return std::make_shared<LoopTake>(takeParams, mixerParams);
}

// Creates a ChannelMixer with N input and output channels.
ChannelMixer MakeChannelMixer(unsigned int numChans, unsigned int bufSize)
{
	ChannelMixerParams p;
	p.InputBufferSize = bufSize;
	p.OutputBufferSize = bufSize;
	p.NumInputChannels = numChans;
	p.NumOutputChannels = numChans;
	return ChannelMixer(p);
}

// Deterministic pseudo-random sample in the range (-1.0, 1.0).
// Uses a simple LCG-style hash to avoid depending on rand() state.
float TestSample(unsigned int index, unsigned int multiplier = 7)
{
	return static_cast<float>((((index + 1) * multiplier) % 2000) - 1000) / 1001.0f;
}

// Fills an interleaved buffer with deterministic non-zero test data.
// buf must have space for numChans * numSamps floats.
void FillTestData(float* buf, unsigned int numChans, unsigned int numSamps,
	unsigned int blockIndex = 0)
{
	for (unsigned int s = 0; s < numSamps; s++)
	{
		for (unsigned int c = 0; c < numChans; c++)
		{
			auto globalIndex = blockIndex * numSamps + s;
			buf[s * numChans + c] = TestSample(globalIndex, (c + 1) * 7);
		}
	}
}

// Write one block through the ADC -> Station pipeline.
void WriteBlock(ChannelMixer& chanMixer,
	const std::shared_ptr<Station>& station,
	float* inBuf, unsigned int numChans, unsigned int numSamps)
{
	chanMixer.FromAdc(inBuf, numChans, numSamps);
	chanMixer.Source()->OnPlay(
		std::dynamic_pointer_cast<base::MultiAudioSink>(station),
		nullptr, 0, numSamps);
	chanMixer.Source()->EndMultiPlay(numSamps);
	station->EndMultiWrite(numSamps, true, Audible::AUDIOSOURCE_ADC);
}

// Read one block from the Station -> DAC pipeline.
void ReadBlock(ChannelMixer& chanMixer,
	const std::shared_ptr<Station>& station,
	float* outBuf, unsigned int numChans, unsigned int numSamps)
{
	station->Zero(numSamps, Audible::AUDIOSOURCE_LOOPS);
	chanMixer.Sink()->Zero(numSamps, Audible::AUDIOSOURCE_LOOPS);
	station->OnPlay(chanMixer.Sink(), nullptr, 0, numSamps);
	chanMixer.ToDac(outBuf, numChans, numSamps);
	chanMixer.Sink()->EndMultiWrite(numSamps, true, Audible::AUDIOSOURCE_LOOPS);
	station->EndMultiPlay(numSamps);
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

bool IsAllZero(const float* buf, unsigned int count)
{
	return !HasNonZero(buf, count);
}

// Capturing single-channel AudioSink for per-sample verification.
class CaptureSink :
	public base::AudioSink
{
public:
	CaptureSink(unsigned int bufSize) : Samples(bufSize, 0.0f) {}

	virtual int OnMixWrite(float samp, float fadeCurrent, float fadeNew,
		int indexOffset, base::Audible::AudioSourceType source) override
	{
		auto bufferIndex = _writeIndex + indexOffset;
		if (bufferIndex < Samples.size())
			Samples[bufferIndex] = (fadeNew * samp) + (fadeCurrent * Samples[bufferIndex]);
		return indexOffset + 1;
	}
	virtual void EndWrite(unsigned int numSamps, bool updateIndex) override
	{
		if (updateIndex) _writeIndex += numSamps;
	}

	std::vector<float> Samples;
};

// Capturing single-channel MultiAudioSink wrapping a CaptureSink.
class CaptureMultiSink :
	public base::MultiAudioSink
{
public:
	CaptureMultiSink(unsigned int bufSize)
		: _sink(std::make_shared<CaptureSink>(bufSize)) {}

	virtual unsigned int NumInputChannels(
		base::Audible::AudioSourceType source) const override { return 1; }
	std::shared_ptr<CaptureSink> GetSink() const { return _sink; }

protected:
	virtual const std::shared_ptr<base::AudioSink> _InputChannel(
		unsigned int channel, base::Audible::AudioSourceType source) override
	{
		return (channel == 0) ? _sink : nullptr;
	}

private:
	std::shared_ptr<CaptureSink> _sink;
};

} // anonymous namespace

// ===========================================================================
// Write-path tests
// ===========================================================================

// 1. Single channel: write one block through ADC -> Station -> Take -> Loop,
//    verify that samples are recorded.
TEST(AudioFlow, SingleChannel_WriteBlockReachesLoop)
{
	const unsigned int numChans = 1;
	const unsigned int blockSize = 512;

	auto station = MakeStation(numChans);
	auto take = MakeTake();
	station->AddTake(take);
	take->Record({ 0 }, "test");
	station->CommitChanges();

	auto chanMixer = MakeChannelMixer(numChans, constants::MaxBlockSize);

	std::vector<float> inBuf(numChans * blockSize);
	FillTestData(inBuf.data(), numChans, blockSize);

	WriteBlock(chanMixer, station, inBuf.data(), numChans, blockSize);

	ASSERT_EQ(take->NumRecordedSamps(), blockSize);
}

// 2. Single channel: write multiple blocks, verify cumulative recording.
TEST(AudioFlow, SingleChannel_WriteMultiBlockAccumulates)
{
	const unsigned int numChans = 1;
	const unsigned int blockSize = 512;
	const unsigned int numBlocks = 4;

	auto station = MakeStation(numChans);
	auto take = MakeTake();
	station->AddTake(take);
	take->Record({ 0 }, "test");
	station->CommitChanges();

	auto chanMixer = MakeChannelMixer(numChans, constants::MaxBlockSize);

	std::vector<float> inBuf(numChans * blockSize);
	for (unsigned int b = 0; b < numBlocks; b++)
	{
		FillTestData(inBuf.data(), numChans, blockSize, b);
		WriteBlock(chanMixer, station, inBuf.data(), numChans, blockSize);
	}

	ASSERT_EQ(take->NumRecordedSamps(),
		static_cast<unsigned long>(blockSize) * numBlocks);
}

// 3. Two channels: write to both channels, verify both loops record.
TEST(AudioFlow, TwoChannel_WriteReachesBothLoops)
{
	const unsigned int numChans = 2;
	const unsigned int blockSize = 512;

	auto station = MakeStation(numChans);
	auto take = MakeTake();
	station->AddTake(take);
	take->Record({ 0, 1 }, "test");
	station->CommitChanges();

	auto chanMixer = MakeChannelMixer(numChans, constants::MaxBlockSize);

	std::vector<float> inBuf(numChans * blockSize);
	FillTestData(inBuf.data(), numChans, blockSize);

	WriteBlock(chanMixer, station, inBuf.data(), numChans, blockSize);

	ASSERT_EQ(take->NumRecordedSamps(), blockSize);
	ASSERT_EQ(take->NumInputChannels(Audible::AUDIOSOURCE_ADC), 2u);
}

// ===========================================================================
// Read-path tests
// ===========================================================================

// 4. Single channel: record a loop directly, play back through
//    Station -> DAC, verify non-zero output.
TEST(AudioFlow, SingleChannel_LoopPlaybackProducesOutput)
{
	const unsigned int numChans = 1;
	const unsigned int blockSize = 512;
	const unsigned long loopLength = 1000ul;
	const unsigned long totalRecord = constants::MaxLoopFadeSamps + loopLength;

	auto station = MakeStation(numChans);
	auto take = MakeTake();
	station->AddTake(take);

	take->Record({}, "test");
	auto loop0 = take->AddLoop(0, "test");
	loop0->Record();
	station->CommitChanges();

	// Fill the loop's buffer bank directly with non-zero data.
	for (unsigned long i = 0; i < totalRecord; i++)
	{
		float samp = TestSample(static_cast<unsigned int>(i));
		loop0->OnMixWrite(samp, 0.0f, 1.0f, static_cast<int>(i),
			Audible::AUDIOSOURCE_ADC);
	}
	loop0->EndWrite(static_cast<unsigned int>(totalRecord), true);

	// Transition to playing.
	take->Play(constants::MaxLoopFadeSamps, loopLength, 0);

	auto chanMixer = MakeChannelMixer(numChans, constants::MaxBlockSize);
	std::vector<float> outBuf(numChans * blockSize, 0.0f);

	ReadBlock(chanMixer, station, outBuf.data(), numChans, blockSize);

	ASSERT_TRUE(HasNonZero(outBuf.data(), numChans * blockSize));
}

// 5. Two channels: record two loops directly, play back through
//    Station -> DAC, verify both channels have non-zero output.
TEST(AudioFlow, TwoChannel_LoopPlaybackProducesOutput)
{
	const unsigned int numChans = 2;
	const unsigned int blockSize = 512;
	const unsigned long loopLength = 1000ul;
	const unsigned long totalRecord = constants::MaxLoopFadeSamps + loopLength;

	auto station = MakeStation(numChans);
	auto take = MakeTake();
	station->AddTake(take);

	take->Record({}, "test");
	auto loop0 = take->AddLoop(0, "test");
	auto loop1 = take->AddLoop(1, "test");
	loop0->Record();
	loop1->Record();
	station->CommitChanges();

	// Fill both loops with deterministic non-zero data.
	for (unsigned long i = 0; i < totalRecord; i++)
	{
		float samp0 = TestSample(static_cast<unsigned int>(i), 7);
		float samp1 = TestSample(static_cast<unsigned int>(i), 13);
		loop0->OnMixWrite(samp0, 0.0f, 1.0f, static_cast<int>(i),
			Audible::AUDIOSOURCE_ADC);
		loop1->OnMixWrite(samp1, 0.0f, 1.0f, static_cast<int>(i),
			Audible::AUDIOSOURCE_ADC);
	}
	loop0->EndWrite(static_cast<unsigned int>(totalRecord), true);
	loop1->EndWrite(static_cast<unsigned int>(totalRecord), true);

	// Transition to playing.
	take->Play(constants::MaxLoopFadeSamps, loopLength, 0);

	auto chanMixer = MakeChannelMixer(numChans, constants::MaxBlockSize);
	std::vector<float> outBuf(numChans * blockSize, 0.0f);

	ReadBlock(chanMixer, station, outBuf.data(), numChans, blockSize);

	// Check that channel 0 has non-zero output.
	bool ch0NonZero = false;
	bool ch1NonZero = false;
	for (unsigned int s = 0; s < blockSize; s++)
	{
		if (outBuf[s * numChans + 0] != 0.0f) ch0NonZero = true;
		if (outBuf[s * numChans + 1] != 0.0f) ch1NonZero = true;
	}
	ASSERT_TRUE(ch0NonZero);
	ASSERT_TRUE(ch1NonZero);
}

// ===========================================================================
// Round-trip tests
// ===========================================================================

// 6. Single channel: write data through full pipeline, transition to playing,
//    read back and verify output contains the written data.
TEST(AudioFlow, SingleChannel_WriteReadRoundtrip)
{
	const unsigned int numChans = 1;
	const unsigned int blockSize = 512;
	const unsigned long loopLength = 2048ul;
	const unsigned long totalRecord = constants::MaxLoopFadeSamps + loopLength;
	const unsigned int totalBlocks =
		static_cast<unsigned int>((totalRecord + blockSize - 1) / blockSize);

	auto station = MakeStation(numChans);
	auto take = MakeTake();
	station->AddTake(take);
	take->Record({ 0 }, "test");
	station->CommitChanges();

	auto chanMixer = MakeChannelMixer(numChans, constants::MaxBlockSize);

	// ----- Write phase: push test data through ADC -> Loop -----
	std::vector<float> allWritten;
	allWritten.reserve(totalBlocks * blockSize);
	std::vector<float> inBuf(numChans * blockSize);

	for (unsigned int b = 0; b < totalBlocks; b++)
	{
		auto sampsThisBlock = blockSize;
		FillTestData(inBuf.data(), numChans, sampsThisBlock, b);
		WriteBlock(chanMixer, station, inBuf.data(), numChans, sampsThisBlock);

		for (unsigned int s = 0; s < sampsThisBlock; s++)
			allWritten.push_back(inBuf[s * numChans]);
	}

	// ----- Transition to playing -----
	take->Play(constants::MaxLoopFadeSamps, loopLength, 0);

	// ----- Read phase: pull data from Loop -> DAC -----
	// Use a fresh channel mixer for read to avoid ADC buffer state interference.
	auto readMixer = MakeChannelMixer(numChans, constants::MaxBlockSize);
	const unsigned int readBlocks = 4;
	std::vector<float> allRead;
	allRead.reserve(readBlocks * blockSize);

	for (unsigned int b = 0; b < readBlocks; b++)
	{
		std::vector<float> outBuf(numChans * blockSize, 0.0f);
		ReadBlock(readMixer, station, outBuf.data(), numChans, blockSize);

		for (unsigned int s = 0; s < blockSize; s++)
			allRead.push_back(outBuf[s * numChans]);
	}

	// Verify: output must contain non-zero samples matching the
	// loop region of the written data. Due to the 2-block internal
	// mixing buffer delay, the first matching block may be offset.
	ASSERT_TRUE(HasNonZero(allRead.data(), static_cast<unsigned int>(allRead.size())));

	// Verify that the read samples match a portion of the written loop region.
	// The loop region in the written data starts at MaxLoopFadeSamps.
	// Account for possible multi-block delay: find the first non-zero read
	// sample and compare subsequent samples against the loop region.
	unsigned int firstNonZero = 0;
	for (unsigned int i = 0; i < allRead.size(); i++)
	{
		if (allRead[i] != 0.0f) { firstNonZero = i; break; }
	}

	unsigned int matchCount = 0;
	for (unsigned int i = firstNonZero; i < allRead.size() && matchCount < loopLength; i++)
	{
		auto loopIndex = (i - firstNonZero) % loopLength;
		auto expectedIndex = constants::MaxLoopFadeSamps + loopIndex;
		if (expectedIndex < allWritten.size())
		{
			if (allRead[i] != 0.0f && allRead[i] == allWritten[expectedIndex])
				matchCount++;
		}
	}

	ASSERT_GT(matchCount, 0u);
}

// 7. Two channels: write and read round-trip, verify both channels produce output.
TEST(AudioFlow, TwoChannel_WriteReadRoundtrip)
{
	const unsigned int numChans = 2;
	const unsigned int blockSize = 512;
	const unsigned long loopLength = 2048ul;
	const unsigned long totalRecord = constants::MaxLoopFadeSamps + loopLength;
	const unsigned int totalBlocks =
		static_cast<unsigned int>((totalRecord + blockSize - 1) / blockSize);

	auto station = MakeStation(numChans);
	auto take = MakeTake();
	station->AddTake(take);
	take->Record({ 0, 1 }, "test");
	station->CommitChanges();

	auto chanMixer = MakeChannelMixer(numChans, constants::MaxBlockSize);
	std::vector<float> inBuf(numChans * blockSize);

	// ----- Write phase -----
	for (unsigned int b = 0; b < totalBlocks; b++)
	{
		FillTestData(inBuf.data(), numChans, blockSize, b);
		WriteBlock(chanMixer, station, inBuf.data(), numChans, blockSize);
	}

	// ----- Transition to playing -----
	take->Play(constants::MaxLoopFadeSamps, loopLength, 0);

	// ----- Read phase -----
	auto readMixer = MakeChannelMixer(numChans, constants::MaxBlockSize);
	const unsigned int readBlocks = 4;
	bool ch0NonZero = false;
	bool ch1NonZero = false;

	for (unsigned int b = 0; b < readBlocks; b++)
	{
		std::vector<float> outBuf(numChans * blockSize, 0.0f);
		ReadBlock(readMixer, station, outBuf.data(), numChans, blockSize);

		for (unsigned int s = 0; s < blockSize; s++)
		{
			if (outBuf[s * numChans + 0] != 0.0f) ch0NonZero = true;
			if (outBuf[s * numChans + 1] != 0.0f) ch1NonZero = true;
		}
	}

	ASSERT_TRUE(ch0NonZero);
	ASSERT_TRUE(ch1NonZero);
}

// ===========================================================================
// Edge-case / state tests
// ===========================================================================

// 8. Verify writes are ignored when take is inactive (not recording).
TEST(AudioFlow, InactiveState_WriteIgnored)
{
	const unsigned int numChans = 1;
	const unsigned int blockSize = 512;

	auto station = MakeStation(numChans);
	auto take = MakeTake();
	station->AddTake(take);
	// Deliberately do NOT call take->Record(...). Take remains inactive.
	station->CommitChanges();

	auto chanMixer = MakeChannelMixer(numChans, constants::MaxBlockSize);
	std::vector<float> inBuf(numChans * blockSize);
	FillTestData(inBuf.data(), numChans, blockSize);

	// Write through the pipeline.
	WriteBlock(chanMixer, station, inBuf.data(), numChans, blockSize);

	// Take should have recorded nothing.
	ASSERT_EQ(take->NumRecordedSamps(), 0u);
}

// 9. Verify that reading an empty/inactive loop produces silence.
TEST(AudioFlow, ReadEmptyLoop_ProducesSilence)
{
	const unsigned int numChans = 1;
	const unsigned int blockSize = 512;

	auto station = MakeStation(numChans);
	auto take = MakeTake();
	station->AddTake(take);
	// Do NOT record anything. Take and loops are inactive.
	station->CommitChanges();

	auto chanMixer = MakeChannelMixer(numChans, constants::MaxBlockSize);
	std::vector<float> outBuf(numChans * blockSize, 0.0f);

	ReadBlock(chanMixer, station, outBuf.data(), numChans, blockSize);

	ASSERT_TRUE(IsAllZero(outBuf.data(), numChans * blockSize));
}

// ===========================================================================
// Per-sample value verification
// ===========================================================================

// 10. Write a known ascending sequence to a loop via OnMixWrite, read back
//     through Loop::OnPlay → mock sink, and verify every sample matches.
//     This validates that sample values pass through the mixer path unchanged
//     (WireMixBehaviour, fade = 1.0).
TEST(AudioFlow, WriteToLoop_ReadBackExactValues)
{
	const unsigned int loopLength = 64;
	const unsigned int blockSize = 32;
	const unsigned long totalRecord = constants::MaxLoopFadeSamps + loopLength;

	// Create a loop with WireMixBehaviour routing to channel 0.
	WireMixBehaviourParams wireBehaviour;
	wireBehaviour.Channels = { 0 };
	auto mixerParams = Loop::GetMixerParams({ 80, 80 }, wireBehaviour);

	LoopParams loopParams;
	loopParams.Wav = "test";
	loopParams.FadeSamps = constants::DefaultFadeSamps;

	Loop loop(loopParams, mixerParams);
	loop.Record();

	// Write a known ascending sequence. The loop region starts at
	// index MaxLoopFadeSamps. We write (k+1)*0.01f for position k
	// within the loop region; the fade-in region is left as zero.
	for (unsigned long i = 0; i < totalRecord; i++)
	{
		float val = (i >= constants::MaxLoopFadeSamps)
			? static_cast<float>((i - constants::MaxLoopFadeSamps) + 1) * 0.01f
			: 0.0f;
		loop.OnMixWrite(val, 0.0f, 1.0f, static_cast<int>(i),
			Audible::AUDIOSOURCE_ADC);
	}
	loop.EndWrite(static_cast<unsigned int>(totalRecord), true);

	// Transition to playing: _playIndex = MaxLoopFadeSamps.
	loop.Play(constants::MaxLoopFadeSamps, loopLength, false);

	// Read one block via Loop::OnPlay into a capturing mock sink.
	auto sink = std::make_shared<CaptureMultiSink>(blockSize);
	loop.OnPlay(sink, nullptr, 0, blockSize);
	loop.EndMultiPlay(blockSize);

	// Verify exact per-sample values. The mixer fade is 1.0 (Jump +
	// SetTarget in Record/Reset), so samples pass through unchanged.
	const auto& captured = sink->GetSink()->Samples;
	for (unsigned int s = 0; s < blockSize; s++)
	{
		float expected = static_cast<float>(s + 1) * 0.01f;
		ASSERT_FLOAT_EQ(captured[s], expected) << "Mismatch at sample " << s;
	}
}
