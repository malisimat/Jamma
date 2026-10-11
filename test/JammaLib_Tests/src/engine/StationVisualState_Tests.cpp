#include "gtest/gtest.h"
#include <atomic>
#include <thread>

#include "actions/TriggerAction.h"
#include "engine/LoopTake.h"
#include "engine/Station.h"
#include "engine/Trigger.h"
#include "midi/MidiLoop.h"
#include "utils/Timer.h"

using actions::TriggerAction;
using engine::Station;
using engine::StationParams;
using engine::StationVisualState;
using engine::Trigger;
using engine::TriggerParams;

static std::shared_ptr<Station> MakeStation(const std::string& name)
	{
	StationParams params;
	params.Name = name;
	params.Size = { 100u, 100u };
	audio::MergeMixBehaviourParams merge;
	return std::make_shared<Station>(params, Station::GetMixerParams(params.Size, merge));
}

static TriggerAction MakeTriggerAction(TriggerAction::TriggerActionType type,
	unsigned long sampleCount = 0u)
{
	TriggerAction action;
	action.ActionType = type;
	action.SampleCount = sampleCount;
	return action;
}

TEST(StationVisualState, StartsDefault)
{
	auto station = MakeStation("station");

	EXPECT_EQ(StationVisualState::STATIONSTATE_DEFAULT, station->GetVisualState());
}

TEST(StationVisualState, RecordingEndRemainsVisibleUntilTheTakeFinishes)
{
	auto station = MakeStation("station");
	io::UserConfig config{};
	auto start = MakeTriggerAction(TriggerAction::TRIGGER_REC_START);
	start.InputChannels = { 0u };
	const auto started = station->OnAction(start);
	station->CommitChanges();

	EXPECT_EQ(StationVisualState::STATIONSTATE_RECORDING, station->GetVisualState());

	auto end = MakeTriggerAction(TriggerAction::TRIGGER_REC_END, 64u);
	end.TargetId = started.TargetId;
	end.SetUserConfig(config);
	station->OnAction(end);
	EXPECT_EQ(StationVisualState::STATIONSTATE_ENDRECORDING, station->GetVisualState());
	ASSERT_EQ(1u, station->GetLoopTakes().size());
	EXPECT_EQ(engine::LoopTake::STATE_PLAYINGRECORDING, station->GetLoopTakes()[0]->TakeState());
}

TEST(StationVisualState, MidiOnlyRecordingFinishesImmediatelyWithAnAudioTailConfigured)
{
	auto station = MakeStation("station");
	station->SetAllowedMidiChannels({ 1 });
	io::UserConfig config{};
	ASSERT_GT(config.EndRecordingSamps(0), 0u);
	auto start = MakeTriggerAction(TriggerAction::TRIGGER_REC_START);
	start.MidiInputDevices = { "Keys" };
	const auto started = station->OnAction(start);
	station->CommitChanges();
	ASSERT_EQ(1u, station->GetLoopTakes().size());
	const auto take = station->GetLoopTakes()[0];
	ASSERT_TRUE(take->GetLoops().empty());
	ASSERT_FALSE(take->GetMidiLoops().empty());

	auto end = MakeTriggerAction(TriggerAction::TRIGGER_REC_END, 64u);
	end.TargetId = started.TargetId;
	end.SetUserConfig(config);
	station->OnAction(end);

	EXPECT_EQ(engine::LoopTake::STATE_PLAYING, take->TakeState());
	EXPECT_EQ(StationVisualState::STATIONSTATE_DEFAULT, station->GetVisualState());
	for (const auto& loop : take->GetMidiLoops())
	{
		EXPECT_EQ(midi::MidiLoopState::Playing, loop->State());
		EXPECT_EQ(64u, loop->LoopLengthSamps());
	}
}

TEST(StationVisualState, MixedRecordingPreservesAudioTailAndFinalizesMidiImmediately)
{
	auto station = MakeStation("station");
	station->SetAllowedMidiChannels({ 1 });
	io::UserConfig config{};
	auto start = MakeTriggerAction(TriggerAction::TRIGGER_REC_START);
	start.InputChannels = { 0u };
	start.MidiInputDevices = { "Keys" };
	const auto started = station->OnAction(start);
	station->CommitChanges();
	ASSERT_EQ(1u, station->GetLoopTakes().size());
	const auto take = station->GetLoopTakes()[0];
	ASSERT_FALSE(take->GetLoops().empty());
	ASSERT_FALSE(take->GetMidiLoops().empty());

	auto end = MakeTriggerAction(TriggerAction::TRIGGER_REC_END, 64u);
	end.TargetId = started.TargetId;
	end.SetUserConfig(config);
	station->OnAction(end);

	EXPECT_EQ(engine::LoopTake::STATE_PLAYINGRECORDING, take->TakeState());
	EXPECT_EQ(StationVisualState::STATIONSTATE_ENDRECORDING, station->GetVisualState());
	for (const auto& loop : take->GetMidiLoops())
		EXPECT_EQ(midi::MidiLoopState::Playing, loop->State());
}

TEST(StationVisualState, MidiOnlyOverdubFinishesImmediatelyWithAnAudioTailConfigured)
{
	auto station = MakeStation("station");
	auto source = station->AddTake();
	source->Record({}, station->Name(), { 0u });
	source->Play(0u, 64u, 0u);
	auto take = station->AddTake();
	take->Overdub({}, station->Name(), { 0u }, {}, source);
	station->CommitChanges();
	ASSERT_EQ(engine::LoopTake::STATE_OVERDUBBING, take->TakeState());
	ASSERT_FALSE(take->GetMidiLoops().empty());

	take->Play(0u, 64u, 128u);

	EXPECT_EQ(engine::LoopTake::STATE_PLAYING, take->TakeState());
	for (const auto& loop : take->GetMidiLoops())
		EXPECT_EQ(midi::MidiLoopState::Playing, loop->State());
}

TEST(StationVisualState, MidiOnlyStopKeepsOtherAudioAndMixedRecordingTailsVisible)
{
	for (const bool mixed : { false, true })
	{
		SCOPED_TRACE(mixed ? "mixed tail" : "audio tail");
		auto station = MakeStation("station");
		station->SetAllowedMidiChannels({ 1 });
		io::UserConfig config{};
		auto audioStart = MakeTriggerAction(TriggerAction::TRIGGER_REC_START);
		audioStart.InputChannels = { 0u };
		if (mixed)
			audioStart.MidiInputDevices = { "Keys" };
		const auto audioStarted = station->OnAction(audioStart);
		station->CommitChanges();
		ASSERT_EQ(1u, station->GetLoopTakes().size());
		const auto audioTake = station->GetLoopTakes()[0];

		auto audioEnd = MakeTriggerAction(TriggerAction::TRIGGER_REC_END, 64u);
		audioEnd.TargetId = audioStarted.TargetId;
		audioEnd.SetUserConfig(config);
		station->OnAction(audioEnd);
		ASSERT_EQ(engine::LoopTake::STATE_PLAYINGRECORDING, audioTake->TakeState());

		auto midiStart = MakeTriggerAction(TriggerAction::TRIGGER_REC_START);
		midiStart.MidiInputDevices = { "Keys" };
		const auto midiStarted = station->OnAction(midiStart);
		station->CommitChanges();
		ASSERT_EQ(2u, station->GetLoopTakes().size());
		const auto midiTake = station->GetLoopTakes()[1];
		ASSERT_TRUE(midiTake->GetLoops().empty());
		ASSERT_FALSE(midiTake->GetMidiLoops().empty());

		auto midiEnd = MakeTriggerAction(TriggerAction::TRIGGER_REC_END, 64u);
		midiEnd.TargetId = midiStarted.TargetId;
		midiEnd.SetUserConfig(config);
		station->OnAction(midiEnd);
		EXPECT_EQ(engine::LoopTake::STATE_PLAYING, midiTake->TakeState());
		EXPECT_EQ(engine::LoopTake::STATE_PLAYINGRECORDING, audioTake->TakeState());
		EXPECT_EQ(StationVisualState::STATIONSTATE_ENDRECORDING, station->GetVisualState());

		station->OnTick(utils::Timer::GetTime(), 0u, std::nullopt, std::nullopt);
		EXPECT_EQ(StationVisualState::STATIONSTATE_ENDRECORDING, station->GetVisualState());
		audioTake->EndRecording();
		station->OnTick(utils::Timer::GetTime(), 0u, std::nullopt, std::nullopt);
		EXPECT_EQ(StationVisualState::STATIONSTATE_DEFAULT, station->GetVisualState());
	}
}

TEST(StationVisualState, AbsentTargetEndDoesNotInventARecordingTail)
{
	auto station = MakeStation("station");
	station->OnAction(MakeTriggerAction(TriggerAction::TRIGGER_REC_END, 64u));
	EXPECT_EQ(StationVisualState::STATIONSTATE_DEFAULT, station->GetVisualState());

	station->OnTick(utils::Timer::GetTime(), 0u, std::nullopt, std::nullopt);
	EXPECT_EQ(StationVisualState::STATIONSTATE_DEFAULT, station->GetVisualState());
}

TEST(StationVisualState, FirstRecordingSeedsAnUninitialisedClock)
{
	auto station = MakeStation("station");
	auto clock = std::make_shared<utils::Timer>();
	station->SetClock(clock);
	ASSERT_EQ(0ul, clock->SeedSourceLength());
	ASSERT_FALSE(clock->IsQuantisable());

	io::UserConfig config{};
	audio::AudioStreamParams stream{};
	stream.SampleRate = 48000u;
	constexpr unsigned long recordedLength = 96000ul;
	auto expected = config.DeduceLoopTiming(recordedLength, stream.SampleRate);
	ASSERT_TRUE(expected.has_value());

	TriggerAction start = MakeTriggerAction(TriggerAction::TRIGGER_REC_START);
	start.SetUserConfig(config);
	start.SetAudioParams(stream);
	const auto startResult = station->OnAction(start);
	ASSERT_TRUE(startResult.IsEaten);

	TriggerAction end = MakeTriggerAction(TriggerAction::TRIGGER_REC_END, recordedLength);
	end.TargetId = startResult.TargetId;
	end.SetUserConfig(config);
	end.SetAudioParams(stream);
	const auto endResult = station->OnAction(end);
	ASSERT_TRUE(endResult.IsEaten);
	EXPECT_EQ(expected->GrainSamps, clock->QuantiseSamps());
	EXPECT_EQ(static_cast<unsigned long>(expected->GrainSamps) * expected->LoopGrains,
		clock->SeedSourceLength());
}

TEST(StationVisualState, OverdubAndPunchInTrackTheirOwnTransitions)
{
	auto station = MakeStation("station");

	const auto started = station->OnAction(MakeTriggerAction(TriggerAction::TRIGGER_OVERDUB_START));
	EXPECT_EQ(StationVisualState::STATIONSTATE_OVERDUBBING, station->GetVisualState());

	auto punch = MakeTriggerAction(TriggerAction::TRIGGER_PUNCHIN_START);
	punch.TargetId = started.TargetId;
	station->OnAction(punch);
	EXPECT_EQ(StationVisualState::STATIONSTATE_PUNCHIN, station->GetVisualState());

	punch.ActionType = TriggerAction::TRIGGER_PUNCHIN_END;
	station->OnAction(punch);
	EXPECT_EQ(StationVisualState::STATIONSTATE_OVERDUBBING, station->GetVisualState());
}

TEST(StationVisualState, TriggerUpdatesOnlyItsReceivingStation)
{
	auto receivingStation = MakeStation("receiving");
	auto unrelatedStation = MakeStation("unrelated");
	TriggerParams triggerParams;
	triggerParams.Activate.emplace_back(engine::TriggerBinding(engine::TRIGGER_KEY, 'R', 1u),
		engine::TriggerBinding(engine::TRIGGER_KEY, 'R', 0u));
	auto trigger = std::make_shared<Trigger>(triggerParams);
	trigger->SetReceiver(receivingStation);

	base::Action action;
	EXPECT_TRUE(trigger->QueueExternalControlAction(true, true, action).IsEaten);
	trigger->OnTick(utils::Timer::GetTime(), 0u, std::nullopt, std::nullopt);
	trigger->ProcessStructuralActionsOnJob(std::nullopt, std::nullopt);
	trigger->OnTick(utils::Timer::GetTime(), 0u, std::nullopt, std::nullopt);

	EXPECT_EQ(engine::TRIGSTATE_RECORDING, trigger->GetState());
	EXPECT_EQ(StationVisualState::STATIONSTATE_RECORDING, receivingStation->GetVisualState());
	EXPECT_EQ(StationVisualState::STATIONSTATE_DEFAULT, unrelatedStation->GetVisualState());
}

TEST(StationVisualState, RetiresRemovedTakeAfterReplacementAudioBoundary)
{
	auto station = MakeStation("station");
	auto take = station->AddTake();
	const auto takeId = take->Id();
	std::weak_ptr<engine::LoopTake> retiredTake = take;
	station->CommitChanges();
	station->AcknowledgeAudioBoundary();
	station->CommitChanges();

	auto ditch = MakeTriggerAction(TriggerAction::TRIGGER_DITCH);
	ditch.TargetId = takeId;
	station->OnAction(ditch);
	take.reset();
	station->CommitChanges();
	EXPECT_FALSE(retiredTake.expired());
	EXPECT_EQ(1u, station->RetiredAudioStateCount());

	station->AcknowledgeAudioBoundary();
	station->ReleaseRetiredAudioStates();
	EXPECT_EQ(0u, station->RetiredAudioStateCount());
}

TEST(StationVisualState, LatestActiveModeWinsAndEndingNewestRevealsEarlierCapture)
{
	for (const bool recordFirst : { false, true })
	{
		SCOPED_TRACE(recordFirst);
		auto station = MakeStation("station");
		auto record = MakeTriggerAction(TriggerAction::TRIGGER_REC_START);
		record.InputChannels = { 0u };
		auto overdub = MakeTriggerAction(TriggerAction::TRIGGER_OVERDUB_START);
		overdub.InputChannels = { 0u };
		const auto older = station->OnAction(recordFirst ? record : overdub);
		station->CommitChanges();
		const auto newer = station->OnAction(recordFirst ? overdub : record);
		// New membership and accepted mode are visible before the UI commits.
		EXPECT_EQ(recordFirst ? StationVisualState::STATIONSTATE_OVERDUBBING
			: StationVisualState::STATIONSTATE_RECORDING, station->GetVisualState());
		station->CommitChanges(); // Materialize audio loops before exercising their tail.
		auto end = MakeTriggerAction(recordFirst ? TriggerAction::TRIGGER_OVERDUB_END
			: TriggerAction::TRIGGER_REC_END, 64u);
		end.TargetId = newer.TargetId;
		end.SetUserConfig(io::UserConfig{});
		station->OnAction(end);
		EXPECT_EQ(recordFirst ? StationVisualState::STATIONSTATE_RECORDING
			: StationVisualState::STATIONSTATE_OVERDUBBING, station->GetVisualState());
		auto ditch = MakeTriggerAction(TriggerAction::TRIGGER_DITCH);
		ditch.TargetId = older.TargetId;
		station->OnAction(ditch);
		EXPECT_EQ(StationVisualState::STATIONSTATE_ENDRECORDING, station->GetVisualState());
	}
}

TEST(StationVisualState, RepeatedAndAbsentPunchDoNotStealRecency)
{
	auto station = MakeStation("station");
	const auto overdub = station->OnAction(MakeTriggerAction(TriggerAction::TRIGGER_OVERDUB_START));
	auto punch = MakeTriggerAction(TriggerAction::TRIGGER_PUNCHIN_START);
	punch.TargetId = overdub.TargetId;
	punch.ApplyToTargetTake = false; // Audio punch is delayed, but logical mode is immediate.
	station->OnAction(punch);
	EXPECT_EQ(StationVisualState::STATIONSTATE_PUNCHIN, station->GetVisualState());
	const auto record = station->OnAction(MakeTriggerAction(TriggerAction::TRIGGER_REC_START));
	station->OnAction(punch);
	EXPECT_EQ(StationVisualState::STATIONSTATE_RECORDING, station->GetVisualState());
	punch.TargetId = "absent";
	station->OnAction(punch);
	EXPECT_EQ(StationVisualState::STATIONSTATE_RECORDING, station->GetVisualState());
	punch.TargetId = overdub.TargetId;
	punch.ActionType = TriggerAction::TRIGGER_PUNCHIN_END;
	station->OnAction(punch);
	EXPECT_EQ(StationVisualState::STATIONSTATE_OVERDUBBING, station->GetVisualState());
	auto ditch = MakeTriggerAction(TriggerAction::TRIGGER_DITCH);
	ditch.TargetId = overdub.TargetId;
	station->OnAction(ditch);
	EXPECT_EQ(StationVisualState::STATIONSTATE_RECORDING, station->GetVisualState());
	ditch.TargetId = record.TargetId;
	station->OnAction(ditch);
	EXPECT_EQ(StationVisualState::STATIONSTATE_DEFAULT, station->GetVisualState());
}

TEST(StationVisualState, EqualActiveModesAndDitchOlderKeepNewerVisible)
{
	auto station = MakeStation("station");
	const auto older = station->OnAction(MakeTriggerAction(TriggerAction::TRIGGER_REC_START));
	const auto newer = station->OnAction(MakeTriggerAction(TriggerAction::TRIGGER_REC_START));
	auto ditch = MakeTriggerAction(TriggerAction::TRIGGER_DITCH);
	ditch.TargetId = older.TargetId;
	station->OnAction(ditch);
	EXPECT_EQ(StationVisualState::STATIONSTATE_RECORDING, station->GetVisualState());
	ditch.TargetId = "absent";
	station->OnAction(ditch);
	EXPECT_EQ(StationVisualState::STATIONSTATE_RECORDING, station->GetVisualState());
	ditch.TargetId = newer.TargetId;
	station->OnAction(ditch);
	EXPECT_EQ(StationVisualState::STATIONSTATE_DEFAULT, station->GetVisualState());
}

TEST(StationVisualState, MidiOnlyPunchUsesLogicalModeAndCompletedContentFallsBackToDefault)
{
	auto station = MakeStation("station");
	station->SetAllowedMidiChannels({ 1 });
	auto start = MakeTriggerAction(TriggerAction::TRIGGER_OVERDUB_START);
	start.MidiInputDevices = { "Keys" };
	const auto started = station->OnAction(start);
	auto punch = MakeTriggerAction(TriggerAction::TRIGGER_PUNCHIN_START);
	punch.TargetId = started.TargetId;
	punch.ApplyToTargetAudio = false;
	station->OnAction(punch);
	EXPECT_EQ(StationVisualState::STATIONSTATE_PUNCHIN, station->GetVisualState());
	auto end = MakeTriggerAction(TriggerAction::TRIGGER_OVERDUB_END, 64u);
	end.TargetId = started.TargetId;
	end.SetUserConfig(io::UserConfig{});
	station->OnAction(end);
	EXPECT_EQ(StationVisualState::STATIONSTATE_DEFAULT, station->GetVisualState());
	EXPECT_EQ(1u, station->GetLoopTakeSnapshot().size());
	station->Reset();
	EXPECT_EQ(StationVisualState::STATIONSTATE_DEFAULT, station->GetVisualState());
}

TEST(StationVisualState, PresentationPublicationKeepsModeAndSerialCoherent)
{
	auto station = MakeStation("station");
	const auto take = station->AddTake();
	std::atomic<bool> begin{ false };
	std::atomic<bool> done{ false };
	std::atomic<bool> mismatched{ false };
	std::thread reader([&] {
		begin.store(true, std::memory_order_release);
		while (!done.load(std::memory_order_acquire))
		{
			const auto observed = take->GetPresentation();
			if (observed.Serial != 0u && observed.Mode != ((observed.Serial & 1u)
				? engine::LoopTake::PresentationMode::Record : engine::LoopTake::PresentationMode::Punch))
				mismatched.store(true, std::memory_order_relaxed);
			const auto display = station->GetVisualState();
			if (display != StationVisualState::STATIONSTATE_DEFAULT
				&& display != StationVisualState::STATIONSTATE_RECORDING
				&& display != StationVisualState::STATIONSTATE_PUNCHIN)
				mismatched.store(true, std::memory_order_relaxed);
		}
	});
	while (!begin.load(std::memory_order_acquire)) std::this_thread::yield();
	for (std::uint64_t serial = 1u; serial <= 10000u; ++serial)
		take->SetPresentation((serial & 1u) ? engine::LoopTake::PresentationMode::Record
			: engine::LoopTake::PresentationMode::Punch, serial);
	done.store(true, std::memory_order_release);
	reader.join();
	EXPECT_FALSE(mismatched.load());
}

TEST(StationVisualState, ImmutableMembershipRemainsSafeAcrossPendingAdditionAndRemoval)
{
	auto station = MakeStation("station");
	std::atomic<unsigned int> phase{ 0u };
	std::atomic<unsigned int> observed{ 0u };
	std::atomic<bool> wrong{ false };
	std::thread reader([&] {
		for (unsigned int next = 1u; next <= 3u; ++next)
		{
			while (phase.load(std::memory_order_acquire) < next) std::this_thread::yield();
			const auto display = station->GetVisualState();
			const auto expected = next == 2u ? StationVisualState::STATIONSTATE_RECORDING
				: StationVisualState::STATIONSTATE_DEFAULT;
			if (display != expected) wrong.store(true, std::memory_order_relaxed);
			observed.store(next, std::memory_order_release);
		}
	});
	phase.store(1u, std::memory_order_release);
	while (observed.load(std::memory_order_acquire) < 1u) std::this_thread::yield();
	const auto started = station->OnAction(MakeTriggerAction(TriggerAction::TRIGGER_REC_START));
	const auto oldMembership = station->GetLoopTakeSnapshot();
	phase.store(2u, std::memory_order_release);
	while (observed.load(std::memory_order_acquire) < 2u) std::this_thread::yield();
	auto ditch = MakeTriggerAction(TriggerAction::TRIGGER_DITCH);
	ditch.TargetId = started.TargetId;
	station->OnAction(ditch);
	phase.store(3u, std::memory_order_release);
	reader.join();
	EXPECT_FALSE(wrong.load());
	EXPECT_TRUE(station->GetLoopTakeSnapshot().empty());
	ASSERT_EQ(1u, oldMembership.size());
	EXPECT_EQ(started.TargetId, oldMembership.front()->Id());
	EXPECT_EQ(engine::LoopTake::PresentationMode::Inactive, oldMembership.front()->GetPresentation().Mode);
}
