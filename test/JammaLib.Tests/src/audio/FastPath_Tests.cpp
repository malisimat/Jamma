#include "gtest/gtest.h"
#include "audio/AudioMixer.h"
#include "audio/AudioBuffer.h"
#include "audio/BufferBank.h"
#include "audio/InterpolatedValue.h"
#include "engine/Loop.h"
#include "base/AudioSink.h"

using audio::AudioMixer;
using audio::AudioMixerParams;
using audio::AudioBuffer;
using audio::BufferBank;
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

// --- Mocked sink for capturing per-channel output ---

class FastPathMockedSink :
	public AudioSink
{
public:
	FastPathMockedSink(unsigned int bufSize) :
		Samples(bufSize, 0.0f)
	{
	}

public:
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

class FastPathMockedMultiSink :
	public MultiAudioSink
{
public:
	FastPathMockedMultiSink(unsigned int numChans, unsigned int bufSize)
	{
		for (auto i = 0u; i < numChans; i++)
			_sinks.push_back(std::make_shared<FastPathMockedSink>(bufSize));
	}

public:
	virtual unsigned int NumInputChannels(base::Audible::AudioSourceType source) const override
	{
		return (unsigned int)_sinks.size();
	}

	const std::shared_ptr<FastPathMockedSink>& GetSink(unsigned int chan) const
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
	std::vector<std::shared_ptr<FastPathMockedSink>> _sinks;
};

// --- InterpolatedValue IsSettled tests ---

TEST(FastPath, ExpFadeIsSettledAfterJump) {
	InterpolatedValueExp::ExponentialParams params;
	params.Damping = 100.0;
	auto fade = InterpolatedValueExp(params);

	fade.Jump(1.0);
	ASSERT_TRUE(fade.IsSettled());
}

TEST(FastPath, ExpFadeIsNotSettledDuringRamp) {
	InterpolatedValueExp::ExponentialParams params;
	params.Damping = 100.0;
	auto fade = InterpolatedValueExp(params);

	fade.Jump(0.0);
	fade.SetTarget(1.0);
	fade.Next();
	ASSERT_FALSE(fade.IsSettled());
}

TEST(FastPath, ExpFadeSettlesAfterManySamples) {
	InterpolatedValueExp::ExponentialParams params;
	params.Damping = 100.0;
	auto fade = InterpolatedValueExp(params);

	fade.Jump(0.0);
	fade.SetTarget(1.0);

	for (auto i = 0; i < 10000; i++)
		fade.Next();

	ASSERT_TRUE(fade.IsSettled());
}

// --- AudioMixer WriteBlock tests ---

TEST(FastPath, WireMixerWriteBlock) {
	WireMixBehaviourParams wire;
	wire.Channels = { 0 };

	AudioMixerParams mixerParams;
	mixerParams.Size = { 110, 80 };
	mixerParams.Position = { 6, 6 };
	mixerParams.Behaviour = wire;

	auto mixer = AudioMixer(mixerParams);
	auto dest = std::make_shared<FastPathMockedMultiSink>(1, 64);

	float src[] = { 0.5f, 0.25f };
	mixer.WriteBlock(dest, src, 2);

	auto sink = dest->GetSink(0);
	ASSERT_FLOAT_EQ(sink->Samples[0], 0.5f);
	ASSERT_FLOAT_EQ(sink->Samples[1], 0.25f);
}

// --- BufferBank block access tests ---

TEST(FastPath, BufferBankIsBlockContiguous) {
	BufferBank bank;
	bank.Resize(2000);
	ASSERT_TRUE(bank.IsBlockContiguous(0, 512));
	ASSERT_TRUE(bank.IsBlockContiguous(100, 256));
}

TEST(FastPath, BufferBankBlockPtrReturnsData) {
	BufferBank bank;
	bank.Resize(2000);

	bank[0] = 0.5f;
	bank[1] = 0.75f;

	auto ptr = bank.BlockPtr(0);
	ASSERT_NE(ptr, nullptr);
	ASSERT_FLOAT_EQ(ptr[0], 0.5f);
	ASSERT_FLOAT_EQ(ptr[1], 0.75f);
}

// --- AudioBuffer block read/write tests ---

TEST(FastPath, AudioBufferIsContiguous) {
	auto buf = AudioBuffer(100);
	ASSERT_TRUE(buf.IsContiguous(0, 50));
	ASSERT_TRUE(buf.IsContiguous(0, 100));
	ASSERT_FALSE(buf.IsContiguous(50, 60));
}

TEST(FastPath, AudioBufferBlockReadReturnsData) {
	auto buf = std::make_shared<AudioBuffer>(100);

	std::vector<float> data(10);
	for (auto i = 0u; i < 10; i++)
		data[i] = (float)(i + 1) * 0.1f;

	base::AudioWriteRequest request;
	request.samples = data.data();
	request.numSamps = 10;
	request.stride = 1;
	request.fadeCurrent = 0.0f;
	request.fadeNew = 1.0f;
	request.source = base::Audible::AUDIOSOURCE_ADC;

	buf->OnBlockWrite(request, 0);
	buf->EndWrite(10, true);

	auto playIdx = buf->Delay(0);
	auto ptr = buf->BlockRead(playIdx);
	ASSERT_NE(ptr, nullptr);
}

TEST(FastPath, AudioBufferOnBlockWriteProducesCorrectOutput) {
	auto bufSize = 100u;
	auto numSamps = 32u;

	std::vector<float> src(numSamps);
	for (auto i = 0u; i < numSamps; i++)
		src[i] = ((float)i + 1.0f) * 0.03f;

	float fadeLevel = 0.8f;

	// Block path
	auto buf = std::make_shared<AudioBuffer>(bufSize);
	AudioWriteRequest request;
	request.samples = src.data();
	request.numSamps = numSamps;
	request.stride = 1;
	request.fadeCurrent = 0.0f;
	request.fadeNew = fadeLevel;
	request.source = base::Audible::AUDIOSOURCE_MIXER;

	buf->OnBlockWrite(request, 0);
	buf->EndWrite(numSamps, true);

	for (auto i = 0u; i < numSamps; i++)
	{
		ASSERT_FLOAT_EQ((*buf)[i], src[i] * fadeLevel)
			<< "Mismatch at index " << i;
	}
}

// --- AudioMixer WriteBlock produces correct output ---

TEST(FastPath, MixerWriteBlockProducesCorrectOutput) {
	auto bufSize = 64u;
	auto numSamps = 32u;

	WireMixBehaviourParams wire;
	wire.Channels = { 0 };

	AudioMixerParams mixerParams;
	mixerParams.Size = { 110, 80 };
	mixerParams.Position = { 6, 6 };
	mixerParams.Behaviour = wire;

	std::vector<float> src(numSamps);
	for (auto i = 0u; i < numSamps; i++)
		src[i] = ((float)i + 1.0f) * 0.02f;

	// Block path via WriteBlock
	auto mixer = AudioMixer(mixerParams);
	auto dest = std::make_shared<FastPathMockedMultiSink>(1, bufSize);
	mixer.WriteBlock(dest, src.data(), numSamps);

	dest->EndMultiWrite(numSamps, true, base::Audible::AUDIOSOURCE_MIXER);

	auto sink = dest->GetSink(0);
	for (auto i = 0u; i < numSamps; i++)
	{
		ASSERT_FLOAT_EQ(sink->Samples[i], src[i])
			<< "Mismatch at sample " << i;
	}
}

// --- Loop::WriteBlock fast path produces identical output ---

TEST(FastPath, LoopWriteBlockMatchesFallback) {
	auto bufSize = 100u;
	auto blockSize = 32u;
	auto loopLength = 50ul;
	auto totalRecordSamps = constants::MaxLoopFadeSamps + loopLength;
	auto renderedSamps = (bufSize / blockSize) * blockSize;

	WireMixBehaviourParams fastMixBehaviour;
	fastMixBehaviour.Channels = { 0 };

	PanMixBehaviourParams fallbackMixBehaviour;
	fallbackMixBehaviour.ChannelLevels = { 1.0f };

	AudioMixerParams fastMixerParams;
	fastMixerParams.Size = { 160, 320 };
	fastMixerParams.Position = { 6, 6 };
	fastMixerParams.Behaviour = fastMixBehaviour;

	AudioMixerParams fallbackMixerParams;
	fallbackMixerParams.Size = { 160, 320 };
	fallbackMixerParams.Position = { 6, 6 };
	fallbackMixerParams.Behaviour = fallbackMixBehaviour;

	LoopParams loopParams;
	loopParams.Wav = "hh";
	loopParams.Size = { 80, 80 };
	loopParams.Position = { 10, 22 };

	auto loopA = Loop(loopParams, fastMixerParams);
	auto loopB = Loop(loopParams, fallbackMixerParams);

	loopA.Record();
	loopB.Record();

	std::vector<float> recordData((unsigned int)totalRecordSamps, 0.5f);
	AudioWriteRequest recordReq;
	recordReq.samples = recordData.data();
	recordReq.numSamps = (unsigned int)totalRecordSamps;
	recordReq.stride = 1;
	recordReq.fadeCurrent = 0.0f;
	recordReq.fadeNew = 1.0f;
	recordReq.source = base::Audible::AUDIOSOURCE_ADC;

	loopA.OnBlockWrite(recordReq, 0);
	loopB.OnBlockWrite(recordReq, 0);

	loopA.EndWrite(totalRecordSamps, true);
	loopB.EndWrite(totalRecordSamps, true);
	loopA.Play(constants::MaxLoopFadeSamps, loopLength, false);
	loopB.Play(constants::MaxLoopFadeSamps, loopLength, false);

	auto destA = std::make_shared<FastPathMockedMultiSink>(1, bufSize);
	auto destB = std::make_shared<FastPathMockedMultiSink>(1, bufSize);
	auto numBlocks = bufSize / blockSize;

	for (auto i = 0u; i < numBlocks; i++)
	{
		destA->Zero(blockSize, base::Audible::AUDIOSOURCE_MIXER);
		destB->Zero(blockSize, base::Audible::AUDIOSOURCE_MIXER);
		loopA.WriteBlock(destA, std::shared_ptr<engine::Trigger>(), 0u, blockSize);
		loopB.WriteBlock(destB, std::shared_ptr<engine::Trigger>(), 0u, blockSize);
		loopA.EndMultiPlay(blockSize);
		loopB.EndMultiPlay(blockSize);
		destA->EndMultiWrite(blockSize, true, base::Audible::AUDIOSOURCE_MIXER);
		destB->EndMultiWrite(blockSize, true, base::Audible::AUDIOSOURCE_MIXER);
	}

	auto sinkA = destA->GetSink(0);
	auto sinkB = destB->GetSink(0);
	bool hasNonZero = false;
	for (auto i = 0u; i < renderedSamps; i++)
	{
		if (sinkA->Samples[i] != 0.0f)
		{
			hasNonZero = true;
			break;
		}
	}
	ASSERT_TRUE(hasNonZero);

	for (auto i = 0u; i < renderedSamps; i++)
	{
		ASSERT_FLOAT_EQ(sinkA->Samples[i], sinkB->Samples[i])
			<< "Mismatch at sample " << i;
	}

	for (auto i = renderedSamps; i < bufSize; i++)
	{
		ASSERT_FLOAT_EQ(sinkA->Samples[i], 0.0f)
			<< "Fast-path tail sample should be untouched at index " << i;
		ASSERT_FLOAT_EQ(sinkB->Samples[i], 0.0f)
			<< "Fallback tail sample should be untouched at index " << i;
	}
}
