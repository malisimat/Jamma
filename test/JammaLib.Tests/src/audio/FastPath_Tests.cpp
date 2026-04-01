#include "gtest/gtest.h"
#include "audio/AudioMixer.h"
#include "audio/AudioBuffer.h"
#include "audio/BufferBank.h"
#include "audio/InterpolatedValue.h"
#include "engine/Loop.h"

using audio::AudioMixer;
using audio::AudioMixerParams;
using audio::AudioBuffer;
using audio::BufferBank;
using audio::WireMixBehaviourParams;
using audio::PanMixBehaviourParams;
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
	inline virtual int OnMixWrite(float samp,
		float fadeCurrent,
		float fadeNew,
		int indexOffset,
		base::Audible::AudioSourceType source) override
	{
		auto destIndex = _writeIndex + indexOffset;
		if (destIndex < Samples.size())
			Samples[destIndex] = (fadeNew * samp) + (fadeCurrent * Samples[destIndex]);

		return indexOffset + 1;
	};
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

// --- AudioMixer block eligibility tests ---

TEST(FastPath, WireMixerIsBlockEligible) {
	WireMixBehaviourParams wire;
	wire.Channels = { 0 };

	AudioMixerParams mixerParams;
	mixerParams.Size = { 110, 80 };
	mixerParams.Position = { 6, 6 };
	mixerParams.Behaviour = wire;

	auto mixer = AudioMixer(mixerParams);
	ASSERT_TRUE(mixer.IsBlockEligible());
}

TEST(FastPath, PanMixerIsNotBlockEligible) {
	PanMixBehaviourParams pan;
	pan.ChannelLevels = { 0.5f, 0.5f };

	AudioMixerParams mixerParams;
	mixerParams.Size = { 110, 80 };
	mixerParams.Position = { 6, 6 };
	mixerParams.Behaviour = pan;

	auto mixer = AudioMixer(mixerParams);
	ASSERT_FALSE(mixer.IsBlockEligible());
}

TEST(FastPath, MergeMixerIsNotBlockEligible) {
	MergeMixBehaviourParams merge;
	merge.Channels = { 0 };

	AudioMixerParams mixerParams;
	mixerParams.Size = { 110, 80 };
	mixerParams.Position = { 6, 6 };
	mixerParams.Behaviour = merge;

	auto mixer = AudioMixer(mixerParams);
	ASSERT_FALSE(mixer.IsBlockEligible());
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

	for (auto i = 0u; i < 10; i++)
		buf->OnMixWrite((float)(i + 1) * 0.1f, 0.0f, 1.0f, (int)i, base::Audible::AUDIOSOURCE_ADC);
	buf->EndWrite(10, true);

	auto playIdx = buf->Delay(0);
	auto ptr = buf->BlockRead(playIdx);
	ASSERT_NE(ptr, nullptr);
}

TEST(FastPath, AudioBufferOnBlockWriteMatchesPerSample) {
	auto bufSize = 100u;
	auto numSamps = 32u;

	std::vector<float> src(numSamps);
	for (auto i = 0u; i < numSamps; i++)
		src[i] = ((float)i + 1.0f) * 0.03f;

	float fadeLevel = 0.8f;

	// Per-sample path
	auto bufA = std::make_shared<AudioBuffer>(bufSize);
	for (auto i = 0u; i < numSamps; i++)
		bufA->OnMixWrite(src[i], 0.0f, fadeLevel, (int)i, base::Audible::AUDIOSOURCE_MIXER);
	bufA->EndWrite(numSamps, true);

	// Block path
	auto bufB = std::make_shared<AudioBuffer>(bufSize);
	AudioWriteRequest request;
	request.samples = src.data();
	request.numSamps = numSamps;
	request.stride = 1;
	request.fadeCurrent = 0.0f;
	request.fadeNew = fadeLevel;
	request.source = base::Audible::AUDIOSOURCE_MIXER;

	bufB->OnBlockWrite(request, 0);
	bufB->EndWrite(numSamps, true);

	for (auto i = 0u; i < numSamps; i++)
	{
		ASSERT_FLOAT_EQ((*bufA)[i], (*bufB)[i])
			<< "Mismatch at index " << i;
	}
}

// --- AudioMixer OnPlayBlock matches OnPlay ---

TEST(FastPath, MixerOnPlayBlockMatchesPerSample) {
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

	// Per-sample path
	auto mixerA = AudioMixer(mixerParams);
	auto destA = std::make_shared<FastPathMockedMultiSink>(1, bufSize);
	for (auto i = 0u; i < numSamps; i++)
		mixerA.OnPlay(destA, src[i], i);

	destA->EndMultiWrite(numSamps, true, base::Audible::AUDIOSOURCE_MIXER);

	// Block path
	auto mixerB = AudioMixer(mixerParams);
	auto destB = std::make_shared<FastPathMockedMultiSink>(1, bufSize);
	ASSERT_TRUE(mixerB.IsBlockEligible());
	mixerB.OnPlayBlock(destB, src.data(), numSamps);

	destB->EndMultiWrite(numSamps, true, base::Audible::AUDIOSOURCE_MIXER);

	auto sinkA = destA->GetSink(0);
	auto sinkB = destB->GetSink(0);
	for (auto i = 0u; i < numSamps; i++)
	{
		ASSERT_FLOAT_EQ(sinkA->Samples[i], sinkB->Samples[i])
			<< "Mismatch at sample " << i;
	}
}

// --- Loop::OnPlay fast path produces identical output ---

TEST(FastPath, LoopOnPlayBlockMatchesFallback) {
	auto bufSize = 100u;
	auto blockSize = 32u;
	auto loopLength = 50ul;
	auto totalRecordSamps = constants::MaxLoopFadeSamps + loopLength;

	WireMixBehaviourParams mixBehaviour;
	mixBehaviour.Channels = { 0 };

	AudioMixerParams mixerParams;
	mixerParams.Size = { 160, 320 };
	mixerParams.Position = { 6, 6 };
	mixerParams.Behaviour = mixBehaviour;

	LoopParams loopParams;
	loopParams.Wav = "hh";
	loopParams.Size = { 80, 80 };
	loopParams.Position = { 10, 22 };

	auto loopA = Loop(loopParams, mixerParams);
	loopA.Record();
	for (auto i = 0ul; i < totalRecordSamps; i++)
		loopA.OnMixWrite(0.5f, 0.0f, 1.0f, (int)i, base::Audible::AUDIOSOURCE_ADC);
	loopA.EndWrite(totalRecordSamps, true);
	loopA.Play(constants::MaxLoopFadeSamps, loopLength, false);

	auto destA = std::make_shared<FastPathMockedMultiSink>(1, bufSize);
	auto numBlocks = bufSize / blockSize;

	for (auto i = 0u; i < numBlocks; i++)
	{
		destA->Zero(blockSize, base::Audible::AUDIOSOURCE_MIXER);
		loopA.OnPlay(destA, std::shared_ptr<engine::Trigger>(), 0u, blockSize);
		loopA.EndMultiPlay(blockSize);
		destA->EndMultiWrite(blockSize, true, base::Audible::AUDIOSOURCE_MIXER);
	}

	auto sinkA = destA->GetSink(0);
	bool hasNonZero = false;
	for (auto i = 0u; i < bufSize; i++)
	{
		if (sinkA->Samples[i] != 0.0f)
		{
			hasNonZero = true;
			break;
		}
	}
	ASSERT_TRUE(hasNonZero);

	for (auto i = 0u; i < bufSize; i++)
	{
		ASSERT_FLOAT_EQ(sinkA->Samples[i], 0.5f)
			<< "Unexpected value at sample " << i;
	}
}
