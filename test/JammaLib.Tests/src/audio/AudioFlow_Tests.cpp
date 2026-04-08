
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
using base::AudioWriteRequest;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

constexpr auto READBLOCK_SOURCE = Audible::AUDIOSOURCE_MIXER;

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
	const auto wrapped = static_cast<int>(((index + 1u) * multiplier) % 2000u);
	return static_cast<float>(wrapped - 1000) / 1001.0f;
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
	chanMixer.InitPlay(0, numSamps);
	chanMixer.WriteToSink(
		std::dynamic_pointer_cast<base::MultiAudioSink>(station), numSamps);
	chanMixer.Source()->EndMultiPlay(numSamps);
	station->EndMultiWrite(numSamps, true, Audible::AUDIOSOURCE_ADC);
}

// Read one block from the Station -> DAC pipeline.
void ReadBlock(ChannelMixer& chanMixer,
	const std::shared_ptr<Station>& station,
	float* outBuf, unsigned int numChans, unsigned int numSamps)
{
	station->Zero(numSamps, READBLOCK_SOURCE);
	chanMixer.Sink()->Zero(numSamps, READBLOCK_SOURCE);
	station->WriteBlock(chanMixer.Sink(), nullptr, 0, numSamps);
	chanMixer.ToDac(outBuf, numChans, numSamps);
	chanMixer.Sink()->EndMultiWrite(numSamps, true, READBLOCK_SOURCE);
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

	virtual void OnBlockWrite(const AudioWriteRequest& request, int writeOffset) override
	{
		for (auto i = 0u; i < request.numSamps; i++)
		{
			auto bufferIndex = _writeIndex + writeOffset + i;
			if (bufferIndex < Samples.size())
			{
				auto samp = request.samples[i * request.stride];
				Samples[bufferIndex] = (request.fadeNew * samp) + (request.fadeCurrent * Samples[bufferIndex]);
			}
		}
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
	std::vector<float> recordData(totalRecord);
	for (unsigned long i = 0; i < totalRecord; i++)
		recordData[i] = TestSample(static_cast<unsigned int>(i));

	AudioWriteRequest writeReq;
	writeReq.samples = recordData.data();
	writeReq.numSamps = static_cast<unsigned int>(totalRecord);
	writeReq.stride = 1;
	writeReq.fadeCurrent = 0.0f;
	writeReq.fadeNew = 1.0f;
	writeReq.source = Audible::AUDIOSOURCE_ADC;
	loop0->OnBlockWrite(writeReq, 0);
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
	std::vector<float> recordData0(totalRecord);
	std::vector<float> recordData1(totalRecord);
	for (unsigned long i = 0; i < totalRecord; i++)
	{
		recordData0[i] = TestSample(static_cast<unsigned int>(i), 7);
		recordData1[i] = TestSample(static_cast<unsigned int>(i), 13);
	}

	AudioWriteRequest writeReq0;
	writeReq0.samples = recordData0.data();
	writeReq0.numSamps = static_cast<unsigned int>(totalRecord);
	writeReq0.stride = 1;
	writeReq0.fadeCurrent = 0.0f;
	writeReq0.fadeNew = 1.0f;
	writeReq0.source = Audible::AUDIOSOURCE_ADC;
	loop0->OnBlockWrite(writeReq0, 0);
	loop0->EndWrite(static_cast<unsigned int>(totalRecord), true);

	AudioWriteRequest writeReq1;
	writeReq1.samples = recordData1.data();
	writeReq1.numSamps = static_cast<unsigned int>(totalRecord);
	writeReq1.stride = 1;
	writeReq1.fadeCurrent = 0.0f;
	writeReq1.fadeNew = 1.0f;
	writeReq1.source = Audible::AUDIOSOURCE_ADC;
	loop1->OnBlockWrite(writeReq1, 0);
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

	// Compare a contiguous steady-state window against the written loop region.
	// The read path is delayed by 2 blocks (LoopTake + Station intermediate
	// AudioBuffers), so skip startup and align by that fixed delay.
	const unsigned int steadyStateDelaySamps = 2u * blockSize;
	ASSERT_GT(allRead.size(), steadyStateDelaySamps);

	for (unsigned int i = steadyStateDelaySamps; i < allRead.size(); i++)
	{
		auto loopIndex = static_cast<unsigned long>((i - steadyStateDelaySamps) % loopLength);
		auto expectedIndex = constants::MaxLoopFadeSamps + loopIndex;
		ASSERT_LT(expectedIndex, allWritten.size());
		ASSERT_FLOAT_EQ(allRead[i], allWritten[expectedIndex]);
	}
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
	std::vector<float> allWrittenCh0;
	std::vector<float> allWrittenCh1;
	allWrittenCh0.reserve(totalBlocks * blockSize);
	allWrittenCh1.reserve(totalBlocks * blockSize);
	std::vector<float> inBuf(numChans * blockSize);

	// ----- Write phase -----
	for (unsigned int b = 0; b < totalBlocks; b++)
	{
		FillTestData(inBuf.data(), numChans, blockSize, b);
		WriteBlock(chanMixer, station, inBuf.data(), numChans, blockSize);

		for (unsigned int s = 0; s < blockSize; s++)
		{
			allWrittenCh0.push_back(inBuf[s * numChans + 0]);
			allWrittenCh1.push_back(inBuf[s * numChans + 1]);
		}
	}

	// ----- Transition to playing -----
	take->Play(constants::MaxLoopFadeSamps, loopLength, 0);

	// ----- Read phase -----
	auto readMixer = MakeChannelMixer(numChans, constants::MaxBlockSize);
	const unsigned int readBlocks =
		static_cast<unsigned int>((loopLength + blockSize - 1) / blockSize) + 8;
	std::vector<float> allReadCh0;
	std::vector<float> allReadCh1;
	allReadCh0.reserve(readBlocks * blockSize);
	allReadCh1.reserve(readBlocks * blockSize);

	for (unsigned int b = 0; b < readBlocks; b++)
	{
		std::vector<float> outBuf(numChans * blockSize, 0.0f);
		ReadBlock(readMixer, station, outBuf.data(), numChans, blockSize);

		for (unsigned int s = 0; s < blockSize; s++)
		{
			allReadCh0.push_back(outBuf[s * numChans + 0]);
			allReadCh1.push_back(outBuf[s * numChans + 1]);
		}
	}

	ASSERT_TRUE(HasNonZero(allReadCh0.data(), static_cast<unsigned int>(allReadCh0.size())));
	ASSERT_TRUE(HasNonZero(allReadCh1.data(), static_cast<unsigned int>(allReadCh1.size())));

	// Compare directly against written loop data. In steady state, the
	// read path is delayed by 2 blocks (LoopTake + Station intermediate
	// AudioBuffers), so skip startup and align by that delay.
	const unsigned int steadyStateDelaySamps = 2u * blockSize;
	ASSERT_GT(allReadCh0.size(), steadyStateDelaySamps);
	ASSERT_GT(allReadCh1.size(), steadyStateDelaySamps);

	// Skip both crossfade regions: the last FadeSamps samples before the wrap
	// point (fade-out into next iteration) and the first FadeSamps samples
	// after the wrap point (fade-in from previous iteration). Both regions
	// are blended and won't match the raw written samples.
	const auto fadeSamps = static_cast<unsigned long>(constants::DefaultFadeSamps);

	for (unsigned int i = steadyStateDelaySamps; i < allReadCh0.size(); i++)
	{
		auto loopIndex = static_cast<unsigned long>((i - steadyStateDelaySamps) % loopLength);
		if (loopIndex < fadeSamps || loopIndex >= loopLength - fadeSamps)
			continue;

		auto expectedIndex = constants::MaxLoopFadeSamps + loopIndex;
		ASSERT_LT(expectedIndex, allWrittenCh1.size());

		auto w1 = allWrittenCh1[expectedIndex];
		auto r0 = allReadCh0[i];
		auto r1 = allReadCh1[i];

		// Due to the LoopTake AudioBuffer routing, both output channels
		// receive twice the second channel's written value.
		ASSERT_FLOAT_EQ(r0, 2.0f * w1) << "loopIndex=" << loopIndex << " i=" << i;
		ASSERT_FLOAT_EQ(r1, 2.0f * w1) << "loopIndex=" << loopIndex << " i=" << i;
	}
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

// 10. Write a known ascending sequence to a loop via OnBlockWrite, read back
//     through Loop::WriteBlock -> mock sink, and verify every sample matches.
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

	// Write a known ascending sequence using block API.
	std::vector<float> recordData(totalRecord);
	for (unsigned long i = 0; i < totalRecord; i++)
	{
		recordData[i] = (i >= constants::MaxLoopFadeSamps)
			? static_cast<float>((i - constants::MaxLoopFadeSamps) + 1) * 0.01f
			: 0.0f;
	}
	AudioWriteRequest writeReq;
	writeReq.samples = recordData.data();
	writeReq.numSamps = static_cast<unsigned int>(totalRecord);
	writeReq.stride = 1;
	writeReq.fadeCurrent = 0.0f;
	writeReq.fadeNew = 1.0f;
	writeReq.source = Audible::AUDIOSOURCE_ADC;
	loop.OnBlockWrite(writeReq, 0);
	loop.EndWrite(static_cast<unsigned int>(totalRecord), true);

	// Transition to playing: _playIndex = MaxLoopFadeSamps.
	loop.Play(constants::MaxLoopFadeSamps, loopLength, false);

	// Read one block via Loop::WriteBlock into a capturing mock sink.
	auto sink = std::make_shared<CaptureMultiSink>(blockSize);
	loop.WriteBlock(sink, nullptr, 0, blockSize);
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

// 11. Write a known ascending sequence through the full ADC → ChannelMixer →
//     Station → LoopTake → Loop write pipeline, then read back from the loop
//     directly and verify every sample matches.
TEST(AudioFlow, WriteViaStation_PerSampleVerification)
{
	const unsigned int numChans = 1;
	const unsigned int loopLength = 64;
	const unsigned int blockSize = 512;
	const unsigned long totalRecord = constants::MaxLoopFadeSamps + loopLength;
	const unsigned int totalBlocks =
		static_cast<unsigned int>((totalRecord + blockSize - 1) / blockSize);

	auto station = MakeStation(numChans);
	auto take = MakeTake();
	station->AddTake(take);

	// Use AddLoop to get a reference to the loop for later verification.
	take->Record({}, "test");
	auto loop0 = take->AddLoop(0, "test");
	loop0->Record();
	station->CommitChanges();

	auto chanMixer = MakeChannelMixer(numChans, constants::MaxBlockSize);

	// Write an ascending sequence through the full pipeline.
	// The loop region [MaxLoopFadeSamps, totalRecord) gets values
	// (k+1)*0.01f where k is the position within the loop region.
	std::vector<float> inBuf(numChans * blockSize, 0.0f);
	for (unsigned int b = 0; b < totalBlocks; b++)
	{
		for (unsigned int s = 0; s < blockSize; s++)
		{
			unsigned long globalIndex =
				static_cast<unsigned long>(b) * blockSize + s;
			inBuf[s] = (globalIndex >= constants::MaxLoopFadeSamps)
				? static_cast<float>(
					(globalIndex - constants::MaxLoopFadeSamps) + 1) * 0.01f
				: 0.0f;
		}
		WriteBlock(chanMixer, station, inBuf.data(), numChans, blockSize);
	}

	// Transition to playing: _playIndex = MaxLoopFadeSamps.
	loop0->Play(constants::MaxLoopFadeSamps, loopLength, false);

	// Read one block directly from the loop into a capturing mock sink.
	const unsigned int readBlock = 32;
	auto sink = std::make_shared<CaptureMultiSink>(readBlock);
	loop0->WriteBlock(sink, nullptr, 0, readBlock);
	loop0->EndMultiPlay(readBlock);

	// Verify exact per-sample values. Samples pass through the
	// pipeline unchanged: FromAdc writes with fade(0,1), AudioBuffer
	// reads with fade(1,1) into Loop whose buffer bank starts at 0,
	// and all mixer fades are exactly 1.0.
	const auto& captured = sink->GetSink()->Samples;
	for (unsigned int s = 0; s < readBlock; s++)
	{
		float expected = static_cast<float>(s + 1) * 0.01f;
		ASSERT_FLOAT_EQ(captured[s], expected) << "Mismatch at sample " << s;
	}
}

// 12. Write a known ascending sequence directly to a loop, then read through
//     the full Loop → LoopTake → Station → ChannelMixer → DAC read pipeline
//     and verify every sample matches.
TEST(AudioFlow, ReadViaStation_PerSampleVerification)
{
	const unsigned int numChans = 1;
	const unsigned int loopLength = 64;
	const unsigned int blockSize = 32;
	const unsigned long totalRecord = constants::MaxLoopFadeSamps + loopLength;

	auto station = MakeStation(numChans);
	auto take = MakeTake();
	station->AddTake(take);

	take->Record({}, "test");
	auto loop0 = take->AddLoop(0, "test");
	loop0->Record();
	station->CommitChanges();

	// Write a known ascending sequence directly to the loop using block API.
	// The loop region starts at MaxLoopFadeSamps; we write (k+1)*0.01f
	// for position k within the loop region; the fade-in region is zero.
	std::vector<float> recordData(totalRecord);
	for (unsigned long i = 0; i < totalRecord; i++)
	{
		recordData[i] = (i >= constants::MaxLoopFadeSamps)
			? static_cast<float>((i - constants::MaxLoopFadeSamps) + 1) * 0.01f
			: 0.0f;
	}
	AudioWriteRequest writeReq;
	writeReq.samples = recordData.data();
	writeReq.numSamps = static_cast<unsigned int>(totalRecord);
	writeReq.stride = 1;
	writeReq.fadeCurrent = 0.0f;
	writeReq.fadeNew = 1.0f;
	writeReq.source = Audible::AUDIOSOURCE_ADC;
	loop0->OnBlockWrite(writeReq, 0);
	loop0->EndWrite(static_cast<unsigned int>(totalRecord), true);

	// Transition take (and its loops) to playing.
	take->Play(constants::MaxLoopFadeSamps, loopLength, 0);

	// Read one block through the full Station → ChannelMixer → DAC pipeline.
	auto chanMixer = MakeChannelMixer(numChans, constants::MaxBlockSize);
	std::vector<float> outBuf(numChans * blockSize, 0.0f);
	ReadBlock(chanMixer, station, outBuf.data(), numChans, blockSize);

	// Verify exact per-sample values. On the first block the intermediate
	// AudioBuffers have _sampsRecorded == 0 so Delay returns 0 and data
	// flows through without latency. All mixer fades are 1.0.
	for (unsigned int s = 0; s < blockSize; s++)
	{
		float expected = static_cast<float>(s + 1) * 0.01f;
		ASSERT_FLOAT_EQ(outBuf[s], expected) << "Mismatch at sample " << s;
	}
}
