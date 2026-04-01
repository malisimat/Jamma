
#include "gtest/gtest.h"
#include "engine/LoopTake.h"
#include "engine/Station.h"

using engine::LoopTake;
using engine::LoopTakeParams;
using engine::Station;
using engine::StationParams;
using audio::MergeMixBehaviourParams;
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
// Since we cannot directly see the front/back buffer, passing
// does not directly guarantee takes in specific front/back buffers.
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
// station at the time of commit.
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
