
#include "gtest/gtest.h"
#include "audio/AudioMixer.h"
#include "base/AudioSink.h"

using audio::WireMixBehaviour;
using audio::WireMixBehaviourParams;
using audio::MergeMixBehaviour;
using audio::MergeMixBehaviourParams;
using audio::BounceMixBehaviour;
using audio::BounceMixBehaviourParams;
using base::MultiAudioSink;

struct BlockWriteCall
{
	unsigned int Channel;
	float FadeCurrent;
	float FadeNew;
	int WriteOffset;
	unsigned int NumSamps;
	std::vector<float> SamplesReceived;
};

class MockMultiSink :
	public MultiAudioSink
{
public:
	MockMultiSink(unsigned int numChannels) :
		_numChannels(numChannels)
	{
	}

public:
	virtual unsigned int NumInputChannels(base::Audible::AudioSourceType source) const override
	{
		return _numChannels;
	}

	virtual void OnBlockWriteChannel(unsigned int channel,
		const base::AudioWriteRequest& request,
		int writeOffset) override
	{
		BlockWriteCall call;
		call.Channel = channel;
		call.FadeCurrent = request.fadeCurrent;
		call.FadeNew = request.fadeNew;
		call.WriteOffset = writeOffset;
		call.NumSamps = request.numSamps;
		for (auto i = 0u; i < request.numSamps; i++)
			call.SamplesReceived.push_back(request.samples[i * request.stride]);
		Calls.push_back(call);
	}

	std::vector<BlockWriteCall> Calls;

private:
	unsigned int _numChannels;
};

// ---- WireMixBehaviour ----

TEST(WireMixBehaviour, ApplyBlockWritesToSpecifiedChannel)
{
	WireMixBehaviourParams params({ 1u });
	WireMixBehaviour behaviour(params);

	auto sink = std::make_shared<MockMultiSink>(4u);
	float sample = 0.5f;
	behaviour.ApplyBlock(sink, &sample, 0.8f, 1, 0);

	ASSERT_EQ(1u, sink->Calls.size());
	EXPECT_EQ(1u, sink->Calls[0].Channel);
	EXPECT_FLOAT_EQ(0.5f, sink->Calls[0].SamplesReceived[0]);
	EXPECT_FLOAT_EQ(0.8f, sink->Calls[0].FadeNew);
}

TEST(WireMixBehaviour, ApplyBlockFadeCurrentIsAlwaysZero)
{
	WireMixBehaviourParams params({ 0u });
	WireMixBehaviour behaviour(params);

	auto sink = std::make_shared<MockMultiSink>(2u);
	float sample = 0.3f;
	behaviour.ApplyBlock(sink, &sample, 0.6f, 1, 0);

	ASSERT_EQ(1u, sink->Calls.size());
	EXPECT_FLOAT_EQ(0.0f, sink->Calls[0].FadeCurrent);
}

TEST(WireMixBehaviour, ApplyBlockWritesToMultipleChannels)
{
	WireMixBehaviourParams params({ 0u, 2u, 3u });
	WireMixBehaviour behaviour(params);

	auto sink = std::make_shared<MockMultiSink>(4u);
	float sample = 1.0f;
	behaviour.ApplyBlock(sink, &sample, 1.0f, 1, 5);

	ASSERT_EQ(3u, sink->Calls.size());
	EXPECT_EQ(0u, sink->Calls[0].Channel);
	EXPECT_EQ(2u, sink->Calls[1].Channel);
	EXPECT_EQ(3u, sink->Calls[2].Channel);
	for (const auto& call : sink->Calls)
	{
		EXPECT_EQ(5, call.WriteOffset);
		EXPECT_FLOAT_EQ(0.0f, call.FadeCurrent);
		EXPECT_FLOAT_EQ(1.0f, call.FadeNew);
	}
}

TEST(WireMixBehaviour, ApplyBlockNullDestDoesNotCrash)
{
	WireMixBehaviourParams params({ 0u });
	WireMixBehaviour behaviour(params);

	float sample = 1.0f;
	EXPECT_NO_FATAL_FAILURE(behaviour.ApplyBlock(nullptr, &sample, 1.0f, 1, 0));
}

TEST(WireMixBehaviour, SetParamsUpdatesChannels)
{
	WireMixBehaviourParams initial({ 0u });
	WireMixBehaviour behaviour(initial);

	WireMixBehaviourParams updated({ 1u, 2u });
	behaviour.SetParams(updated);

	auto sink = std::make_shared<MockMultiSink>(4u);
	float sample = 0.5f;
	behaviour.ApplyBlock(sink, &sample, 1.0f, 1, 0);

	ASSERT_EQ(2u, sink->Calls.size());
	EXPECT_EQ(1u, sink->Calls[0].Channel);
	EXPECT_EQ(2u, sink->Calls[1].Channel);
}

TEST(WireMixBehaviour, SetMaxChannelsRemovesOutOfRangeChannels)
{
	WireMixBehaviourParams params({ 0u, 1u, 2u, 3u });
	WireMixBehaviour behaviour(params);

	behaviour.SetMaxChannels(2u);

	auto sink = std::make_shared<MockMultiSink>(4u);
	float sample = 1.0f;
	behaviour.ApplyBlock(sink, &sample, 1.0f, 1, 0);

	// SetMaxChannels keeps channels with index < chans (count semantics).
	// For chans = 2, channels 0 and 1 remain; channels 2 and 3 are removed.
	ASSERT_EQ(2u, sink->Calls.size());
	EXPECT_EQ(0u, sink->Calls[0].Channel);
	EXPECT_EQ(1u, sink->Calls[1].Channel);
}

// ---- MergeMixBehaviour ----

TEST(MergeMixBehaviour, ApplyBlockWritesToSpecifiedChannel)
{
	MergeMixBehaviourParams params;
	params.Channels = { 0u };
	MergeMixBehaviour behaviour(params);

	auto sink = std::make_shared<MockMultiSink>(2u);
	float sample = 0.7f;
	behaviour.ApplyBlock(sink, &sample, 0.5f, 1, 0);

	ASSERT_EQ(1u, sink->Calls.size());
	EXPECT_EQ(0u, sink->Calls[0].Channel);
	EXPECT_FLOAT_EQ(0.7f, sink->Calls[0].SamplesReceived[0]);
	EXPECT_FLOAT_EQ(0.5f, sink->Calls[0].FadeNew);
}

TEST(MergeMixBehaviour, ApplyBlockFadeCurrentIsAlwaysOne)
{
	MergeMixBehaviourParams params;
	params.Channels = { 0u };
	MergeMixBehaviour behaviour(params);

	auto sink = std::make_shared<MockMultiSink>(2u);
	float sample = 0.4f;
	behaviour.ApplyBlock(sink, &sample, 0.9f, 1, 0);

	ASSERT_EQ(1u, sink->Calls.size());
	EXPECT_FLOAT_EQ(1.0f, sink->Calls[0].FadeCurrent);
}

TEST(MergeMixBehaviour, ApplyBlockIsAdditiveAcrossMultipleChannels)
{
	MergeMixBehaviourParams params;
	params.Channels = { 0u, 1u };
	MergeMixBehaviour behaviour(params);

	auto sink = std::make_shared<MockMultiSink>(2u);
	float sample = 0.5f;
	behaviour.ApplyBlock(sink, &sample, 0.5f, 1, 0);

	ASSERT_EQ(2u, sink->Calls.size());
	for (const auto& call : sink->Calls)
	{
		EXPECT_FLOAT_EQ(1.0f, call.FadeCurrent);
		EXPECT_FLOAT_EQ(0.5f, call.FadeNew);
		EXPECT_FLOAT_EQ(0.5f, call.SamplesReceived[0]);
	}
}

TEST(MergeMixBehaviour, ApplyBlockNullDestDoesNotCrash)
{
	MergeMixBehaviourParams params;
	params.Channels = { 0u };
	MergeMixBehaviour behaviour(params);

	float sample = 1.0f;
	EXPECT_NO_FATAL_FAILURE(behaviour.ApplyBlock(nullptr, &sample, 1.0f, 1, 0));
}

TEST(MergeMixBehaviour, SetParamsUpdatesChannels)
{
	MergeMixBehaviourParams initial;
	initial.Channels = { 0u };
	MergeMixBehaviour behaviour(initial);

	// MergeMixBehaviour inherits SetParams from WireMixBehaviour, which
	// handles WireMixBehaviourParams in the variant.
	WireMixBehaviourParams updated({ 2u, 3u });
	behaviour.SetParams(updated);

	auto sink = std::make_shared<MockMultiSink>(4u);
	float sample = 1.0f;
	behaviour.ApplyBlock(sink, &sample, 1.0f, 1, 0);

	ASSERT_EQ(2u, sink->Calls.size());
	EXPECT_EQ(2u, sink->Calls[0].Channel);
	EXPECT_EQ(3u, sink->Calls[1].Channel);
}

TEST(MergeMixBehaviour, SetMaxChannelsRemovesOutOfRangeChannels)
{
	MergeMixBehaviourParams params;
	params.Channels = { 0u, 1u, 3u };
	MergeMixBehaviour behaviour(params);

	behaviour.SetMaxChannels(2u);

	auto sink = std::make_shared<MockMultiSink>(4u);
	float sample = 1.0f;
	behaviour.ApplyBlock(sink, &sample, 1.0f, 1, 0);

	// SetMaxChannels removes channels strictly greater than chans.
	// Channel 3 (3 > 2) is removed; channels 0 and 1 remain.
	ASSERT_EQ(2u, sink->Calls.size());
	EXPECT_EQ(0u, sink->Calls[0].Channel);
	EXPECT_EQ(1u, sink->Calls[1].Channel);
}

// ---- BounceMixBehaviour ----

TEST(BounceMixBehaviour, ApplyBlockFadeNewZeroGivesFadeCurrentOne)
{
	BounceMixBehaviourParams params;
	params.Channels = { 0u };
	BounceMixBehaviour behaviour(params);

	auto sink = std::make_shared<MockMultiSink>(2u);
	float sample = 0.8f;
	behaviour.ApplyBlock(sink, &sample, 0.0f, 1, 0);

	ASSERT_EQ(1u, sink->Calls.size());
	EXPECT_FLOAT_EQ(0.0f, sink->Calls[0].FadeNew);
	EXPECT_FLOAT_EQ(1.0f, sink->Calls[0].FadeCurrent);
}

TEST(BounceMixBehaviour, ApplyBlockFadeNewOneGivesFadeCurrentZero)
{
	BounceMixBehaviourParams params;
	params.Channels = { 0u };
	BounceMixBehaviour behaviour(params);

	auto sink = std::make_shared<MockMultiSink>(2u);
	float sample = 0.8f;
	behaviour.ApplyBlock(sink, &sample, 1.0f, 1, 0);

	ASSERT_EQ(1u, sink->Calls.size());
	EXPECT_FLOAT_EQ(1.0f, sink->Calls[0].FadeNew);
	EXPECT_FLOAT_EQ(0.0f, sink->Calls[0].FadeCurrent);
}

TEST(BounceMixBehaviour, ApplyBlockMidFadeInterpolates)
{
	BounceMixBehaviourParams params;
	params.Channels = { 0u };
	BounceMixBehaviour behaviour(params);

	auto sink = std::make_shared<MockMultiSink>(2u);
	float sample = 0.5f;
	behaviour.ApplyBlock(sink, &sample, 0.4f, 1, 0);

	ASSERT_EQ(1u, sink->Calls.size());
	EXPECT_FLOAT_EQ(0.4f, sink->Calls[0].FadeNew);
	EXPECT_FLOAT_EQ(0.6f, sink->Calls[0].FadeCurrent);
}

TEST(BounceMixBehaviour, ApplyBlockNullDestDoesNotCrash)
{
	BounceMixBehaviourParams params;
	params.Channels = { 0u };
	BounceMixBehaviour behaviour(params);

	float sample = 1.0f;
	EXPECT_NO_FATAL_FAILURE(behaviour.ApplyBlock(nullptr, &sample, 0.5f, 1, 0));
}

TEST(BounceMixBehaviour, ApplyBlockWritesToMultipleChannels)
{
	BounceMixBehaviourParams params;
	params.Channels = { 0u, 1u };
	BounceMixBehaviour behaviour(params);

	auto sink = std::make_shared<MockMultiSink>(2u);
	float sample = 0.5f;
	behaviour.ApplyBlock(sink, &sample, 0.3f, 1, 0);

	ASSERT_EQ(2u, sink->Calls.size());
	EXPECT_EQ(0u, sink->Calls[0].Channel);
	EXPECT_EQ(1u, sink->Calls[1].Channel);
	for (const auto& call : sink->Calls)
	{
		EXPECT_FLOAT_EQ(0.3f, call.FadeNew);
		EXPECT_FLOAT_EQ(0.7f, call.FadeCurrent);
	}
}
