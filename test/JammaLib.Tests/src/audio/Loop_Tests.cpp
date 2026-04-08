
#include "gtest/gtest.h"
#include "resources/ResourceLib.h"
#include "engine/Loop.h"

using resources::ResourceLib;
using engine::Loop;
using engine::LoopParams;
using audio::WireMixBehaviourParams;
using audio::AudioMixerParams;
using base::AudioSource;
using base::AudioSink;
using base::MultiAudioSink;
using base::AudioSourceParams;
using base::AudioWriteRequest;

class LoopMockedSink :
public AudioSink
{
public:
LoopMockedSink(unsigned int bufSize) :
Samples({})
{
Samples = std::vector<float>(bufSize);

for (auto i = 0u; i < bufSize; i++)
{
Samples[i] = 0.0f;
}
}

public:
virtual void OnBlockWrite(const AudioWriteRequest& request, int writeOffset) override
{
for (auto i = 0u; i < request.numSamps; i++)
{
auto destIndex = _writeIndex + writeOffset + i;
if (destIndex < Samples.size())
{
auto samp = request.samples[i * request.stride];
Samples[destIndex] = (request.fadeNew * samp) + (request.fadeCurrent * Samples[destIndex]);
}
}
}
virtual void EndWrite(unsigned int numSamps, bool updateIndex)
{
if (updateIndex)
_writeIndex += numSamps;
}

bool IsFilled() { return _writeIndex >= Samples.size(); }

std::vector<float> Samples;
};

class MockedMultiSink :
public MultiAudioSink
{
public:
MockedMultiSink(unsigned int bufSize)
{
_sink = std::make_shared<LoopMockedSink>(bufSize);
}

public:
virtual unsigned int NumInputChannels(base::Audible::AudioSourceType source) const override { return 1; };

bool IsFilled() { return _sink->IsFilled(); }
const std::vector<float>& GetSamples() const { return _sink->Samples; }
std::shared_ptr<LoopMockedSink> GetSink() { return _sink; }

protected:
virtual const std::shared_ptr<AudioSink> _InputChannel(unsigned int channel,
base::Audible::AudioSourceType source) override
{
if (channel == 0)
return _sink;

return std::shared_ptr<AudioSink>();
}
private:
std::shared_ptr<LoopMockedSink> _sink;
};

TEST(Loop, PlayWrapsAround) {
auto bufSize = 100;
auto blockSize = 11;

auto sink = std::make_shared<MockedMultiSink>(bufSize);

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

engine::TriggerParams trigParams;
trigParams.Index = 0;

auto trigger = std::make_shared<engine::Trigger>(trigParams);

auto loop = Loop(loopParams, mixerParams);

// Initialize loop with audio data so _loopLength > 0 before playback.
const auto loopLength = 50ul;
const auto totalRecordSamps = constants::MaxLoopFadeSamps + loopLength;

loop.Record();

// Write data using block API
std::vector<float> recordData(totalRecordSamps, 1.0f);
AudioWriteRequest writeReq;
writeReq.samples = recordData.data();
writeReq.numSamps = static_cast<unsigned int>(totalRecordSamps);
writeReq.stride = 1;
writeReq.fadeCurrent = 0.0f;
writeReq.fadeNew = 1.0f;
writeReq.source = base::Audible::AUDIOSOURCE_ADC;
loop.OnBlockWrite(writeReq, 0);
loop.EndWrite(static_cast<unsigned int>(totalRecordSamps), true);

loop.Play(constants::MaxLoopFadeSamps, loopLength, false);

auto numBlocks = (bufSize * 2) / blockSize;

for (int i = 0; i < numBlocks; i++)
{
sink->Zero(blockSize, base::Audible::AUDIOSOURCE_ADC);
loop.WriteBlock(sink, std::shared_ptr<engine::Trigger>(), 0, blockSize);
loop.EndMultiPlay(blockSize);
sink->EndMultiWrite(blockSize, true, base::Audible::AUDIOSOURCE_ADC);
}

ASSERT_TRUE(sink->IsFilled());
}

// -- Helpers ----------------------------------------------------------------

static Loop MakeLoop()
{
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

return Loop(loopParams, mixerParams);
}

// Write loopLength samples of `value` into `loop` using block API.
static void WriteData(Loop& loop,
unsigned long loopLength,
float value = 1.0f)
{
const auto totalRecordSamps = constants::MaxLoopFadeSamps + loopLength;

std::vector<float> data(totalRecordSamps, value);
AudioWriteRequest request;
request.samples = data.data();
request.numSamps = static_cast<unsigned int>(totalRecordSamps);
request.stride = 1;
request.fadeCurrent = 0.0f;
request.fadeNew = 1.0f;
request.source = base::Audible::AUDIOSOURCE_ADC;
loop.OnBlockWrite(request, 0);
loop.EndWrite(static_cast<unsigned int>(totalRecordSamps), true);
}

// Record loopLength samples then transition to PLAYING or PLAYINGRECORDING.
static void RecordAndPlay(Loop& loop,
unsigned long loopLength,
bool continueRecording,
float value = 1.0f)
{
loop.Record();
WriteData(loop, loopLength, value);
loop.Play(constants::MaxLoopFadeSamps, loopLength, continueRecording);
}

// Play one block through `loop` into `sink` and advance indices.
static void PlayOneBlock(Loop& loop,
const std::shared_ptr<MockedMultiSink>& sink,
unsigned int blockSize)
{
sink->Zero(blockSize, base::Audible::AUDIOSOURCE_ADC);
loop.WriteBlock(sink, std::shared_ptr<engine::Trigger>(), 0, blockSize);
loop.EndMultiPlay(blockSize);
sink->EndMultiWrite(blockSize, true, base::Audible::AUDIOSOURCE_ADC);
}

static bool HasNonZeroSample(const std::vector<float>& samples)
{
for (auto sample : samples)
if (sample != 0.0f)
return true;
return false;
}

// -- State-transition tests -------------------------------------------------

TEST(Loop, StateTransition_InactiveToRecording)
{
auto loop = MakeLoop();
ASSERT_EQ(Loop::STATE_INACTIVE, loop.PlayState());
loop.Record();
ASSERT_EQ(Loop::STATE_RECORDING, loop.PlayState());
}

TEST(Loop, StateTransition_RecordIgnoredWhenNotInactive)
{
auto loop = MakeLoop();
loop.Record();
ASSERT_EQ(Loop::STATE_RECORDING, loop.PlayState());
loop.Record();  // already active - must be ignored
ASSERT_EQ(Loop::STATE_RECORDING, loop.PlayState());
}

TEST(Loop, StateTransition_RecordingToPlaying)
{
auto loop = MakeLoop();
const auto loopLength = 50ul;
RecordAndPlay(loop, loopLength, false);
ASSERT_EQ(Loop::STATE_PLAYING, loop.PlayState());
}

TEST(Loop, StateTransition_RecordingToPlayingRecording)
{
auto loop = MakeLoop();
const auto loopLength = 50ul;
RecordAndPlay(loop, loopLength, true);
ASSERT_EQ(Loop::STATE_PLAYINGRECORDING, loop.PlayState());
}

TEST(Loop, StateTransition_PlayingRecordingToPlaying)
{
auto loop = MakeLoop();
const auto loopLength = 50ul;
RecordAndPlay(loop, loopLength, true);
ASSERT_EQ(Loop::STATE_PLAYINGRECORDING, loop.PlayState());
loop.EndRecording();
ASSERT_EQ(Loop::STATE_PLAYING, loop.PlayState());
}

TEST(Loop, StateTransition_InactiveToOverdubbing)
{
auto loop = MakeLoop();
loop.Overdub();
ASSERT_EQ(Loop::STATE_OVERDUBBING, loop.PlayState());
}

TEST(Loop, StateTransition_OverdubbingIgnoredWhenNotInactive)
{
auto loop = MakeLoop();
loop.Record();  // RECORDING state
loop.Overdub(); // must be ignored
ASSERT_EQ(Loop::STATE_RECORDING, loop.PlayState());
}

TEST(Loop, StateTransition_OverdubbingToPunchedIn)
{
auto loop = MakeLoop();
loop.Overdub();
loop.PunchIn();
ASSERT_EQ(Loop::STATE_PUNCHEDIN, loop.PlayState());
}

TEST(Loop, StateTransition_PunchInIgnoredWhenNotOverdubbing)
{
auto loop = MakeLoop();
loop.PunchIn(); // INACTIVE - must be ignored
ASSERT_EQ(Loop::STATE_INACTIVE, loop.PlayState());
}

TEST(Loop, StateTransition_PunchedInToOverdubbing)
{
auto loop = MakeLoop();
loop.Overdub();
loop.PunchIn();
loop.PunchOut();
ASSERT_EQ(Loop::STATE_OVERDUBBING, loop.PlayState());
}

TEST(Loop, StateTransition_PunchOutIgnoredWhenNotPunchedIn)
{
auto loop = MakeLoop();
loop.Overdub();
loop.PunchOut(); // OVERDUBBING, not PUNCHEDIN - must be ignored
ASSERT_EQ(Loop::STATE_OVERDUBBING, loop.PlayState());
}

TEST(Loop, StateTransition_OverdubbingToOverdubbingRecording)
{
auto loop = MakeLoop();
const auto loopLength = 50ul;
loop.Overdub();
WriteData(loop, loopLength);
loop.Play(constants::MaxLoopFadeSamps, loopLength, true);
ASSERT_EQ(Loop::STATE_OVERDUBBINGRECORDING, loop.PlayState());
}

TEST(Loop, StateTransition_PunchedInToOverdubbingRecording)
{
auto loop = MakeLoop();
const auto loopLength = 50ul;
loop.Overdub();
loop.PunchIn();
WriteData(loop, loopLength);
loop.Play(constants::MaxLoopFadeSamps, loopLength, true);
ASSERT_EQ(Loop::STATE_OVERDUBBINGRECORDING, loop.PlayState());
}

TEST(Loop, StateTransition_OverdubbingRecordingToPlaying)
{
auto loop = MakeLoop();
const auto loopLength = 50ul;
loop.Overdub();
WriteData(loop, loopLength);
loop.Play(constants::MaxLoopFadeSamps, loopLength, true);
loop.EndRecording();
ASSERT_EQ(Loop::STATE_PLAYING, loop.PlayState());
}

TEST(Loop, StateTransition_DitchResetsFromRecording)
{
auto loop = MakeLoop();
loop.Record();
loop.Ditch();
ASSERT_EQ(Loop::STATE_INACTIVE, loop.PlayState());
}

TEST(Loop, StateTransition_DitchResetsFromPlaying)
{
auto loop = MakeLoop();
const auto loopLength = 50ul;
RecordAndPlay(loop, loopLength, false);
loop.Ditch();
ASSERT_EQ(Loop::STATE_INACTIVE, loop.PlayState());
}

TEST(Loop, StateTransition_DitchIgnoredWhenInactive)
{
auto loop = MakeLoop();
loop.Ditch(); // already INACTIVE - must be ignored
ASSERT_EQ(Loop::STATE_INACTIVE, loop.PlayState());
}

// -- Recording-behaviour tests ----------------------------------------------

TEST(Loop, Recording_WritesDataInRecordingState)
{
const auto loopLength = 50ul;
const auto blockSize = 11u;
auto sink = std::make_shared<MockedMultiSink>(blockSize);

auto loop = MakeLoop();
RecordAndPlay(loop, loopLength, false, 1.0f);
ASSERT_EQ(Loop::STATE_PLAYING, loop.PlayState());

PlayOneBlock(loop, sink, blockSize);

ASSERT_TRUE(HasNonZeroSample(sink->GetSamples()));
}

TEST(Loop, Recording_DoesNotWriteDataWhenInactive)
{
const auto loopLength = 50ul;

auto loop = MakeLoop();
// Attempt to write without entering a recording state
WriteData(loop, loopLength, 1.0f);

// Buffer is empty so Play() must reset to INACTIVE
loop.Play(constants::MaxLoopFadeSamps, loopLength, false);
ASSERT_EQ(Loop::STATE_INACTIVE, loop.PlayState());
}

// -- Playback-behaviour tests -----------------------------------------------

TEST(Loop, Playback_NoOutputInRecordingState)
{
const auto blockSize = 11u;
auto sink = std::make_shared<MockedMultiSink>(blockSize);

auto loop = MakeLoop();
loop.Record();
// Still recording - _loopLength is 0 so WriteBlock must return early
PlayOneBlock(loop, sink, blockSize);

ASSERT_FALSE(HasNonZeroSample(sink->GetSamples()));
}

TEST(Loop, Playback_ProducesOutputInPlayingState)
{
const auto loopLength = 50ul;
const auto blockSize = 11u;
auto sink = std::make_shared<MockedMultiSink>(blockSize);

auto loop = MakeLoop();
RecordAndPlay(loop, loopLength, false, 1.0f);
ASSERT_EQ(Loop::STATE_PLAYING, loop.PlayState());

PlayOneBlock(loop, sink, blockSize);

ASSERT_TRUE(HasNonZeroSample(sink->GetSamples()));
}

TEST(Loop, Playback_ProducesOutputInPlayingRecordingState)
{
const auto loopLength = 50ul;
const auto blockSize = 11u;
auto sink = std::make_shared<MockedMultiSink>(blockSize);

auto loop = MakeLoop();
RecordAndPlay(loop, loopLength, true, 1.0f);  // PLAYINGRECORDING
ASSERT_EQ(Loop::STATE_PLAYINGRECORDING, loop.PlayState());

PlayOneBlock(loop, sink, blockSize);

ASSERT_TRUE(HasNonZeroSample(sink->GetSamples()));
}

TEST(Loop, Playback_ProducesOutputInOverdubbingRecordingState)
{
const auto loopLength = 50ul;
const auto blockSize = 11u;
auto sink = std::make_shared<MockedMultiSink>(blockSize);

auto loop = MakeLoop();
loop.Overdub();
WriteData(loop, loopLength, 1.0f);
loop.Play(constants::MaxLoopFadeSamps, loopLength, true);  // OVERDUBBINGRECORDING
ASSERT_EQ(Loop::STATE_OVERDUBBINGRECORDING, loop.PlayState());

PlayOneBlock(loop, sink, blockSize);

ASSERT_TRUE(HasNonZeroSample(sink->GetSamples()));
}

// Verify that a constant-value loop produces no gain bump in the crossfade
// region at the wrap point. With normalized Hanning weights, fadeIn + fadeOut
// == 1.0, so overlapping identical content must pass through unchanged.
TEST(Loop, WrapXfade_ConstantInputNoGainBump) {
const auto fadeSamps = constants::DefaultFadeSamps;
const auto loopLength = 50ul;
const auto totalRecordSamps = constants::MaxLoopFadeSamps + loopLength;
const float inputVal = 0.5f;

// blockSize spans twice the fade window to guarantee the full xfade
// region is captured (fadeSamps samples before wrap + fadeSamps after).
const unsigned int blockSize = fadeSamps * 2u;

WireMixBehaviourParams mixBehaviour;
mixBehaviour.Channels = { 0 };
auto mixerParams = Loop::GetMixerParams({ 80, 80 }, mixBehaviour);

LoopParams loopParams;
loopParams.Wav = "test";
loopParams.FadeSamps = fadeSamps;

Loop loop(loopParams, mixerParams);
loop.Record();

// Write data using block API
std::vector<float> recordData(totalRecordSamps, inputVal);
AudioWriteRequest writeReq;
writeReq.samples = recordData.data();
writeReq.numSamps = static_cast<unsigned int>(totalRecordSamps);
writeReq.stride = 1;
writeReq.fadeCurrent = 0.0f;
writeReq.fadeNew = 1.0f;
writeReq.source = base::Audible::AUDIOSOURCE_ADC;
loop.OnBlockWrite(writeReq, 0);
loop.EndWrite(static_cast<unsigned int>(totalRecordSamps), true);

// Start playback just before the crossfade region so the captured block
// straddles the entire fade window.
auto startIndex = constants::MaxLoopFadeSamps + loopLength - fadeSamps;
loop.Play(startIndex, loopLength, false);

auto sink = std::make_shared<MockedMultiSink>(blockSize);
loop.WriteBlock(sink, std::shared_ptr<engine::Trigger>(), 0, blockSize);
loop.EndMultiPlay(blockSize);
sink->EndMultiWrite(blockSize, true, base::Audible::AUDIOSOURCE_ADC);

const auto& samples = sink->GetSink()->Samples;
for (unsigned int s = 0; s < blockSize; s++)
{
EXPECT_NEAR(samples[s], inputVal, 1e-5f)
<< "Gain bump at crossfade sample " << s;
}
}
