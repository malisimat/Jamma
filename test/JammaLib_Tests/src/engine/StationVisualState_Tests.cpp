#include "gtest/gtest.h"

#include "actions/TriggerAction.h"
#include "engine/Station.h"
#include "engine/Trigger.h"

using actions::TriggerAction;
using engine::Station;
using engine::StationParams;
using engine::StationVisualState;
using engine::Trigger;
using engine::TriggerParams;

namespace
{
	std::shared_ptr<Station> MakeStation(const std::string& name)
	{
		StationParams params;
		params.Name = name;
		params.Size = { 100u, 100u };
		audio::MergeMixBehaviourParams merge;
		return std::make_shared<Station>(params, Station::GetMixerParams(params.Size, merge));
	}

	TriggerAction MakeTriggerAction(TriggerAction::TriggerActionType type,
		unsigned long sampleCount = 0u)
	{
		TriggerAction action;
		action.ActionType = type;
		action.SampleCount = sampleCount;
		return action;
	}
}

TEST(StationVisualState, StartsDefault)
{
	auto station = MakeStation("station");

	EXPECT_EQ(StationVisualState::STATIONSTATE_DEFAULT, station->GetVisualState());
}

TEST(StationVisualState, RecordingEndRemainsVisibleUntilTheTakeFinishes)
{
	auto station = MakeStation("station");

	station->OnAction(MakeTriggerAction(TriggerAction::TRIGGER_REC_START));
	EXPECT_EQ(StationVisualState::STATIONSTATE_RECORDING, station->GetVisualState());

	station->OnAction(MakeTriggerAction(TriggerAction::TRIGGER_REC_END, 64u));
	EXPECT_EQ(StationVisualState::STATIONSTATE_ENDRECORDING, station->GetVisualState());
}

TEST(StationVisualState, RecordingEndClearsOnTickWhenNoTakeIsInRecordingTail)
{
	auto station = MakeStation("station");
	station->OnAction(MakeTriggerAction(TriggerAction::TRIGGER_REC_END, 64u));
	EXPECT_EQ(StationVisualState::STATIONSTATE_ENDRECORDING, station->GetVisualState());

	station->OnTick(utils::Timer::GetTime(), 0u, std::nullopt, std::nullopt);
	EXPECT_EQ(StationVisualState::STATIONSTATE_PLAYING, station->GetVisualState());
}

TEST(StationVisualState, OverdubAndPunchInTrackTheirOwnTransitions)
{
	auto station = MakeStation("station");

	station->OnAction(MakeTriggerAction(TriggerAction::TRIGGER_OVERDUB_START));
	EXPECT_EQ(StationVisualState::STATIONSTATE_OVERDUBBING, station->GetVisualState());

	station->OnAction(MakeTriggerAction(TriggerAction::TRIGGER_PUNCHIN_START));
	EXPECT_EQ(StationVisualState::STATIONSTATE_PUNCHIN, station->GetVisualState());

	station->OnAction(MakeTriggerAction(TriggerAction::TRIGGER_PUNCHIN_END));
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
	receivingStation->AddTrigger(trigger);

	base::Action action;
	EXPECT_TRUE(trigger->QueueExternalControlAction(true, true, action).IsEaten);
	trigger->OnTick(utils::Timer::GetTime(), 0u, std::nullopt, std::nullopt);

	EXPECT_EQ(engine::TRIGSTATE_RECORDING, trigger->GetState());
	EXPECT_EQ(StationVisualState::STATIONSTATE_RECORDING, receivingStation->GetVisualState());
	EXPECT_EQ(StationVisualState::STATIONSTATE_DEFAULT, unrelatedStation->GetVisualState());
}