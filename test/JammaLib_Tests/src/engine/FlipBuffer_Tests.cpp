
#include "gtest/gtest.h"
#include "base/AudioSink.h"
#include "actions/TriggerAction.h"
#include "engine/LoopTake.h"
#include "engine/Station.h"

using actions::GuiAction;
using actions::TriggerAction;
using engine::LoopTake;
using engine::LoopTakeParams;
using engine::Station;
using engine::StationParams;
using audio::MergeMixBehaviourParams;
using base::AudioWriteRequest;
using base::Audible;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::shared_ptr<LoopTake> MakeLoopTake(const std::string& id = "take-0")
{
	LoopTakeParams params;
	params.Id = id;
	params.Size = { 100, 100 };
	MergeMixBehaviourParams merge;
	auto mixerParams = LoopTake::GetMixerParams(params.Size, merge);
	return std::make_shared<LoopTake>(params, mixerParams);
}

static std::shared_ptr<Station> MakeStation(const std::string& name = "test-station")
{
	StationParams params;
	params.Name = name;
	params.Size = { 200, 200 };
	MergeMixBehaviourParams merge;
	auto mixerParams = Station::GetMixerParams(params.Size, merge);
	return std::make_shared<Station>(params, mixerParams);
}

class CaptureSink :
	public base::AudioSink
{
public:
	explicit CaptureSink(unsigned int bufSize) : Samples(bufSize, 0.0f) {}

	virtual void OnBlockWrite(const AudioWriteRequest& request, int writeOffset) override
	{
		for (auto sampleIndex = 0u; sampleIndex < request.numSamps; sampleIndex++)
		{
			auto bufferIndex = _writeIndex + writeOffset + sampleIndex;
			if (bufferIndex < Samples.size())
			{
				auto samp = request.samples[sampleIndex * request.stride];
				Samples[bufferIndex] = (request.fadeNew * samp) + (request.fadeCurrent * Samples[bufferIndex]);
			}
		}
	}

	virtual void EndWrite(unsigned int numSamps, bool updateIndex) override
	{
		if (updateIndex)
			_writeIndex += numSamps;
	}

	std::vector<float> Samples;
};

class CaptureMultiSink :
	public base::MultiAudioSink
{
public:
	explicit CaptureMultiSink(unsigned int numChannels) : _sinks()
	{
		for (auto channelIndex = 0u; channelIndex < numChannels; channelIndex++)
			_sinks.push_back(std::make_shared<CaptureSink>(1u));
	}

	virtual unsigned int NumInputChannels(base::Audible::AudioSourceType source) const override
	{
		return (unsigned int)_sinks.size();
	}

	float Sample(unsigned int channel) const
	{
		return _sinks.at(channel)->Samples.at(0);
	}

protected:
	virtual const std::shared_ptr<base::AudioSink> _InputChannel(unsigned int channel,
		base::Audible::AudioSourceType source) override
	{
		return channel < _sinks.size() ?
			_sinks[channel] :
			nullptr;
	}

private:
	std::vector<std::shared_ptr<CaptureSink>> _sinks;
};

class TestStation :
	public Station
{
public:
	TestStation(StationParams params, audio::AudioMixerParams mixerParams) :
		Station(params, mixerParams)
	{
	}

	void SetMixerLevel(unsigned int channel, double level)
	{
		_audioMixers.at(channel)->SetUnmutedLevel(level);
		_audioMixers.at(channel)->Offset(4096);
	}
};

static std::shared_ptr<TestStation> MakeTestStation(const std::string& name = "test-station")
{
	StationParams params;
	params.Name = name;
	params.Size = { 200, 200 };
	MergeMixBehaviourParams merge;
	auto mixerParams = Station::GetMixerParams(params.Size, merge);
	return std::make_shared<TestStation>(params, mixerParams);
}

static std::vector<float> ReadStationOutput(const std::shared_ptr<Station>& station,
	const std::vector<float>& busSamples)
{
	station->Zero(1u, Audible::AUDIOSOURCE_MIXER);

	for (auto chan = 0u; chan < busSamples.size(); chan++)
	{
		AudioWriteRequest request;
		request.samples = &busSamples[chan];
		request.numSamps = 1u;
		request.stride = 1u;
		request.fadeCurrent = 0.0f;
		request.fadeNew = 1.0f;
		request.source = Audible::AUDIOSOURCE_MIXER;
		station->OnBlockWriteChannel(chan, request, 0);
	}

	station->EndMultiWrite(1u, true, Audible::AUDIOSOURCE_MIXER);

	auto capture = std::make_shared<CaptureMultiSink>((unsigned int)busSamples.size());
	station->WriteBlock(capture, nullptr, 0, 1u);
	capture->EndMultiWrite(1u, true, Audible::AUDIOSOURCE_MIXER);
	station->EndMultiPlay(1u);

	std::vector<float> outSamples;
	for (auto chan = 0u; chan < busSamples.size(); chan++)
		outSamples.push_back(capture->Sample(chan));

	return outSamples;
}

static void AssertStationRouterUpdateReassignsPerChannelMixer(GuiAction::ActionElementType elementType)
{
	auto station = MakeTestStation();
	station->SetNumBusChannels(2u);
	station->SetNumDacChannels(2u);
	station->CommitChanges();

	station->SetMixerLevel(0u, 0.25);
	station->SetMixerLevel(1u, 1.0);

	GuiAction action;
	action.ElementType = elementType;
	action.Data = GuiAction::GuiConnections{ { {0u, 1u}, {1u, 0u} } };
	station->OnAction(action);

	auto outSamples = ReadStationOutput(station, { 1.0f, 1.0f });
	ASSERT_EQ(2u, outSamples.size());
	// Two bus buffers at 1.0f each are summed through one mixer per output.
	// After swapping routes {(0,1), (1,0)}, output 0 gets mixer 1 (level 1.0)
	// for a total of 2.0f, while output 1 gets mixer 0 (level 0.25) for 0.5f.
	EXPECT_NEAR(2.0f, outSamples[0], 0.01f);
	EXPECT_NEAR(0.5f, outSamples[1], 0.01f);
}

// ---------------------------------------------------------------------------
// LoopTake flip-buffer tests
// ---------------------------------------------------------------------------

// AddLoop should stage into the back buffer. NumInputChannels reflects the
// back buffer while _changesMade && _flipLoopBuffer; after CommitChanges it
// reflects the (now-promoted) front buffer. Both readings should equal 1.
TEST(LoopTakeFlipBuffer, AddLoopStagesInBackBuffer)
{
	auto take = MakeLoopTake();

	// Before any AddLoop, both buffers are empty.
	EXPECT_EQ(0u, take->NumInputChannels(Audible::AUDIOSOURCE_ADC));

	// AddLoop stages a loop into the back buffer.
	take->AddLoop(0u, "station");

	// NumInputChannels now reads from the back buffer (changesMade == true,
	// flipLoopBuffer == true), so it should be 1.
	EXPECT_EQ(1u, take->NumInputChannels(Audible::AUDIOSOURCE_ADC));
}

// After CommitChanges the front buffer matches the back buffer, and
// _changesMade is cleared, so NumInputChannels now reads from the front.
TEST(LoopTakeFlipBuffer, CommitChangesFlipsLoopsToFront)
{
	auto take = MakeLoopTake();

	// Before any AddLoop, front buffer is empty.
	take->CommitChanges();  // commit while empty
	EXPECT_EQ(0u, take->NumInputChannels(Audible::AUDIOSOURCE_ADC));

	take->AddLoop(0u, "station");
	// Back buffer now has 1; front still has 0. NumInputChannels reads back.
	EXPECT_EQ(1u, take->NumInputChannels(Audible::AUDIOSOURCE_ADC));

	take->CommitChanges();
	// Front promoted; NumInputChannels still 1, now reading from front.
	EXPECT_EQ(1u, take->NumInputChannels(Audible::AUDIOSOURCE_ADC));
}

// Adding two loops on two different channels, then committing, should
// expose both channels through NumInputChannels.
TEST(LoopTakeFlipBuffer, MultiChannelAddLoopsThenCommit)
{
	auto take = MakeLoopTake();

	take->AddLoop(0u, "station");
	take->AddLoop(1u, "station");

	// Both loops are staged in the back buffer.
	EXPECT_EQ(2u, take->NumInputChannels(Audible::AUDIOSOURCE_ADC));

	take->CommitChanges();

	// After commit the front buffer has both loops.
	EXPECT_EQ(2u, take->NumInputChannels(Audible::AUDIOSOURCE_ADC));
}

// A second commit after no new AddLoop should leave NumInputChannels stable.
TEST(LoopTakeFlipBuffer, RepeatedCommitIsStable)
{
	auto take = MakeLoopTake();

	take->AddLoop(0u, "station");
	take->CommitChanges();
	EXPECT_EQ(1u, take->NumInputChannels(Audible::AUDIOSOURCE_ADC));

	// No new loops added; commit again.
	take->CommitChanges();
	EXPECT_EQ(1u, take->NumInputChannels(Audible::AUDIOSOURCE_ADC));
}

// ---------------------------------------------------------------------------
// Station flip-buffer tests
// ---------------------------------------------------------------------------

// Flush the initial audio-buffer flip that the Station constructor triggers
// via SetNumBusChannels, so subsequent assertions start from a clean state.
static void CommitInitial(const std::shared_ptr<Station>& station)
{
	station->CommitChanges();
}

// AddTake stages the take into the back buffer. NumTakes reflects the back
// buffer while _changesMade == true.
TEST(StationFlipBuffer, AddTakeStagesInBackBuffer)
{
	auto station = MakeStation();
	CommitInitial(station);

	// No takes yet; front buffer is empty.
	EXPECT_EQ(0u, station->NumTakes());

	station->AddTake();

	// NumTakes now reads from the back buffer (_changesMade == true),
	// so it should be 1.
	EXPECT_EQ(1u, station->NumTakes());
}

// After CommitChanges the front _loopTakes vector matches the back, and
// _changesMade is reset to false.
TEST(StationFlipBuffer, CommitChangesFlipsTakesToFront)
{
	auto station = MakeStation();
	CommitInitial(station);

	station->AddTake();
	ASSERT_EQ(1u, station->NumTakes());

	station->CommitChanges();

	// After commit, _changesMade == false: NumTakes reads from _loopTakes
	// (the promoted front buffer), which should still be 1.
	EXPECT_EQ(1u, station->NumTakes());
}

// Adding two takes before any commit should stage both in the back buffer.
// After a single CommitChanges, the front buffer holds both.
TEST(StationFlipBuffer, MultiTakeAddThenCommit)
{
	auto station = MakeStation();
	CommitInitial(station);

	station->AddTake();
	station->AddTake();

	// Both takes visible through the back buffer.
	EXPECT_EQ(2u, station->NumTakes());

	station->CommitChanges();

	// Both promoted to the front buffer.
	EXPECT_EQ(2u, station->NumTakes());
}

// NumBusChannels is driven by the audio-buffer flip. The constructor calls
// SetNumBusChannels(_DefaultNumBusChannels = 8), which stages 8 buffers in
// the back. After CommitChanges the front has 8.
TEST(StationFlipBuffer, NumBusChannelsFlipsAfterCommit)
{
	auto station = MakeStation();

	// Before the first commit: NumBusChannels reads from back (_changesMade
	// && _flipAudioBuffer == true).
	const auto numBus = station->NumBusChannels();
	EXPECT_GT(numBus, 0u);

	station->CommitChanges();

	// After commit: reads from front; the value should be unchanged.
	EXPECT_EQ(numBus, station->NumBusChannels());
}

// Verify that SetNumBusChannels followed by CommitChanges propagates the new
// channel count to the front buffer.
TEST(StationFlipBuffer, SetNumBusChannelsCommitUpdatesCount)
{
	auto station = MakeStation();
	CommitInitial(station);

	station->SetNumBusChannels(4u);
	// Staged in back.
	EXPECT_EQ(4u, station->NumBusChannels());

	station->CommitChanges();
	// Promoted to front.
	EXPECT_EQ(4u, station->NumBusChannels());
}

// Take added to a station should inherit the bus channel count set on the
// station. Station::AddTake calls take->SetNumBusChannels(NumBusChannels())
// at add-time, so the count is already set on the take before commit.
TEST(StationFlipBuffer, TakeInheritsBusChannelsAfterCommit)
{
	auto station = MakeStation();
	station->SetNumBusChannels(2u);
	CommitInitial(station);

	auto take = station->AddTake();
	station->CommitChanges();

	// The take should have 2 bus channels.
	EXPECT_EQ(2u, take->NumBusChannels());
}

TEST(StationFlipBuffer, RouterActionUpdatesEachChannelMixer)
{
	AssertStationRouterUpdateReassignsPerChannelMixer(GuiAction::ACTIONELEMENT_ROUTER);
}

TEST(StationFlipBuffer, RackConnectionsUpdateEachChannelMixer)
{
	AssertStationRouterUpdateReassignsPerChannelMixer(GuiAction::ACTIONELEMENT_RACK);
}

// ---------------------------------------------------------------------------
// LoopTake removal tests
// ---------------------------------------------------------------------------

// After AddLoop + CommitChanges, Ditch() clears the front buffer directly.
// NumInputChannels returns 0 because _loops is empty and _changesMade is false.
TEST(LoopTakeFlipBuffer, DitchClearsAllLoopsAfterCommit)
{
	auto take = MakeLoopTake();

	take->AddLoop(0u, "station");
	take->CommitChanges();
	ASSERT_EQ(1u, take->NumInputChannels(Audible::AUDIOSOURCE_ADC));

	take->Ditch();

	// Ditch clears _loops directly; _changesMade is not set by Ditch, so
	// NumInputChannels reads from the (now-empty) front buffer.
	EXPECT_EQ(0u, take->NumInputChannels(Audible::AUDIOSOURCE_ADC));
}

// Ditch with multiple committed loops clears all channels.
TEST(LoopTakeFlipBuffer, DitchClearsMultipleLoopsAfterCommit)
{
	auto take = MakeLoopTake();

	take->AddLoop(0u, "station");
	take->AddLoop(1u, "station");
	take->CommitChanges();
	ASSERT_EQ(2u, take->NumInputChannels(Audible::AUDIOSOURCE_ADC));

	take->Ditch();

	EXPECT_EQ(0u, take->NumInputChannels(Audible::AUDIOSOURCE_ADC));
}

// Ditch on a take that has never had loops added is a no-op.
TEST(LoopTakeFlipBuffer, DitchIsNoOpOnEmptyTake)
{
	auto take = MakeLoopTake();
	ASSERT_EQ(0u, take->NumInputChannels(Audible::AUDIOSOURCE_ADC));

	take->Ditch();

	EXPECT_EQ(0u, take->NumInputChannels(Audible::AUDIOSOURCE_ADC));
}

// ---------------------------------------------------------------------------
// Station removal tests
// ---------------------------------------------------------------------------

// TRIGGER_DITCH stages the take removal in the back buffer. Before commit,
// NumTakes reads the back buffer (which has the take erased), so it returns 0.
TEST(StationFlipBuffer, DitchActionStagesTakeRemovalInBackBuffer)
{
	auto station = MakeStation();
	CommitInitial(station);

	auto take = station->AddTake();
	station->CommitChanges();
	ASSERT_EQ(1u, station->NumTakes());

	TriggerAction ditch;
	ditch.ActionType = TriggerAction::TRIGGER_DITCH;
	ditch.TargetId = take->Id();
	station->OnAction(ditch);

	// NumTakes reads _backLoopTakes (_changesMade == true); the take was
	// erased from the back buffer, so the count drops to 0.
	EXPECT_EQ(0u, station->NumTakes());
}

// After CommitChanges following TRIGGER_DITCH, the front buffer is promoted
// and NumTakes reads the (now-empty) front buffer.
TEST(StationFlipBuffer, DitchActionCommitRemovesTakeFromFront)
{
	auto station = MakeStation();
	CommitInitial(station);

	auto take = station->AddTake();
	station->CommitChanges();

	TriggerAction ditch;
	ditch.ActionType = TriggerAction::TRIGGER_DITCH;
	ditch.TargetId = take->Id();
	station->OnAction(ditch);
	ASSERT_EQ(0u, station->NumTakes());

	station->CommitChanges();

	// After commit: _loopTakes = _backLoopTakes = {}; NumTakes reads front.
	EXPECT_EQ(0u, station->NumTakes());
}

// Ditching one of two committed takes stages only that take's removal;
// the other remains in both back and (after commit) front buffers.
TEST(StationFlipBuffer, DitchOneOfMultipleTakesReducesCount)
{
	auto station = MakeStation();
	CommitInitial(station);

	auto take0 = station->AddTake();
	station->AddTake();
	station->CommitChanges();
	ASSERT_EQ(2u, station->NumTakes());

	TriggerAction ditch;
	ditch.ActionType = TriggerAction::TRIGGER_DITCH;
	ditch.TargetId = take0->Id();
	station->OnAction(ditch);

	// Back buffer has 1 take remaining.
	EXPECT_EQ(1u, station->NumTakes());

	station->CommitChanges();

	// Front buffer promoted with 1 take.
	EXPECT_EQ(1u, station->NumTakes());
}
