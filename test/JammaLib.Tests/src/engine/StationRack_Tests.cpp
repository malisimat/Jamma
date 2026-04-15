#include "gtest/gtest.h"
#include "actions/GuiAction.h"
#include "engine/LoopTake.h"
#include "engine/Station.h"

using actions::GuiAction;
using engine::LoopTake;
using engine::LoopTakeParams;
using engine::Station;
using engine::StationParams;
using audio::MergeMixBehaviourParams;

namespace
{
	std::shared_ptr<Station> MakeStation(const std::string& name = "test-station")
	{
		StationParams params;
		params.Name = name;
		params.Size = { 200, 200 };
		MergeMixBehaviourParams merge;
		auto mixerParams = Station::GetMixerParams(params.Size, merge);
		return std::make_shared<Station>(params, mixerParams);
	}

	void CommitInitial(const std::shared_ptr<Station>& station)
	{
		station->CommitChanges();
	}

	class TestLoopTake :
		public LoopTake
	{
	public:
		TestLoopTake(LoopTakeParams params, audio::AudioMixerParams mixerParams) :
			LoopTake(params, mixerParams)
		{
		}

		void ForceRackState(gui::GuiRackParams::RackState state)
		{
			_guiRack->SetRackState(state, true);
		}
	};

	std::shared_ptr<TestLoopTake> MakeTestLoopTake(const std::string& id)
	{
		LoopTakeParams params;
		params.Id = id;
		params.Size = { 100, 100 };
		MergeMixBehaviourParams merge;
		auto mixerParams = LoopTake::GetMixerParams(params.Size, merge);
		return std::make_shared<TestLoopTake>(params, mixerParams);
	}

	void OpenRouterOnTake(const std::shared_ptr<Station>& station,
		const std::shared_ptr<TestLoopTake>& take)
	{
		GuiAction action;
		action.ElementType = GuiAction::ACTIONELEMENT_RACK;
		action.Index = gui::GuiRack::RackStateNotificationIndex;
		action.Data = GuiAction::GuiInt{ (int)gui::GuiRackParams::RACK_ROUTER };

		station->OnAction(action);
		take->ForceRackState(gui::GuiRackParams::RACK_ROUTER);
	}
}

TEST(StationRack, RouterOpenCollapsesCommittedSiblingRacksToMaster)
{
	auto station = MakeStation();
	CommitInitial(station);

	auto take0 = MakeTestLoopTake("take-0");
	auto take1 = MakeTestLoopTake("take-1");
	station->AddTake(take0);
	station->AddTake(take1);
	station->CommitChanges();

	take0->ForceRackState(gui::GuiRackParams::RACK_CHANNELS);
	take1->ForceRackState(gui::GuiRackParams::RACK_CHANNELS);
	OpenRouterOnTake(station, take1);

	EXPECT_EQ(gui::GuiRackParams::RACK_MASTER, take0->GetRackState());
	EXPECT_EQ(gui::GuiRackParams::RACK_ROUTER, take1->GetRackState());
}

TEST(StationRack, RouterOpenCollapsesStagedSiblingRacksToMaster)
{
	auto station = MakeStation();
	CommitInitial(station);

	auto take0 = MakeTestLoopTake("take-0");
	auto take1 = MakeTestLoopTake("take-1");
	station->AddTake(take0);
	station->AddTake(take1);

	take0->ForceRackState(gui::GuiRackParams::RACK_CHANNELS);
	take1->ForceRackState(gui::GuiRackParams::RACK_CHANNELS);
	OpenRouterOnTake(station, take1);

	EXPECT_EQ(gui::GuiRackParams::RACK_MASTER, take0->GetRackState());
	EXPECT_EQ(gui::GuiRackParams::RACK_ROUTER, take1->GetRackState());
}