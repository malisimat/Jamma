
#include "gtest/gtest.h"
#include <thread>
#include "base/AudioSink.h"
#include "actions/TriggerAction.h"
#include "engine/LoopTake.h"
#include "engine/Station.h"
#include "midi/MidiLoop.h"

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

class TestLoopTake :
	public LoopTake
{
public:
	TestLoopTake(LoopTakeParams params, audio::AudioMixerParams mixerParams) :
		LoopTake(params, mixerParams)
	{
	}

	std::size_t ChildCount() const
	{
		return _children.size();
	}
	std::shared_ptr<base::AudioSink> CaptureChannel(Audible::AudioSourceType source)
	{
		return _InputChannel(0u, source);
	}

	const std::vector<std::pair<unsigned int, unsigned int>>& Routes() const
	{
		return _guiRack->Routes();
	}

};

static std::shared_ptr<TestLoopTake> MakeTestLoopTake(const std::string& id = "take-0")
{
	LoopTakeParams params;
	params.Id = id;
	params.Size = { 100, 100 };
	MergeMixBehaviourParams merge;
	auto mixerParams = LoopTake::GetMixerParams(params.Size, merge);
	return std::make_shared<TestLoopTake>(params, mixerParams);
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

	const std::vector<std::pair<unsigned int, unsigned int>>& Routes() const
	{
		return _guiRack->Routes();
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
	// Each wire mixer sends its source to one output. After swapping routes
	// {(0,1), (1,0)}, output 0 receives mixer 1 and output 1 receives mixer 0.
	EXPECT_NEAR(1.0f, outSamples[0], 0.01f);
	EXPECT_NEAR(0.25f, outSamples[1], 0.01f);
}

// AddLoop should stage into the back buffer. NumInputChannels reflects the
// back buffer while _changesMade && _flipLoopBuffer; after CommitChanges it
// reflects the (now-promoted) front buffer. Both readings should equal 1.
TEST(LoopTakeFlipBuffer, AddLoopStagesInBackBuffer)
{
	auto take = MakeTestLoopTake();

	// Before any AddLoop, both buffers are empty.
	EXPECT_EQ(0u, take->NumInputChannels(Audible::AUDIOSOURCE_ADC));
	EXPECT_EQ(1u, take->ChildCount());

	// AddLoop stages a loop into the back buffer.
	take->AddLoop(0u, "station");

	// NumInputChannels now reads from the back buffer (changesMade == true,
	// flipLoopBuffer == true), so it should be 1.
	EXPECT_EQ(1u, take->NumInputChannels(Audible::AUDIOSOURCE_ADC));
	EXPECT_EQ(1u, take->ChildCount());
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

TEST(LoopTakeRouting, LegacyConstructionOrderAppliesVisibleDefaultRouteToAudio)
{
	auto take = MakeTestLoopTake();
	take->AddLoop(0u, "station");
	ASSERT_EQ((std::vector<std::pair<unsigned int, unsigned int>>{ { 0u, 0u } }), take->Routes());

	// Loaded loops are created before Station::AddTake supplies the bus count.
	take->SetNumBusChannels(2u);
	take->CommitChanges();

	ASSERT_EQ((std::vector<std::vector<unsigned long>>{ { 0u } }), take->SnapshotAudioRoutesForExport());
	EXPECT_EQ((std::vector<std::pair<unsigned int, unsigned int>>{ { 0u, 0u } }), take->Routes());
}

TEST(LoopTakeRouting, ExplicitlyEmptySavedRoutesOverrideOneToOneDefaults)
{
	auto take = MakeTestLoopTake();
	take->AddLoop(0u, "station");
	take->SetNumBusChannels(2u);
	ASSERT_TRUE(take->RestoreAudioRoutes({}));
	take->CommitChanges();

	EXPECT_EQ((std::vector<std::vector<unsigned long>>{ {} }), take->SnapshotAudioRoutesForExport());
	EXPECT_TRUE(take->Routes().empty());
}

TEST(StationRouting, RestoreUpdatesPendingAndActiveMixersAndGui)
{
	auto station = MakeTestStation();
	station->SetNumBusChannels(2u);
	station->SetNumDacChannels(2u);
	ASSERT_TRUE(station->RestoreAudioRoutes({ { 1u }, {} }));
	station->CommitChanges();

	EXPECT_EQ((std::vector<std::vector<unsigned long>>{ { 1u }, {} }), station->SnapshotAudioRoutesForExport());
	EXPECT_EQ((std::vector<std::pair<unsigned int, unsigned int>>{ { 0u, 1u } }), station->Routes());
	auto pendingOutput = ReadStationOutput(station, { 0.25f, 1.0f });
	EXPECT_NEAR(0.0f, pendingOutput[0], 0.01f);
	EXPECT_NEAR(0.25f, pendingOutput[1], 0.01f);

	ASSERT_TRUE(station->RestoreAudioRoutes({ {}, { 0u } }));
	EXPECT_EQ((std::vector<std::vector<unsigned long>>{ {}, { 0u } }), station->SnapshotAudioRoutesForExport());
	EXPECT_EQ((std::vector<std::pair<unsigned int, unsigned int>>{ { 1u, 0u } }), station->Routes());
	auto activeOutput = ReadStationOutput(station, { 0.25f, 1.0f });
	EXPECT_NEAR(1.0f, activeOutput[0], 0.01f);
	EXPECT_NEAR(0.0f, activeOutput[1], 0.01f);
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
	auto take = MakeTestLoopTake();

	take->AddLoop(0u, "station");
	take->CommitChanges();
	ASSERT_EQ(1u, take->NumInputChannels(Audible::AUDIOSOURCE_ADC));
	ASSERT_EQ(2u, take->ChildCount());

	take->Ditch();

	// Ditch clears _loops directly; _changesMade is not set by Ditch, so
	// NumInputChannels reads from the (now-empty) front buffer.
	EXPECT_EQ(0u, take->NumInputChannels(Audible::AUDIOSOURCE_ADC));
	EXPECT_EQ(1u, take->ChildCount());
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

class CommitPublicationTestTake : public LoopTake
{
public:
	CommitPublicationTestTake(LoopTakeParams params, audio::AudioMixerParams mixerParams)
		: LoopTake(params, mixerParams) {}
	void MarkDirty() { _changesMade.store(true, std::memory_order_release); }
	unsigned Commits = 0u;
	bool SawCommitContext = false;
protected:
	std::vector<actions::JobAction> _CommitChanges() override
	{
		++Commits;
		SawCommitContext = _HasUncommittedChanges();
		if (Commits == 1u)
			{
			// Producer completes during commit; there is no later audio tick.
			std::thread producer([this] { MarkDirty(); });
			producer.join();
		}
		return {};
	}
};

TEST(LoopTakeCompletionHandoff, PublicationDuringCommitSurvivesWithoutAnotherAudioTick)
{
	LoopTakeParams params;
	params.Id = "commit-publication";
	params.Size = {100, 100};
	MergeMixBehaviourParams merge;
	auto take = std::make_shared<CommitPublicationTestTake>(params, LoopTake::GetMixerParams(params.Size, merge));
	take->MarkDirty();
	take->CommitChanges();
	EXPECT_TRUE(take->SawCommitContext);
	take->CommitChanges();
	EXPECT_EQ(2u, take->Commits);
	take->CommitChanges();
	EXPECT_EQ(2u, take->Commits);
}

TEST(LoopTakeCompletionHandoff, TailPublishesOnceAndStaleJobCannotFinishReusedTake)
{
	auto take = MakeLoopTake("completion-generation");
	take->Record({0u}, "station");
	take->CommitChanges();
	take->Play(0u, 100u, 1u);
	take->EndMultiWrite(1u, true, Audible::AUDIOSOURCE_ADC);
	const auto firstJobs = take->CommitChanges();
	actions::JobAction completion;
	unsigned completions = 0u;
	for (const auto& job : firstJobs)
		if (job.JobActionType == actions::JobAction::JOB_ENDRECORDING)
		{
			completion = job;
			++completions;
		}
	ASSERT_EQ(1u, completions);
	take->EndMultiWrite(1u, true, Audible::AUDIOSOURCE_ADC);
	for (const auto& job : take->CommitChanges())
		EXPECT_NE(actions::JobAction::JOB_ENDRECORDING, job.JobActionType);
	// This test has stopped callback readers before destructive reuse.
	take->Ditch();
	EXPECT_EQ(LoopTake::PresentationMode::Inactive, take->GetPresentation().Mode);
	take->Record({0u}, "station");
	take->CommitChanges();
	take->Play(0u, 100u, 10u);
	take->OnAction(completion);
	EXPECT_EQ(LoopTake::PresentationMode::Tail, take->GetPresentation().Mode);
	EXPECT_TRUE(take->IsArmed());
}

TEST(LoopTakeCompletionHandoff, CancelledPunchCannotRemainArmedOrBeResurrected)
{
	auto take = MakeLoopTake("cancelled-punch");
	take->Overdub({0u}, "station");
	take->CommitChanges();
	take->TriggerPunchInAudio();
	ASSERT_TRUE(take->IsArmed());
	take->CancelCapture();
	EXPECT_FALSE(take->IsArmed());
	EXPECT_EQ(LoopTake::PresentationMode::Inactive, take->GetPresentation().Mode);
	take->TriggerPunchInAudio();
	take->TriggerPunchOutAudio();
	EXPECT_FALSE(take->IsArmed());
}

TEST(LoopTakeCompletionHandoff, CancelledTakeRejectsStaleSnapshotCaptureAndUpdateJobs)
{
	auto take = MakeTestLoopTake("cancelled-snapshot");
	take->Record({0u}, "station");
	const auto jobs = take->CommitChanges();
	ASSERT_EQ(1u, take->GetLoops().size());
	const auto loop = take->GetLoops().front();
	ASSERT_TRUE(take->CaptureChannel(Audible::AUDIOSOURCE_ADC));
	take->EndMultiWrite(32u, true, Audible::AUDIOSOURCE_ADC);
	const auto length = loop->PhysicalLoopLength();
	const auto recorded = take->NumRecordedSamps();
	take->CancelCapture();
	EXPECT_FALSE(take->CaptureChannel(Audible::AUDIOSOURCE_ADC));
	EXPECT_FALSE(take->CaptureChannel(Audible::AUDIOSOURCE_MONITOR));
	EXPECT_FALSE(take->CaptureChannel(Audible::AUDIOSOURCE_BOUNCE));
	take->Zero(32u, Audible::AUDIOSOURCE_ADC);
	take->EndMultiWrite(32u, true, Audible::AUDIOSOURCE_ADC);
	EXPECT_EQ(length, loop->PhysicalLoopLength());
	EXPECT_EQ(recorded, take->NumRecordedSamps());
	for (const auto& job : jobs)
		if (job.JobActionType == actions::JobAction::JOB_UPDATELOOPS)
			EXPECT_FALSE(take->OnAction(job).IsEaten);
}

class CancelledTakeMidiSink final : public midi::IMidiOutputSink
{
public:
	void OnEvent(unsigned int, const midi::MidiEvent& event) noexcept override
	{
		if (event.IsNoteOn()) ++NoteOns;
		if (event.IsNoteOff()) ++NoteOffs;
	}
	unsigned int NoteOns = 0u;
	unsigned int NoteOffs = 0u;
};

class CancelledTakeBounceWriter final : public base::BounceWriter
{
public:
	void WriteBlock(const std::shared_ptr<base::MultiAudioSink>, const float*,
		unsigned int, unsigned int) override { ++Writes; }
	unsigned int Writes = 0u;
};

TEST(LoopTakeCompletionHandoff, CancelledTakeCannotReplayAudioFromRetainedSnapshot)
{
	auto take = MakeLoopTake("cancelled-audio-playback");
	take->Record({0u}, "station");
	take->CommitChanges();
	take->EndMultiWrite(100u, true, Audible::AUDIOSOURCE_ADC);
	take->Play(0u, 100u, 0u);
	auto sink = std::make_shared<CaptureMultiSink>(1u);
	auto writer = std::make_shared<CancelledTakeBounceWriter>();
	take->WriteBlock(sink, writer, 0, 1u);
	ASSERT_GT(writer->Writes, 0u);
	writer->Writes = 0u;
	take->CancelCapture();
	take->WriteBlock(sink, writer, 0, 1u);
	take->EndMultiPlay(1u);
	take->WriteBlock(sink, writer, 0, 1u);
	EXPECT_EQ(0u, writer->Writes);
}

TEST(LoopTakeCompletionHandoff, CancelledTakeFlushesHeldMidiWithoutReplayingOldSnapshot)
{
	auto take = MakeLoopTake("cancelled-midi-snapshot");
	take->Record({}, "station", {0u}, {"Keys"});
	ASSERT_TRUE(take->RecordMidiEvent(midi::MidiEvent::MakeNoteOn(0u, 0u, 60u, 100u), "Keys", 0u));
	take->Play(0u, 128u, 0u);
	CancelledTakeMidiSink sink;
	take->ReadMidiBlock(0u, 32u, sink, 0u);
	ASSERT_EQ(1u, sink.NoteOns);
	ASSERT_EQ(0u, sink.NoteOffs);
	take->CancelCapture();
	EXPECT_FALSE(take->RecordMidiEvent(midi::MidiEvent::MakeNoteOn(32u, 0u, 62u, 100u), "Keys", 32u));
	// A retained Station snapshot still calls this take; only its held note is released.
	take->ReadMidiBlock(32u, 32u, sink, 0u);
	EXPECT_EQ(1u, sink.NoteOns);
	EXPECT_EQ(1u, sink.NoteOffs);
	take->ReadMidiBlock(64u, 128u, sink, 0u);
	take->ReadMidiBlock(192u, 128u, sink, 0u);
	EXPECT_EQ(1u, sink.NoteOns);
	EXPECT_EQ(1u, sink.NoteOffs);
}

TEST(LoopTakeCompletionHandoff, RestoredContentsPlayBeforeTakeCaptureLifecycle)
{
	auto take = MakeLoopTake("restored-contents");
	take->SetNumBusChannels(1u);
	audio::WireMixBehaviourParams wire;
	wire.Channels = {0u};
	audio::AudioMixerParams mixer;
	mixer.Behaviour = wire;
	engine::LoopParams params;
	auto loop = std::make_shared<engine::Loop>(params, mixer);
	loop->Record();
	std::vector<float> samples(constants::MaxLoopFadeSamps + 128u, 0.75f);
	AudioWriteRequest request;
	request.samples = samples.data();
	request.numSamps = static_cast<unsigned int>(samples.size());
	request.stride = 1u;
	request.fadeNew = 1.f;
	request.source = Audible::AUDIOSOURCE_ADC;
	loop->OnBlockWrite(request, 0);
	loop->EndWrite(request.numSamps, true);
	loop->Play(constants::MaxLoopFadeSamps, 128u, false);
	take->AddLoop(loop);
	LoopTake::MidiExportState midiState;
	midiState.LoopLengthSamps = 128u;
	LoopTake::MidiStreamExport stream;
	stream.Loop.LoopLengthSamps = 128u;
	stream.Loop.EventCount = 1u;
	stream.Loop.Events[0] = midi::MidiEvent::MakeNoteOn(0u, 0u, 60u, 100u);
	midiState.Streams.push_back(stream);
	ASSERT_TRUE(take->RestoreMidiFromExport(midiState));
	take->CommitChanges();
	EXPECT_EQ(LoopTake::STATE_INACTIVE, take->TakeState());
	auto audioSink = std::make_shared<CaptureMultiSink>(1u);
	take->WriteBlock(audioSink, nullptr, 0, 1u);
	EXPECT_FLOAT_EQ(0.75f, audioSink->Sample(0u));
	CancelledTakeMidiSink midiSink;
	EXPECT_EQ(1u, take->ReadMidiBlock(0u, 32u, midiSink));
	EXPECT_EQ(1u, midiSink.NoteOns);
	take->CancelCapture();
	take->ReadMidiBlock(32u, 32u, midiSink);
	EXPECT_EQ(1u, midiSink.NoteOffs);
}

TEST(LoopTakeCompletionHandoff, RapidRecordEndKeepsPreparedAudioAndMixedTails)
{
	for (const bool withMidi : {false, true})
	{
		auto take = MakeLoopTake(withMidi ? "rapid-mixed" : "rapid-audio");
		take->Record({0u}, "station", withMidi ? std::vector<unsigned int>{0u} : std::vector<unsigned int>{},
			withMidi ? std::vector<std::string>{"Keys"} : std::vector<std::string>{});
		ASSERT_TRUE(take->GetLoops().empty()); // No UI publication between START and END.
		if (withMidi)
			ASSERT_TRUE(take->RecordMidiEvent(midi::MidiEvent::MakeNoteOn(0u, 0u, 60u, 100u), "Keys", 0u));
		take->Play(constants::MaxLoopFadeSamps, 64u, 32u);
		EXPECT_EQ(LoopTake::STATE_PLAYINGRECORDING, take->TakeState());
		take->CommitChanges();
		ASSERT_EQ(1u, take->GetLoops().size());
		EXPECT_EQ(engine::Loop::STATE_PLAYINGRECORDING, take->GetLoops()[0]->PlayState());
		EXPECT_EQ(64u, take->GetLoops()[0]->LoopLength());
		if (withMidi)
			EXPECT_EQ(midi::MidiLoopState::Playing, take->GetMidiLoopSnapshot()[0]->State());
		take->EndMultiWrite(32u, true, Audible::AUDIOSOURCE_ADC);
		take->EndRecording();
		EXPECT_EQ(LoopTake::STATE_PLAYING, take->TakeState());
		EXPECT_EQ(engine::Loop::STATE_PLAYING, take->GetLoops()[0]->PlayState());
	}
}

TEST(LoopTakeCompletionHandoff, RapidOverdubRetainsUnpublishedSourceAudioSlots)
{
	auto source = MakeLoopTake("rapid-source");
	source->Record({0u, 1u}, "station");
	source->Play(constants::MaxLoopFadeSamps, 64u, 32u);
	ASSERT_TRUE(source->GetLoops().empty());
	auto target = MakeLoopTake("rapid-overdub");
	target->Overdub({}, "station", {}, {}, source);
	EXPECT_TRUE(target->HasTriggerAudioCapture());
	target->Play(constants::MaxLoopFadeSamps, 64u, 64u);
	EXPECT_EQ(LoopTake::STATE_OVERDUBBINGRECORDING, target->TakeState());
	source->CommitChanges();
	target->CommitChanges();
	ASSERT_EQ(2u, target->GetLoops().size());
	for (const auto& loop : target->GetLoops())
		EXPECT_EQ(engine::Loop::STATE_OVERDUBBINGRECORDING, loop->PlayState());
	source->EndMultiWrite(32u, true, Audible::AUDIOSOURCE_ADC);
	source->EndRecording();
	EXPECT_EQ(LoopTake::STATE_PLAYING, source->TakeState());
	EXPECT_EQ(LoopTake::STATE_OVERDUBBINGRECORDING, target->TakeState());
	target->EndMultiWrite(64u, true, Audible::AUDIOSOURCE_BOUNCE);
	target->EndRecording();
	EXPECT_EQ(LoopTake::STATE_PLAYING, target->TakeState());
	for (const auto& loop : target->GetLoops())
		EXPECT_EQ(engine::Loop::STATE_PLAYING, loop->PlayState());
}

TEST(CaptureSourceMute, SharedClaimsFlushMidiOnceAndPreserveReplacementOwnership)
{
	auto source = MakeLoopTake("claimed-midi");
	source->Record({}, "station", {0u}, {"Keys"});
	ASSERT_TRUE(source->RecordMidiEvent(midi::MidiEvent::MakeNoteOn(0u, 0u, 60u, 100u), "Keys", 0u));
	source->Play(0u, 128u, 0u);
	CancelledTakeMidiSink sink;
	source->ReadMidiBlock(0u, 32u, sink);
	ASSERT_EQ(1u, sink.NoteOns);
	auto first = MakeLoopTake("replacement-one");
	auto second = MakeLoopTake("replacement-two");
	first->AcquireReplacementSourceMute(source);
	first->AcquireReplacementSourceMute(source); // One owned claim, even repeated acknowledgement.
	second->AcquireReplacementSourceMute(source);
	source->ReadMidiBlock(32u, 32u, sink);
	EXPECT_EQ(1u, sink.NoteOffs);
	first->CancelCapture();
	EXPECT_TRUE(source->IsMuted());
	source->ReadMidiBlock(64u, 128u, sink);
	EXPECT_EQ(1u, sink.NoteOns);
	EXPECT_EQ(1u, sink.NoteOffs);
	second.reset();
	EXPECT_FALSE(source->IsMuted());
	first->AcquireReplacementSourceMute(source);
	source->AcquireTriggerSourceMuteAudio();
	EXPECT_TRUE(source->UnMute());
	EXPECT_TRUE(source->IsMuted()); // Explicit audition never overrides a live punch owner.
	source->ReleaseTriggerSourceMuteAudio();
	EXPECT_FALSE(source->IsMuted());
	first->AcquireReplacementSourceMute(source); // Replayed acknowledgement cannot re-mute audition.
	EXPECT_FALSE(source->IsMuted());
	auto third = MakeLoopTake("new-replacement");
	third->AcquireReplacementSourceMute(source);
	EXPECT_TRUE(source->IsMuted());
	source->SetPickingFromState(base::GuiElement::EDIT_MUTE, true);
	EXPECT_FALSE(source->IsPicking3d());
	source->SetStateFromPicking(base::GuiElement::EDIT_MUTE, false);
	EXPECT_FALSE(source->IsMuted());
	third->CancelCapture();
	first->CancelCapture();
	EXPECT_FALSE(source->IsMuted());
	source->Mute();
	source->AcquireTriggerSourceMuteAudio();
	source->ReleaseTriggerSourceMuteAudio();
	EXPECT_TRUE(source->IsMuted());
}

TEST(CaptureSourceMute, IndividualLoopAuditionDoesNotOverrideOtherChannelsOrPunch)
{
	auto source = MakeLoopTake("channel-audition-source");
	source->Record({0u, 1u}, "station");
	source->CommitChanges();
	ASSERT_EQ(2u, source->GetLoops().size());
	auto first = MakeLoopTake("channel-replacement-one");
	first->AcquireReplacementSourceMute(source);
	EXPECT_TRUE(source->GetLoops()[0]->IsMuted());
	EXPECT_TRUE(source->GetLoops()[1]->IsMuted());
	EXPECT_TRUE(source->GetLoops()[0]->UnMute());
	EXPECT_FALSE(source->GetLoops()[0]->IsMuted());
	EXPECT_TRUE(source->GetLoops()[1]->IsMuted());
	EXPECT_TRUE(source->IsMuted()); // MIDI and the other source channel remain muted.
	source->AcquireTriggerSourceMuteAudio();
	EXPECT_TRUE(source->GetLoops()[0]->IsMuted());
	source->GetLoops()[0]->UnMute();
	EXPECT_TRUE(source->GetLoops()[0]->IsMuted());
	source->ReleaseTriggerSourceMuteAudio();
	EXPECT_FALSE(source->GetLoops()[0]->IsMuted());
	auto second = MakeLoopTake("channel-replacement-two");
	second->AcquireReplacementSourceMute(source);
	EXPECT_TRUE(source->GetLoops()[0]->IsMuted());
	first->CancelCapture();
	EXPECT_TRUE(source->GetLoops()[0]->IsMuted());
	second->CancelCapture();
	EXPECT_FALSE(source->GetLoops()[0]->IsMuted());
}

TEST(StationFlipBuffer, DitchKeepsBorrowedBuffersUntilSnapshotRetirement)
{
	auto station = MakeStation();
	TriggerAction start;
	start.ActionType = TriggerAction::TRIGGER_REC_START;
	start.InputChannels = {0u};
	const auto removed = station->OnAction(start);
	station->CommitChanges();
	auto retained = station->GetLoopTakeSnapshot();
	ASSERT_EQ(1u, retained.size());
	auto take = retained.front();
	ASSERT_EQ(1u, take->GetLoops().size());
	auto loop = take->GetLoops().front();
	const std::weak_ptr<base::GuiElement> rackLifetime = take->GetGuiRack();
	const std::weak_ptr<base::GuiElement> modelLifetime = loop->Model();
	take->EndMultiWrite(32u, true, Audible::AUDIOSOURCE_ADC);
	const auto physicalLength = loop->PhysicalLoopLength();
	const auto recorded = take->NumRecordedSamps();
	const std::weak_ptr<LoopTake> lifetime = take;
	const auto other = station->OnAction(start);
	station->CommitChanges();
	TriggerAction ditch;
	ditch.ActionType = TriggerAction::TRIGGER_DITCH;
	ditch.TargetId = removed.TargetId;
	ASSERT_EQ(actions::DitchDisposition::Removed, station->OnAction(ditch).DitchResult);
	ASSERT_EQ(1u, take->GetLoops().size());
	EXPECT_EQ(physicalLength, loop->PhysicalLoopLength());
	EXPECT_EQ(LoopTake::STATE_INACTIVE, take->TakeState());
	EXPECT_EQ(engine::StationVisualState::STATIONSTATE_RECORDING, station->GetVisualState());
	ASSERT_EQ(1u, station->GetLoopTakeSnapshot().size());
	EXPECT_EQ(other.TargetId, station->GetLoopTakeSnapshot().front()->Id());
	// A previously acquired callback snapshot can finish using stable resources.
	take->EndMultiWrite(32u, true, Audible::AUDIOSOURCE_ADC);
	EXPECT_EQ(recorded, take->NumRecordedSamps());
	EXPECT_EQ(physicalLength, loop->PhysicalLoopLength());
	station->CommitChanges();
	take.reset();
	retained.clear();
	station->ReleaseRetiredAudioStates();
	EXPECT_FALSE(lifetime.expired());
	station->AcknowledgeAudioBoundary();
	EXPECT_FALSE(lifetime.expired());
	// Only the off-callback retirement owner drops the last take reference.
	station->ReleaseRetiredAudioStates();
	EXPECT_TRUE(lifetime.expired());
	EXPECT_TRUE(rackLifetime.expired());
	EXPECT_FALSE(modelLifetime.expired()); // The explicit loop borrower is still retained.
	loop.reset();
	EXPECT_TRUE(modelLifetime.expired());
}

TEST(StationFlipBuffer, ZeroLengthEndRemovesCancelledTakeWithoutResettingBuffers)
{
	for (const bool overdub : {false, true})
	{
		auto station = MakeStation();
		TriggerAction start;
		start.ActionType = overdub ? TriggerAction::TRIGGER_OVERDUB_START : TriggerAction::TRIGGER_REC_START;
		start.InputChannels = {0u};
		const auto started = station->OnAction(start);
		station->CommitChanges();
		const auto retained = station->GetLoopTakeSnapshot();
		ASSERT_EQ(1u, retained.size());
		ASSERT_EQ(1u, retained.front()->GetLoops().size());
		TriggerAction end;
		end.ActionType = overdub ? TriggerAction::TRIGGER_OVERDUB_END : TriggerAction::TRIGGER_REC_END;
		end.TargetId = started.TargetId;
		end.SampleCount = 0u;
		ASSERT_TRUE(station->OnAction(end).IsEaten);
		EXPECT_TRUE(station->GetLoopTakeSnapshot().empty());
		EXPECT_EQ(0u, station->NumTakes());
		EXPECT_EQ(LoopTake::STATE_INACTIVE, retained.front()->TakeState());
		EXPECT_EQ(1u, retained.front()->GetLoops().size());
		EXPECT_EQ(engine::StationVisualState::STATIONSTATE_DEFAULT, station->GetVisualState());
	}
}

TEST(StationFlipBuffer, ResetCancelsRetainedCaptureWithoutClearingItsBuffers)
{
	auto station = MakeStation();
	TriggerAction start;
	start.ActionType = TriggerAction::TRIGGER_REC_START;
	start.InputChannels = {0u};
	station->OnAction(start);
	station->CommitChanges();
	const auto retained = station->GetLoopTakeSnapshot();
	ASSERT_EQ(1u, retained.size());
	const auto& take = retained.front();
	ASSERT_EQ(1u, take->GetLoops().size());
	take->EndMultiWrite(32u, true, Audible::AUDIOSOURCE_ADC);
	const auto length = take->GetLoops().front()->PhysicalLoopLength();
	station->Reset();
	EXPECT_TRUE(station->GetLoopTakeSnapshot().empty());
	EXPECT_EQ(LoopTake::STATE_INACTIVE, take->TakeState());
	EXPECT_FALSE(take->IsArmed());
	ASSERT_EQ(1u, take->GetLoops().size());
	take->EndMultiWrite(32u, true, Audible::AUDIOSOURCE_ADC);
	EXPECT_EQ(length, take->GetLoops().front()->PhysicalLoopLength());
	EXPECT_FALSE(take->GetGuiRack()->GetReceiver());
	EXPECT_FALSE(take->GetGuiRack()->Parent());
}
