#include "gtest/gtest.h"
#include "actions/GuiAction.h"
#include "actions/TouchAction.h"
#include "actions/TouchMoveAction.h"
#include "engine/LoopTake.h"
#include "engine/Station.h"
#include "gui/GuiVstRack.h"

using actions::GuiAction;
using engine::LoopTake;
using engine::LoopTakeParams;
using engine::Station;
using engine::StationParams;
using audio::MergeMixBehaviourParams;

static std::shared_ptr<Station> MakeStation(const std::string& name = "test-station")
{
	StationParams params;
	params.Name = name;
	params.Size = { 200, 200 };
	MergeMixBehaviourParams merge;
	auto mixerParams = Station::GetMixerParams(params.Size, merge);
	return std::make_shared<Station>(params, mixerParams);
}

static void CommitInitial(const std::shared_ptr<Station>& station)
{
	station->CommitChanges();
}

class StationRackTestLoopTake :
	public LoopTake
{
public:
	StationRackTestLoopTake(LoopTakeParams params, audio::AudioMixerParams mixerParams) :
		LoopTake(params, mixerParams)
	{
	}

	void ForceRackState(gui::GuiRackParams::RackState state)
	{
		_guiRack->SetRackState(state, true);
	}
	const auto& Rack() const { return _guiRack; }
	const auto& BusMixer(unsigned int index) const { return _audioMixers.at(index); }
};

static std::shared_ptr<StationRackTestLoopTake> MakeTestLoopTake(const std::string& id)
{
	LoopTakeParams params;
	params.Id = id;
	params.Size = { 100, 100 };
	MergeMixBehaviourParams merge;
	auto mixerParams = LoopTake::GetMixerParams(params.Size, merge);
	return std::make_shared<StationRackTestLoopTake>(params, mixerParams);
}

static void OpenRouterOnTake(const std::shared_ptr<Station>& station,
	const std::shared_ptr<StationRackTestLoopTake>& take)
{
	GuiAction action;
	action.ElementType = GuiAction::ACTIONELEMENT_RACK;
	action.Index = gui::GuiRack::RackStateNotificationIndex;
	action.Data = GuiAction::GuiInt{ (int)gui::GuiRackParams::RACK_ROUTER };

	station->OnAction(action);
	take->ForceRackState(gui::GuiRackParams::RACK_ROUTER);
}

static void OpenChannelsOnTake(const std::shared_ptr<Station>& station,
	const std::shared_ptr<StationRackTestLoopTake>& take)
{
	GuiAction action;
	action.ElementType = GuiAction::ACTIONELEMENT_RACK;
	action.Index = gui::GuiRack::RackStateNotificationIndex;
	action.Data = GuiAction::GuiInt{ (int)gui::GuiRackParams::RACK_CHANNELS };

	station->OnAction(action);
	take->ForceRackState(gui::GuiRackParams::RACK_CHANNELS);
}

TEST(StationRack, TakeRacksUseProjectedSpacingAndKeepSingleTakeHeight)
{
	auto station = MakeStation();
	CommitInitial(station);
	auto first = MakeTestLoopTake("first");
	auto second = MakeTestLoopTake("second");
	auto third = MakeTestLoopTake("third");
	for (const auto& take : { first, second, third }) station->AddTake(take);
	station->CommitChanges();
	for (const auto& take : { first, second, third })
		take->Rack()->VstRack()->Update(gui::GuiVstRack::Model{}, {});
	station->SetModelPosition({ 0.0f, 0.0f, 0.0f });
	station->SetModelScale(1.0);
	first->SetModelPosition({ 0.0f, 0.0f, 0.0f });
	second->SetModelPosition({ 0.0f, 0.3f, 0.0f });
	third->SetModelPosition({ 0.0f, 0.6f, 0.0f });
	station->LayoutTakeRacks(glm::mat4(1.0f), { 1000u, 1000u });
	const auto emptyExtension = first->Rack()->VstRack()->BottomExtension();
	EXPECT_EQ(142u - emptyExtension, first->Rack()->GetSize().Height);
	EXPECT_EQ(first->Rack()->GetSize().Height, second->Rack()->GetSize().Height);
	const auto bottom = [](const auto& take) { return take->Position().Y + take->Rack()->Position().Y; };
	EXPECT_GE(bottom(second) - static_cast<int>(second->Rack()->VstRack()->BottomExtension()),
		bottom(first) + static_cast<int>(first->Rack()->GetSize().Height) + 8);
	EXPECT_GE(bottom(third) - static_cast<int>(third->Rack()->VstRack()->BottomExtension()),
		bottom(second) + static_cast<int>(second->Rack()->GetSize().Height) + 8);
	EXPECT_FALSE(station->LayoutTakeRacks(glm::mat4(1.0f), { 1000u, 1000u }));

	const auto firstHeight = first->Rack()->GetSize().Height;
	const auto thirdHeight = third->Rack()->GetSize().Height;
	gui::GuiVstRack::Model model;
	gui::GuiVstRack::Slot slot;
	slot.Value.Identity = "plugin";
	model.Slots.push_back(slot);
	model.TotalSlots = 1u;
	second->Rack()->VstRack()->Update(std::move(model), {});
	station->LayoutTakeRacks(glm::mat4(1.0f), { 1000u, 1000u });
	EXPECT_LT(second->Rack()->GetSize().Height, firstHeight);
	EXPECT_EQ(firstHeight, first->Rack()->GetSize().Height);
	EXPECT_EQ(thirdHeight, third->Rack()->GetSize().Height);
	EXPECT_GE(bottom(second) - static_cast<int>(second->Rack()->VstRack()->BottomExtension()),
		bottom(first) + static_cast<int>(first->Rack()->GetSize().Height) + 8);

	second->SetModelPosition({ 0.0f, 0.5f, 0.0f });
	third->SetModelPosition({ 0.0f, 1.0f, 0.0f });
	station->LayoutTakeRacks(glm::mat4(1.0f), { 1000u, 1000u });
	EXPECT_EQ(180u - first->Rack()->VstRack()->BottomExtension(), first->Rack()->GetSize().Height);
	EXPECT_EQ(36u, second->Rack()->VstRack()->SlotButton(0)->GetSize().Height);

	second->SetModelPosition({ 0.0f, 0.02f, 0.0f });
	third->SetModelPosition({ 0.0f, 0.04f, 0.0f });
	station->LayoutTakeRacks(glm::mat4(1.0f), { 1000u, 1000u });
	EXPECT_LT(second->Rack()->VstRack()->SlotButton(0)->GetSize().Height, 36u);
	EXPECT_GE(bottom(second) - static_cast<int>(second->Rack()->VstRack()->BottomExtension()), bottom(first) + static_cast<int>(first->Rack()->GetSize().Height) + 8);
	EXPECT_GE(bottom(third) - static_cast<int>(third->Rack()->VstRack()->BottomExtension()), bottom(second) + static_cast<int>(second->Rack()->GetSize().Height) + 8);
	EXPECT_FALSE(station->LayoutTakeRacks(glm::mat4(1.0f), { 1000u, 1000u }));

	second->SetVisible(false);
	third->SetVisible(false);
	station->LayoutTakeRacks(glm::mat4(1.0f), { 1000u, 1000u });
	EXPECT_EQ(first->Rack()->ConstrainHeight(first->GetSize().Height), first->Rack()->GetSize().Height);
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

TEST(StationRack, ChannelsOpenCollapsesCommittedRouterSiblingToChannels)
{
	auto station = MakeStation();
	CommitInitial(station);

	auto take0 = MakeTestLoopTake("take-0");
	auto take1 = MakeTestLoopTake("take-1");
	station->AddTake(take0);
	station->AddTake(take1);
	station->CommitChanges();

	take0->ForceRackState(gui::GuiRackParams::RACK_ROUTER);
	take1->ForceRackState(gui::GuiRackParams::RACK_CHANNELS);
	OpenChannelsOnTake(station, take1);

	EXPECT_EQ(gui::GuiRackParams::RACK_CHANNELS, take0->GetRackState());
	EXPECT_EQ(gui::GuiRackParams::RACK_CHANNELS, take1->GetRackState());
}

TEST(StationRack, ChannelsOpenCollapsesStagedRouterSiblingToChannels)
{
	auto station = MakeStation();
	CommitInitial(station);

	auto take0 = MakeTestLoopTake("take-0");
	auto take1 = MakeTestLoopTake("take-1");
	station->AddTake(take0);
	station->AddTake(take1);

	take0->ForceRackState(gui::GuiRackParams::RACK_ROUTER);
	take1->ForceRackState(gui::GuiRackParams::RACK_CHANNELS);
	OpenChannelsOnTake(station, take1);

	EXPECT_EQ(gui::GuiRackParams::RACK_CHANNELS, take0->GetRackState());
	EXPECT_EQ(gui::GuiRackParams::RACK_CHANNELS, take1->GetRackState());
}

TEST(StationRack, TakeChannelMetersFollowTheControlledLoopGain)
{
	auto take = MakeTestLoopTake("meter-take");
	take->Record({0u, 1u}, "meter-station", {}, {}); take->CommitChanges();
	ASSERT_EQ(2u, take->GetLoops().size());
	for (unsigned int index = 0u; index < 2u; ++index)
	{
		const auto loop = take->GetLoops()[index];
		const auto slider = take->Rack()->GetChannelSlider(index);
		ASSERT_TRUE(slider); ASSERT_EQ(loop->Mixer(), slider->Mixer());
		EXPECT_NE(take->BusMixer(index), slider->Mixer());
		const auto amplitude = index == 0u ? 0.8f : 0.125f;
		const auto count = constants::MaxLoopFadeSamps + 32u;
		std::vector<float> samples(count, amplitude);
		base::AudioWriteRequest request;
		request.samples = samples.data(); request.numSamps = count; request.stride = 1u;
		request.fadeCurrent = 0.0f; request.fadeNew = 1.0f; request.source = base::Audible::AUDIOSOURCE_ADC;
		loop->OnBlockWrite(request, 0); loop->EndWrite(count, true);
		loop->Play(constants::MaxLoopFadeSamps, 32u, false);
		slider->SetValue(index == 0u ? 0.25 : 1.0);
		loop->Mixer()->Offset(4096u);
		loop->WriteBlock(nullptr, nullptr, 0, 32u);
		EXPECT_FLOAT_EQ(index == 0u ? 0.2f : 0.125f, slider->Mixer()->VuPeakLevel());
	}
	for (const auto depth : {base::SelectDepth::DEPTH_STATION, base::SelectDepth::DEPTH_LOOPTAKE, base::SelectDepth::DEPTH_LOOP})
	{
		take->SetSelectDepth(depth); take->SetRackVisibility(true);
		take->Rack()->SetRackState(gui::GuiRackParams::RACK_CHANNELS, true);
		EXPECT_FLOAT_EQ(0.2f, take->Rack()->GetChannelSlider(0u)->Mixer()->VuPeakLevel());
		EXPECT_FLOAT_EQ(0.125f, take->Rack()->GetChannelSlider(1u)->Mixer()->VuPeakLevel());
	}
}

class MeterBindingTestStation final : public Station
{
public:
	explicit MeterBindingTestStation(StationParams params) : Station(params, Station::GetMixerParams(params.Size, MergeMixBehaviourParams{})) {}
	const auto& Rack() const { return _guiRack; }
	const auto& MasterMixer() const { return _masterMixer; }
	const auto& ChannelMixer(unsigned int index) const { return _audioMixers.at(index); }
};

TEST(StationRack, StationMeterBindingsRefreshWhenAudioChannelsAreRebuilt)
{
	StationParams params; params.Size = {200u, 200u};
	auto station = std::make_shared<MeterBindingTestStation>(params);
	station->SetNumDacChannels(2u); station->SetNumBusChannels(2u); station->CommitChanges();
	ASSERT_EQ(station->MasterMixer(), station->Rack()->GetMasterSlider()->Mixer());
	const auto slider = station->Rack()->GetChannelSlider(0u);
	ASSERT_TRUE(slider); ASSERT_EQ(station->ChannelMixer(0u), slider->Mixer());
	const auto oldMixer = slider->Mixer(); oldMixer->UpdateVu(1.0f, 1u);
	station->SetNumBusChannels(3u); station->CommitChanges();
	ASSERT_EQ(slider, station->Rack()->GetChannelSlider(0u));
	ASSERT_EQ(station->ChannelMixer(0u), slider->Mixer());
	EXPECT_NE(oldMixer, slider->Mixer()); EXPECT_FLOAT_EQ(0.0f, slider->Mixer()->VuPeakLevel());
}

TEST(StationRack, CapturedDragRetainsPublishedGainAcrossAncestorCancellationAndUndo)
{
	for (const bool disableAncestor : {false, true})
	for (const bool moveWhileUnavailable : {false, true})
	{
		StationParams params; params.Size = {200u, 200u};
		auto station = std::make_shared<MeterBindingTestStation>(params);
		station->Init(); station->SetSelectDepth(base::SelectDepth::DEPTH_STATION);
		const auto slider = station->Rack()->GetMasterSlider();
		slider->SetValue(0.25);
		ASSERT_DOUBLE_EQ(0.25, station->MasterMixer()->UnmutedLevel());
		actions::TouchAction touch;
		touch.Position = {10, 10}; touch.State = actions::TouchAction::TOUCH_DOWN;
		const auto press = slider->OnAction(touch);
		const auto captured = press.ActiveElement.lock(); ASSERT_EQ(slider, captured);
		actions::TouchMoveAction move; move.Position = {10, 20};
		captured->OnAction(move);
		const auto publishedGain = station->MasterMixer()->UnmutedLevel();
		ASSERT_GT(publishedGain, 0.25); ASSERT_DOUBLE_EQ(publishedGain, slider->Value());
		if (disableAncestor) station->SetEnabled(false);
		else station->SetSelectDepth(base::SelectDepth::DEPTH_LOOPTAKE); // Tab-equivalent rack hide.
		if (moveWhileUnavailable)
		{
			move.Position = {10, 40}; EXPECT_TRUE(captured->OnAction(move).IsEaten);
			EXPECT_DOUBLE_EQ(publishedGain, station->MasterMixer()->UnmutedLevel());
		}
		touch.Position = {10, 20}; touch.State = actions::TouchAction::TOUCH_UP;
		const auto release = captured->OnAction(touch);
		EXPECT_TRUE(release.IsEaten); ASSERT_TRUE(release.Undo);
		EXPECT_DOUBLE_EQ(publishedGain, slider->Value());
		EXPECT_DOUBLE_EQ(publishedGain, station->MasterMixer()->UnmutedLevel());
		station->SetEnabled(true); station->SetSelectDepth(base::SelectDepth::DEPTH_STATION);
		EXPECT_DOUBLE_EQ(publishedGain, slider->Value());
		ASSERT_TRUE(release.Undo->Undo());
		EXPECT_DOUBLE_EQ(0.25, slider->Value());
		EXPECT_DOUBLE_EQ(0.25, station->MasterMixer()->UnmutedLevel());
	}
}
