
#include "gtest/gtest.h"
#include "audio/AudioMixer.h"

using audio::WireMixBehaviour;
using audio::WireMixBehaviourParams;
using audio::MergeMixBehaviour;
using audio::MergeMixBehaviourParams;
using audio::BounceMixBehaviour;
using audio::BounceMixBehaviourParams;
using base::MultiAudioSink;
using base::AudioSink;
using base::AudioWriteRequest;

struct BlockWriteCall
{
unsigned int Channel;
unsigned int NumSamps;
float FadeCurrent;
float FadeNew;
unsigned int StartIndex;
const float* Samples;
};

class MockBlockSink :
public AudioSink
{
public:
virtual void OnBlockWrite(const AudioWriteRequest& request, int writeOffset) override {}
virtual void EndWrite(unsigned int numSamps, bool updateIndex) override {}
};

class MockBlockMultiSink :
public MultiAudioSink
{
public:
MockBlockMultiSink(unsigned int numChannels) :
_numChannels(numChannels)
{
}

public:
virtual unsigned int NumInputChannels(base::Audible::AudioSourceType source) const override
{
return _numChannels;
}

virtual void OnBlockWriteChannel(unsigned int channel,
const AudioWriteRequest& request,
int writeOffset) override
{
Calls.push_back({
channel,
request.numSamps,
request.fadeCurrent,
request.fadeNew,
static_cast<unsigned int>(writeOffset),
request.samples
});
}

std::vector<BlockWriteCall> Calls;

protected:
virtual const std::shared_ptr<AudioSink> _InputChannel(unsigned int channel,
base::Audible::AudioSourceType source) override
{
return nullptr;
}

private:
unsigned int _numChannels;
};

// ---- WireMixBehaviour ----

TEST(WireMixBehaviour, ApplyBlockWritesToSpecifiedChannel)
{
WireMixBehaviourParams params({ 1u });
WireMixBehaviour behaviour(params);

float srcBuf[] = { 0.5f };
auto sink = std::make_shared<MockBlockMultiSink>(4u);
behaviour.ApplyBlock(sink, srcBuf, 0.8f, 1, 0);

ASSERT_EQ(1u, sink->Calls.size());
EXPECT_EQ(1u, sink->Calls[0].Channel);
EXPECT_FLOAT_EQ(0.8f, sink->Calls[0].FadeNew);
}

TEST(WireMixBehaviour, ApplyBlockFadeCurrentIsAlwaysZero)
{
WireMixBehaviourParams params({ 0u });
WireMixBehaviour behaviour(params);

float srcBuf[] = { 0.3f };
auto sink = std::make_shared<MockBlockMultiSink>(2u);
behaviour.ApplyBlock(sink, srcBuf, 0.6f, 1, 0);

ASSERT_EQ(1u, sink->Calls.size());
EXPECT_FLOAT_EQ(0.0f, sink->Calls[0].FadeCurrent);
}

TEST(WireMixBehaviour, ApplyBlockWritesToMultipleChannels)
{
WireMixBehaviourParams params({ 0u, 2u, 3u });
WireMixBehaviour behaviour(params);

float srcBuf[] = { 1.0f, 1.0f };
auto sink = std::make_shared<MockBlockMultiSink>(4u);
behaviour.ApplyBlock(sink, srcBuf, 1.0f, 2, 5);

ASSERT_EQ(3u, sink->Calls.size());
EXPECT_EQ(0u, sink->Calls[0].Channel);
EXPECT_EQ(2u, sink->Calls[1].Channel);
EXPECT_EQ(3u, sink->Calls[2].Channel);
for (const auto& call : sink->Calls)
{
EXPECT_EQ(5u, call.StartIndex);
EXPECT_FLOAT_EQ(0.0f, call.FadeCurrent);
EXPECT_FLOAT_EQ(1.0f, call.FadeNew);
EXPECT_EQ(2u, call.NumSamps);
}
}

TEST(WireMixBehaviour, ApplyBlockNullDestDoesNotCrash)
{
WireMixBehaviourParams params({ 0u });
WireMixBehaviour behaviour(params);

float srcBuf[] = { 1.0f };
EXPECT_NO_FATAL_FAILURE(behaviour.ApplyBlock(nullptr, srcBuf, 1.0f, 1, 0));
}

TEST(WireMixBehaviour, SetParamsUpdatesChannels)
{
WireMixBehaviourParams initial({ 0u });
WireMixBehaviour behaviour(initial);

WireMixBehaviourParams updated({ 1u, 2u });
behaviour.SetParams(updated);

float srcBuf[] = { 0.5f };
auto sink = std::make_shared<MockBlockMultiSink>(4u);
behaviour.ApplyBlock(sink, srcBuf, 1.0f, 1, 0);

ASSERT_EQ(2u, sink->Calls.size());
EXPECT_EQ(1u, sink->Calls[0].Channel);
EXPECT_EQ(2u, sink->Calls[1].Channel);
}

TEST(WireMixBehaviour, SetMaxChannelsRemovesOutOfRangeChannels)
{
WireMixBehaviourParams params({ 0u, 1u, 2u, 3u });
WireMixBehaviour behaviour(params);

behaviour.SetMaxChannels(2u);

float srcBuf[] = { 1.0f };
auto sink = std::make_shared<MockBlockMultiSink>(4u);
behaviour.ApplyBlock(sink, srcBuf, 1.0f, 1, 0);

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

float srcBuf[] = { 0.7f };
auto sink = std::make_shared<MockBlockMultiSink>(2u);
behaviour.ApplyBlock(sink, srcBuf, 0.5f, 1, 0);

ASSERT_EQ(1u, sink->Calls.size());
EXPECT_EQ(0u, sink->Calls[0].Channel);
EXPECT_FLOAT_EQ(0.5f, sink->Calls[0].FadeNew);
}

TEST(MergeMixBehaviour, ApplyBlockFadeCurrentIsAlwaysOne)
{
MergeMixBehaviourParams params;
params.Channels = { 0u };
MergeMixBehaviour behaviour(params);

float srcBuf[] = { 0.4f };
auto sink = std::make_shared<MockBlockMultiSink>(2u);
behaviour.ApplyBlock(sink, srcBuf, 0.9f, 1, 0);

ASSERT_EQ(1u, sink->Calls.size());
EXPECT_FLOAT_EQ(1.0f, sink->Calls[0].FadeCurrent);
}

TEST(MergeMixBehaviour, ApplyBlockIsAdditiveAcrossMultipleChannels)
{
MergeMixBehaviourParams params;
params.Channels = { 0u, 1u };
MergeMixBehaviour behaviour(params);

float srcBuf[] = { 0.5f };
auto sink = std::make_shared<MockBlockMultiSink>(2u);
behaviour.ApplyBlock(sink, srcBuf, 0.5f, 1, 0);

ASSERT_EQ(2u, sink->Calls.size());
for (const auto& call : sink->Calls)
{
EXPECT_FLOAT_EQ(1.0f, call.FadeCurrent);
EXPECT_FLOAT_EQ(0.5f, call.FadeNew);
}
}

TEST(MergeMixBehaviour, ApplyBlockNullDestDoesNotCrash)
{
MergeMixBehaviourParams params;
params.Channels = { 0u };
MergeMixBehaviour behaviour(params);

float srcBuf[] = { 1.0f };
EXPECT_NO_FATAL_FAILURE(behaviour.ApplyBlock(nullptr, srcBuf, 1.0f, 1, 0));
}

TEST(MergeMixBehaviour, SetParamsUpdatesChannels)
{
MergeMixBehaviourParams initial;
initial.Channels = { 0u };
MergeMixBehaviour behaviour(initial);

WireMixBehaviourParams updated({ 2u, 3u });
behaviour.SetParams(updated);

float srcBuf[] = { 1.0f };
auto sink = std::make_shared<MockBlockMultiSink>(4u);
behaviour.ApplyBlock(sink, srcBuf, 1.0f, 1, 0);

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

float srcBuf[] = { 1.0f };
auto sink = std::make_shared<MockBlockMultiSink>(4u);
behaviour.ApplyBlock(sink, srcBuf, 1.0f, 1, 0);

// SetMaxChannels removes channels >= chans.
// Channel 3 (3 >= 2) is removed; channels 0 and 1 remain.
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

float srcBuf[] = { 0.8f };
auto sink = std::make_shared<MockBlockMultiSink>(2u);
behaviour.ApplyBlock(sink, srcBuf, 0.0f, 1, 0);

ASSERT_EQ(1u, sink->Calls.size());
EXPECT_FLOAT_EQ(0.0f, sink->Calls[0].FadeNew);
EXPECT_FLOAT_EQ(1.0f, sink->Calls[0].FadeCurrent);
}

TEST(BounceMixBehaviour, ApplyBlockFadeNewOneGivesFadeCurrentZero)
{
BounceMixBehaviourParams params;
params.Channels = { 0u };
BounceMixBehaviour behaviour(params);

float srcBuf[] = { 0.8f };
auto sink = std::make_shared<MockBlockMultiSink>(2u);
behaviour.ApplyBlock(sink, srcBuf, 1.0f, 1, 0);

ASSERT_EQ(1u, sink->Calls.size());
EXPECT_FLOAT_EQ(1.0f, sink->Calls[0].FadeNew);
EXPECT_FLOAT_EQ(0.0f, sink->Calls[0].FadeCurrent);
}

TEST(BounceMixBehaviour, ApplyBlockMidFadeInterpolates)
{
BounceMixBehaviourParams params;
params.Channels = { 0u };
BounceMixBehaviour behaviour(params);

float srcBuf[] = { 0.5f };
auto sink = std::make_shared<MockBlockMultiSink>(2u);
behaviour.ApplyBlock(sink, srcBuf, 0.4f, 1, 0);

ASSERT_EQ(1u, sink->Calls.size());
EXPECT_FLOAT_EQ(0.4f, sink->Calls[0].FadeNew);
EXPECT_FLOAT_EQ(0.6f, sink->Calls[0].FadeCurrent);
}

TEST(BounceMixBehaviour, ApplyBlockNullDestDoesNotCrash)
{
BounceMixBehaviourParams params;
params.Channels = { 0u };
BounceMixBehaviour behaviour(params);

float srcBuf[] = { 1.0f };
EXPECT_NO_FATAL_FAILURE(behaviour.ApplyBlock(nullptr, srcBuf, 0.5f, 1, 0));
}

TEST(BounceMixBehaviour, ApplyBlockWritesToMultipleChannels)
{
BounceMixBehaviourParams params;
params.Channels = { 0u, 1u };
BounceMixBehaviour behaviour(params);

float srcBuf[] = { 0.5f };
auto sink = std::make_shared<MockBlockMultiSink>(2u);
behaviour.ApplyBlock(sink, srcBuf, 0.3f, 1, 0);

ASSERT_EQ(2u, sink->Calls.size());
EXPECT_EQ(0u, sink->Calls[0].Channel);
EXPECT_EQ(1u, sink->Calls[1].Channel);
for (const auto& call : sink->Calls)
{
EXPECT_FLOAT_EQ(0.3f, call.FadeNew);
EXPECT_FLOAT_EQ(0.7f, call.FadeCurrent);
}
}
