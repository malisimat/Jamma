#include "gtest/gtest.h"
#include "audio/AudioBuffer.h"
#include "audio/AudioMixer.h"
#include "audio/BufferBank.h"
#include "audio/ChannelMixer.h"
#include "audio/InterpolatedValue.h"
#include "engine/Loop.h"

using audio::AudioBuffer;
using audio::AudioMixer;
using audio::AudioMixerParams;
using audio::BufferBank;
using audio::ChannelMixer;
using audio::ChannelMixerParams;
using audio::WireMixBehaviourParams;
using audio::PanMixBehaviourParams;
using audio::BounceMixBehaviourParams;
using audio::MergeMixBehaviourParams;
using audio::InterpolatedValueExp;
using engine::Loop;
using engine::LoopParams;
using base::AudioSink;
using base::MultiAudioSink;
using base::AudioWriteRequest;

// ---------------------------------------------------------------
// Mock sinks using the new block API
// ---------------------------------------------------------------

class MockedSink :
	public AudioSink
{
public:
	MockedSink(unsigned int bufSize) :
		Samples(bufSize, 0.0f)
	{
	}

	virtual void OnBlockWrite(const AudioWriteRequest& request, int writeOffset) override
	{
		for (auto i = 0u; i < request.numSamps; i++)
		{
			auto destIndex = _writeIndex + writeOffset + i;
			if (destIndex < Samples.size())
				Samples[destIndex] = (request.fadeNew * request.samples[i * request.stride]) +
					(request.fadeCurrent * Samples[destIndex]);
		}
	}

	virtual void EndWrite(unsigned int numSamps, bool updateIndex) override
	{
		if (updateIndex)
			_writeIndex += numSamps;
	}

	std::vector<float> Samples;
};

class MockedMultiSink :
	public MultiAudioSink
{
public:
	MockedMultiSink(unsigned int numChans, unsigned int bufSize)
	{
		for (auto i = 0u; i < numChans; i++)
			_sinks.push_back(std::make_shared<MockedSink>(bufSize));
	}

	virtual unsigned int NumInputChannels(base::Audible::AudioSourceType source) const override
	{
		return (unsigned int)_sinks.size();
	}

	const std::shared_ptr<MockedSink>& GetSink(unsigned int chan) const
	{
		return _sinks[chan];
	}

protected:
	virtual const std::shared_ptr<AudioSink> _InputChannel(unsigned int channel,
		base::Audible::AudioSourceType source) override
	{
		if (channel < _sinks.size())
			return _sinks[channel];
		return std::shared_ptr<AudioSink>();
	}

private:
	std::vector<std::shared_ptr<MockedSink>> _sinks;
};

// ---------------------------------------------------------------
// AudioBuffer block write tests
// ---------------------------------------------------------------

TEST(BlockApi, AudioBufferBlockWrite) {
	auto bufSize = 64u;
	auto numSamps = 32u;

	auto buf = std::make_shared<AudioBuffer>(bufSize);

	std::vector<float> data(numSamps);
	for (auto i = 0u; i < numSamps; i++)
		data[i] = (float)(i + 1) * 0.1f;

	AudioWriteRequest request;
	request.samples = data.data();
	request.numSamps = numSamps;
	request.stride = 1;
	request.fadeCurrent = 0.0f;
	request.fadeNew = 1.0f;
	request.source = base::Audible::AUDIOSOURCE_ADC;

	buf->OnBlockWrite(request, 0);
	buf->EndWrite(numSamps, true);

	for (auto i = 0u; i < numSamps; i++)
	{
		ASSERT_FLOAT_EQ((*buf)[i], data[i])
			<< "Mismatch at index " << i;
	}
}

TEST(BlockApi, AudioBufferStridedBlockWrite) {
	auto bufSize = 64u;
	auto numChannels = 2u;
	auto numSamps = 16u;

	// Interleaved stereo: [L0, R0, L1, R1, ...]
	std::vector<float> interleaved(numSamps * numChannels);
	for (auto i = 0u; i < numSamps; i++)
	{
		interleaved[i * numChannels + 0] = (float)i * 0.01f;
		interleaved[i * numChannels + 1] = (float)i * -0.01f;
	}

	auto buf0 = std::make_shared<AudioBuffer>(bufSize);
	AudioWriteRequest req;
	req.samples = &interleaved[0];
	req.numSamps = numSamps;
	req.stride = numChannels;
	req.fadeCurrent = 0.0f;
	req.fadeNew = 1.0f;
	req.source = base::Audible::AUDIOSOURCE_ADC;
	buf0->OnBlockWrite(req, 0);
	buf0->EndWrite(numSamps, true);

	auto buf1 = std::make_shared<AudioBuffer>(bufSize);
	req.samples = &interleaved[1];
	buf1->OnBlockWrite(req, 0);
	buf1->EndWrite(numSamps, true);

	for (auto i = 0u; i < numSamps; i++)
	{
		ASSERT_FLOAT_EQ((*buf0)[i], (float)i * 0.01f);
		ASSERT_FLOAT_EQ((*buf1)[i], (float)i * -0.01f);
	}
}

TEST(BlockApi, AudioBufferBlockWriteWithFade) {
	auto bufSize = 64u;
	auto numSamps = 16u;

	auto buf = std::make_shared<AudioBuffer>(bufSize);

	// Write initial data (all 1.0)
	std::vector<float> initial(numSamps, 1.0f);
	AudioWriteRequest req1;
	req1.samples = initial.data();
	req1.numSamps = numSamps;
	req1.stride = 1;
	req1.fadeCurrent = 0.0f;
	req1.fadeNew = 1.0f;
	req1.source = base::Audible::AUDIOSOURCE_ADC;
	buf->OnBlockWrite(req1, 0);

	// Overwrite with fade: 0.5 * existing + 0.5 * new(0.0) = 0.5
	std::vector<float> overlay(numSamps, 0.0f);
	AudioWriteRequest req2;
	req2.samples = overlay.data();
	req2.numSamps = numSamps;
	req2.stride = 1;
	req2.fadeCurrent = 0.5f;
	req2.fadeNew = 0.5f;
	req2.source = base::Audible::AUDIOSOURCE_ADC;
	buf->OnBlockWrite(req2, 0);

	for (auto i = 0u; i < numSamps; i++)
	{
		ASSERT_FLOAT_EQ((*buf)[i], 0.5f);
	}
}

TEST(BlockApi, AudioBufferBlockReadContiguous) {
	auto bufSize = 64u;
	auto numSamps = 16u;

	auto buf = std::make_shared<AudioBuffer>(bufSize);

	std::vector<float> data(numSamps);
	for (auto i = 0u; i < numSamps; i++)
		data[i] = (float)(i + 1) * 0.05f;

	AudioWriteRequest request;
	request.samples = data.data();
	request.numSamps = numSamps;
	request.stride = 1;
	request.fadeCurrent = 0.0f;
	request.fadeNew = 1.0f;
	request.source = base::Audible::AUDIOSOURCE_ADC;
	buf->OnBlockWrite(request, 0);
	buf->EndWrite(numSamps, true);

	auto playIndex = buf->Delay(numSamps);
	ASSERT_TRUE(buf->IsContiguous(playIndex, numSamps));

	auto ptr = buf->BlockRead(playIndex);
	ASSERT_NE(ptr, nullptr);

	for (auto i = 0u; i < numSamps; i++)
	{
		ASSERT_FLOAT_EQ(ptr[i], data[i]);
	}
}

TEST(BlockApi, AudioBufferPlaybackRead) {
	auto bufSize = 64u;
	auto numSamps = 16u;

	auto buf = std::make_shared<AudioBuffer>(bufSize);

	std::vector<float> data(numSamps);
	for (auto i = 0u; i < numSamps; i++)
		data[i] = (float)(i + 1) * 0.05f;

	AudioWriteRequest request;
	request.samples = data.data();
	request.numSamps = numSamps;
	request.stride = 1;
	request.fadeCurrent = 0.0f;
	request.fadeNew = 1.0f;
	request.source = base::Audible::AUDIOSOURCE_ADC;
	buf->OnBlockWrite(request, 0);
	buf->EndWrite(numSamps, true);

	// PlaybackRead should return pointer to contiguous data
	float tempBuf[constants::MaxBlockSize];
	buf->Delay(numSamps);
	auto ptr = buf->PlaybackRead(tempBuf, numSamps);
	ASSERT_NE(ptr, nullptr);

	for (auto i = 0u; i < numSamps; i++)
	{
		ASSERT_FLOAT_EQ(ptr[i], data[i])
			<< "PlaybackRead mismatch at index " << i;
	}
}

// ---------------------------------------------------------------
// AudioMixer::WriteBlock tests
// ---------------------------------------------------------------

TEST(BlockApi, WireMixerWriteBlock) {
	auto numSamps = 32u;

	WireMixBehaviourParams wire;
	wire.Channels = { 0 };

	AudioMixerParams mixerParams;
	mixerParams.Size = { 110, 80 };
	mixerParams.Position = { 6, 6 };
	mixerParams.Behaviour = wire;

	auto mixer = AudioMixer(mixerParams);
	auto dest = std::make_shared<MockedMultiSink>(1, numSamps);

	std::vector<float> src(numSamps);
	for (auto i = 0u; i < numSamps; i++)
		src[i] = (float)(i + 1) * 0.02f;

	mixer.WriteBlock(dest, src.data(), numSamps);
	dest->EndMultiWrite(numSamps, true, base::Audible::AUDIOSOURCE_MIXER);

	auto sink = dest->GetSink(0);
	for (auto i = 0u; i < numSamps; i++)
	{
		ASSERT_FLOAT_EQ(sink->Samples[i], src[i])
			<< "Mismatch at sample " << i;
	}
}

TEST(BlockApi, PanMixerWriteBlock) {
	auto numSamps = 16u;

	PanMixBehaviourParams pan;
	pan.ChannelLevels = { 0.5f, 0.5f };

	AudioMixerParams mixerParams;
	mixerParams.Size = { 110, 80 };
	mixerParams.Position = { 6, 6 };
	mixerParams.Behaviour = pan;

	auto mixer = AudioMixer(mixerParams);
	auto dest = std::make_shared<MockedMultiSink>(2, numSamps);

	std::vector<float> src(numSamps, 1.0f);

	mixer.WriteBlock(dest, src.data(), numSamps);
	dest->EndMultiWrite(numSamps, true, base::Audible::AUDIOSOURCE_MIXER);

	// Pan with 0.5 level on each channel: fadeNew = 1.0 * 0.5 = 0.5
	auto ch0 = dest->GetSink(0);
	auto ch1 = dest->GetSink(1);
	for (auto i = 0u; i < numSamps; i++)
	{
		ASSERT_FLOAT_EQ(ch0->Samples[i], 0.5f);
		ASSERT_FLOAT_EQ(ch1->Samples[i], 0.5f);
	}
}

// ---------------------------------------------------------------
// Loop block write and read tests
// ---------------------------------------------------------------

TEST(BlockApi, LoopBlockWriteAndRead) {
	auto blockSize = 32u;
	auto loopLength = (unsigned long)blockSize;
	auto totalRecordSamps = constants::MaxLoopFadeSamps + loopLength;

	WireMixBehaviourParams mixBehaviour;
	mixBehaviour.Channels = { 0 };

	AudioMixerParams mixerParams;
	mixerParams.Size = { 160, 320 };
	mixerParams.Position = { 6, 6 };
	mixerParams.Behaviour = mixBehaviour;

	LoopParams loopParams;
	loopParams.Wav = "test";
	loopParams.Size = { 80, 80 };
	loopParams.Position = { 10, 22 };

	auto loop = Loop(loopParams, mixerParams);
	loop.Record();

	// Write data using OnBlockWrite
	std::vector<float> data(totalRecordSamps, 0.5f);
	AudioWriteRequest writeReq;
	writeReq.samples = data.data();
	writeReq.numSamps = totalRecordSamps;
	writeReq.stride = 1;
	writeReq.fadeCurrent = 0.0f;
	writeReq.fadeNew = 1.0f;
	writeReq.source = base::Audible::AUDIOSOURCE_ADC;
	loop.OnBlockWrite(writeReq, 0);
	loop.EndWrite(totalRecordSamps, true);

	loop.Play(constants::MaxLoopFadeSamps, loopLength, false);

	// Read data using WriteBlock
	auto dest = std::make_shared<MockedMultiSink>(1, blockSize);
	dest->Zero(blockSize, base::Audible::AUDIOSOURCE_MIXER);
	loop.WriteBlock(dest, std::shared_ptr<engine::Trigger>(), 0u, blockSize);
	loop.EndMultiPlay(blockSize);
	dest->EndMultiWrite(blockSize, true, base::Audible::AUDIOSOURCE_MIXER);

	// Verify all samples are 0.5
	auto sink = dest->GetSink(0);
	bool hasNonZero = false;
	for (auto i = 0u; i < blockSize; i++)
	{
		if (sink->Samples[i] != 0.0f)
			hasNonZero = true;
	}
	ASSERT_TRUE(hasNonZero);

	for (auto i = 0u; i < blockSize; i++)
	{
		ASSERT_FLOAT_EQ(sink->Samples[i], 0.5f)
			<< "Unexpected value at sample " << i;
	}
}

TEST(BlockApi, LoopReadBlock) {
	auto blockSize = 32u;
	auto loopLength = 64ul;
	auto totalRecordSamps = constants::MaxLoopFadeSamps + loopLength;

	WireMixBehaviourParams mixBehaviour;
	mixBehaviour.Channels = { 0 };

	AudioMixerParams mixerParams;
	mixerParams.Size = { 160, 320 };
	mixerParams.Position = { 6, 6 };
	mixerParams.Behaviour = mixBehaviour;

	LoopParams loopParams;
	loopParams.Wav = "test";
	loopParams.Size = { 80, 80 };
	loopParams.Position = { 10, 22 };

	auto loop = Loop(loopParams, mixerParams);
	loop.Record();

	// Write known data to Loop via OnBlockWrite
	std::vector<float> data(totalRecordSamps);
	for (auto i = 0u; i < totalRecordSamps; i++)
		data[i] = (float)(i + 1) * 0.001f;

	AudioWriteRequest writeReq;
	writeReq.samples = data.data();
	writeReq.numSamps = totalRecordSamps;
	writeReq.stride = 1;
	writeReq.fadeCurrent = 0.0f;
	writeReq.fadeNew = 1.0f;
	writeReq.source = base::Audible::AUDIOSOURCE_ADC;
	loop.OnBlockWrite(writeReq, 0);
	loop.EndWrite(totalRecordSamps, true);

	loop.Play(constants::MaxLoopFadeSamps, loopLength, false);

	// ReadBlock — pure data read, no destination parameter
	float readBuf[constants::MaxBlockSize] = {};
	auto sampsRead = loop.ReadBlock(readBuf, 0, blockSize);

	ASSERT_EQ(sampsRead, blockSize);

	// Verify ReadBlock returns the recorded data (starting at MaxLoopFadeSamps)
	for (auto i = 0u; i < blockSize; i++)
	{
		auto expected = data[constants::MaxLoopFadeSamps + i];
		ASSERT_FLOAT_EQ(readBuf[i], expected)
			<< "ReadBlock mismatch at sample " << i;
	}
}

// ---------------------------------------------------------------
// ChannelMixer write path test
// ---------------------------------------------------------------

TEST(BlockApi, ChannelMixerFromAdcUsesBlockWrite) {
	auto bufSize = 256u;
	auto numChannels = 2u;
	auto numSamps = 64u;

	ChannelMixerParams params;
	params.InputBufferSize = bufSize;
	params.OutputBufferSize = bufSize;
	params.NumInputChannels = numChannels;
	params.NumOutputChannels = numChannels;

	auto chanMixer = ChannelMixer(params);

	// Create interleaved stereo input
	std::vector<float> input(numSamps * numChannels);
	for (auto i = 0u; i < numSamps; i++)
	{
		input[i * numChannels + 0] = (float)i * 0.01f;
		input[i * numChannels + 1] = (float)i * -0.01f;
	}

	chanMixer.FromAdc(input.data(), numChannels, numSamps);

	// Verify channel 0
	auto ch0 = chanMixer.Source()->NumOutputChannels(base::Audible::AUDIOSOURCE_ADC);
	ASSERT_GE(ch0, 2u);
}

// ---------------------------------------------------------------
// BufferBank block access tests
// ---------------------------------------------------------------

TEST(BlockApi, BufferBankBlockAccess) {
	BufferBank bank;
	bank.Resize(2000);

	bank[0] = 0.5f;
	bank[1] = 0.75f;

	ASSERT_TRUE(bank.IsBlockContiguous(0, 512));

	auto ptr = bank.BlockPtr(0);
	ASSERT_NE(ptr, nullptr);
	ASSERT_FLOAT_EQ(ptr[0], 0.5f);
	ASSERT_FLOAT_EQ(ptr[1], 0.75f);
}

// ---------------------------------------------------------------
// InterpolatedValue IsSettled tests
// ---------------------------------------------------------------

TEST(BlockApi, FadeIsSettledAfterJump) {
	InterpolatedValueExp::ExponentialParams params;
	params.Damping = 100.0;
	auto fade = InterpolatedValueExp(params);

	fade.Jump(1.0);
	ASSERT_TRUE(fade.IsSettled());
}

TEST(BlockApi, FadeIsNotSettledDuringRamp) {
	InterpolatedValueExp::ExponentialParams params;
	params.Damping = 100.0;
	auto fade = InterpolatedValueExp(params);

	fade.Jump(0.0);
	fade.SetTarget(1.0);
	fade.Next();
	ASSERT_FALSE(fade.IsSettled());
}

// ---------------------------------------------------------------
// MultiAudioSink::OnBlockWriteChannel test
// ---------------------------------------------------------------

TEST(BlockApi, MultiSinkBlockWriteChannel) {
	auto bufSize = 64u;
	auto numSamps = 16u;

	auto dest = std::make_shared<MockedMultiSink>(2, bufSize);

	std::vector<float> ch0Data(numSamps);
	for (auto i = 0u; i < numSamps; i++)
		ch0Data[i] = (float)i * 0.01f;

	AudioWriteRequest req0;
	req0.samples = ch0Data.data();
	req0.numSamps = numSamps;
	req0.stride = 1;
	req0.fadeCurrent = 0.0f;
	req0.fadeNew = 1.0f;
	req0.source = base::Audible::AUDIOSOURCE_MIXER;
	dest->OnBlockWriteChannel(0, req0, 0);

	std::vector<float> ch1Data(numSamps);
	for (auto i = 0u; i < numSamps; i++)
		ch1Data[i] = (float)i * -0.01f;

	AudioWriteRequest req1;
	req1.samples = ch1Data.data();
	req1.numSamps = numSamps;
	req1.stride = 1;
	req1.fadeCurrent = 0.0f;
	req1.fadeNew = 1.0f;
	req1.source = base::Audible::AUDIOSOURCE_MIXER;
	dest->OnBlockWriteChannel(1, req1, 0);

	auto ch0 = dest->GetSink(0);
	auto ch1 = dest->GetSink(1);
	for (auto i = 0u; i < numSamps; i++)
	{
		ASSERT_FLOAT_EQ(ch0->Samples[i], (float)i * 0.01f);
		ASSERT_FLOAT_EQ(ch1->Samples[i], (float)i * -0.01f);
	}
}
