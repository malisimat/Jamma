#include "gtest/gtest.h"
#include "engine/PerformanceGroupService.h"
#include "engine/Station.h"
#include "engine/PerformanceGroupController.h"

class PerformanceGroupServiceTest : public testing::Test
{
protected:
    static std::shared_ptr<engine::Station> Station(const std::string& id)
    {
        engine::StationParams params;
        params.Id = id;
        params.Size = { 160u, 320u };
        return std::make_shared<engine::Station>(params,
            engine::Station::GetMixerParams(params.Size, audio::MergeMixBehaviourParams{}));
    }
};

TEST_F(PerformanceGroupServiceTest, MuteRecallRetainsNegativeStatesAndSkipsNewTargets)
{
    auto station = Station("station-a");
    auto muted = station->AddTake();
    auto audible = station->AddTake();
    muted->SetGroupMutedAudio(true);
    engine::PerformanceGroupService::Stations stations{ station };
    const auto snapshot = engine::PerformanceGroupService::CaptureMuteConfiguration(stations);
    muted->SetGroupMutedAudio(false);
    audible->SetGroupMutedAudio(true);
    auto newTake = station->AddTake();
    newTake->SetGroupMutedAudio(true);
    auto application = engine::PerformanceGroupService::PrepareMuteApplication(snapshot, stations, 1u);
    application->ApplyAudio();
    EXPECT_TRUE(muted->IsMuted());
    EXPECT_FALSE(audible->IsMuted());
    EXPECT_TRUE(newTake->IsMuted());
    EXPECT_TRUE(engine::PerformanceGroupService::PrepareMuteApplication(snapshot, {}, 2u)->Takes.empty());
}

TEST_F(PerformanceGroupServiceTest, SourceSuppressionBecomesPersistentWithoutReleasingSourceControls)
{
    auto station = Station("station-a");
    auto take = station->AddTake();
    take->AcquireTriggerSourceMuteAudio();
    engine::PerformanceGroupService::Stations stations{ station };
    const auto snapshot = engine::PerformanceGroupService::CaptureMuteConfiguration(stations);
    auto application = engine::PerformanceGroupService::PrepareMuteApplication(snapshot, stations, 1u);
    application->ApplyAudio();
    take->ReleaseTriggerSourceMuteAudio();
    EXPECT_TRUE(take->IsMuted());
    EXPECT_TRUE(take->GetTweakState() & base::Tweakable::TWEAKSTATE_MUTED);
}

TEST_F(PerformanceGroupServiceTest, SelectionRestoresParentsWithoutChangingNewTakes)
{
    auto station = Station("station-a");
    auto take = station->AddTake();
    station->Select();
    take->SetGroupSelected(false);
    engine::PerformanceGroupService::Stations stations{ station };
    const auto snapshot = engine::PerformanceGroupService::CaptureSelectionConfiguration(stations, engine::GroupTargetKind::Station);
    station->DeSelect();
    take->SetGroupSelected(true);
    auto newTake = station->AddTake();
    newTake->SetGroupSelected(true);
    engine::PerformanceGroupService::RestoreSelection(snapshot, stations);
    EXPECT_TRUE(station->IsSelected());
    EXPECT_FALSE(take->IsSelected());
    EXPECT_TRUE(newTake->IsSelected());
    EXPECT_EQ(snapshot.Depth, engine::GroupTargetKind::Station);
}

TEST_F(PerformanceGroupServiceTest, BackspaceDeduplicatesStationOverlapAndInvertsMixedStatesIndependently)
{
    auto station = Station("station-a");
    auto muted = station->AddTake();
    auto audible = station->AddTake();
    muted->SetGroupMutedAudio(true);
    station->Select();
    muted->SetGroupSelected(true);
    auto application = engine::PerformanceGroupService::PrepareSelectedTakeInversion({ station }, 1u);
    ASSERT_EQ(application->Takes.size(), 2u);
    application->ApplyAudio();
    EXPECT_FALSE(muted->IsMuted());
    EXPECT_TRUE(audible->IsMuted());
    station->DeSelect();
    muted->SetGroupSelected(false);
    EXPECT_TRUE(engine::PerformanceGroupService::PrepareSelectedTakeInversion({ station }, 2u)->Takes.empty());
}

TEST_F(PerformanceGroupServiceTest, RapidBackspaceInvertsLatestPendingStateAndSuppressesRepeat)
{
    auto station = Station("station-a");
    auto take = station->AddTake();
    take->SetGroupSelected(true);
    engine::PerformanceGroupController controller;
    std::shared_ptr<const engine::GroupMuteApplication> pending;
    auto publish = [&pending](auto application) { pending = std::move(application); };
    actions::KeyAction action;
    action.KeyChar = 8u;
    action.Modifiers = base::Action::MODIFIER_NONE;
    action.KeyActionType = actions::KeyAction::KEY_DOWN;
    ASSERT_TRUE(controller.HandleKey(action, { station }, engine::GroupTargetKind::Take, publish));
    ASSERT_TRUE(pending);
    EXPECT_TRUE(pending->Takes[0].Muted);
    EXPECT_TRUE(controller.HandleKey(action, { station }, engine::GroupTargetKind::Take, publish));
    EXPECT_EQ(pending->Version, 1u);
    action.KeyActionType = actions::KeyAction::KEY_UP;
    EXPECT_TRUE(controller.ConsumeRelease(action));
    action.KeyActionType = actions::KeyAction::KEY_DOWN;
    EXPECT_TRUE(controller.HandleKey(action, { station }, engine::GroupTargetKind::Take, publish));
    EXPECT_EQ(pending->Version, 2u);
    EXPECT_FALSE(pending->Takes[0].Muted);
}

TEST_F(PerformanceGroupServiceTest, AudioLeafMuteAndSelectionPreserveSiblingChoicesAndNewLeaves)
{
    auto station = Station("station-a");
    auto take = station->AddTake();
    take->AddLoop(0u, "station-a");
    take->AddLoop(1u, "station-a");
    take->CommitChanges();
    ASSERT_EQ(take->GetLoops().size(), 2u);
    const auto first = take->GetLoops()[0];
    const auto second = take->GetLoops()[1];
    // The take aggregate flag must not erase finer audible/selection choices.
    take->SetGroupMutedAudio(true);
    first->SetGroupMutedAudio(false);
    second->SetGroupMutedAudio(true);
    take->SetGroupSelected(true);
    first->DeSelect();
    second->Select();
    engine::PerformanceGroupService::Stations stations{ station };
    const auto mute = engine::PerformanceGroupService::CaptureMuteConfiguration(stations);
    const auto selection = engine::PerformanceGroupService::CaptureSelectionConfiguration(stations, engine::GroupTargetKind::AudioLoop);
    first->SetGroupMutedAudio(true);
    second->SetGroupMutedAudio(false);
    first->Select();
    second->DeSelect();
    take->AddLoop(2u, "station-a");
    take->CommitChanges();
    const auto newLeaf = take->GetLoops()[2];
    newLeaf->SetGroupMutedAudio(true);
    newLeaf->Select();
    engine::PerformanceGroupService::PrepareMuteApplication(mute, stations, 1u)->ApplyAudio();
    engine::PerformanceGroupService::RestoreSelection(selection, stations);
    EXPECT_FALSE(first->IsMuted());
    EXPECT_TRUE(second->IsMuted());
    EXPECT_FALSE(first->IsSelected());
    EXPECT_TRUE(second->IsSelected());
    EXPECT_TRUE(newLeaf->IsMuted());
    EXPECT_TRUE(newLeaf->IsSelected());
    EXPECT_TRUE(take->IsSelected());
}

TEST_F(PerformanceGroupServiceTest, MidiSelectionUsesStreamIdentityAfterReorderAndSkipsReplacement)
{
    auto station = Station("station-a");
    auto take = station->AddTake();
    engine::LoopTake::MidiExportState midi;
    midi.LoopLengthSamps = 128u;
    for (unsigned int i = 0u; i < 2u; ++i)
    {
        engine::LoopTake::MidiStreamExport stream;
        stream.Id = i == 0u ? "midi-a" : "midi-b";
        stream.Channel = i;
        stream.Loop.LoopLengthSamps = 128u;
        stream.Loop.EventCount = 1u;
        stream.Loop.Events[0] = midi::MidiEvent::MakeNoteOn(0u, static_cast<std::uint8_t>(i), 60u, 100u);
        midi.Streams.push_back(stream);
    }
    ASSERT_TRUE(take->RestoreMidiFromExport(midi));
    take->SetGroupSelected(true);
    take->GetMidiLoops()[0]->Model()->Select();
    take->GetMidiLoops()[1]->Model()->DeSelect();
    engine::PerformanceGroupService::Stations stations{ station };
    const auto selection = engine::PerformanceGroupService::CaptureSelectionConfiguration(stations, engine::GroupTargetKind::AudioLoop);
    std::swap(midi.Streams[0], midi.Streams[1]);
    ASSERT_TRUE(take->RestoreMidiFromExport(midi));
    take->GetMidiLoops()[0]->Model()->Select();
    take->GetMidiLoops()[1]->Model()->DeSelect();
    engine::PerformanceGroupService::RestoreSelection(selection, stations);
    EXPECT_FALSE(take->GetMidiLoops()[0]->Model()->IsSelected());
    EXPECT_TRUE(take->GetMidiLoops()[1]->Model()->IsSelected());
    midi.Streams[0].Id = "replacement";
    ASSERT_TRUE(take->RestoreMidiFromExport(midi));
    take->GetMidiLoops()[0]->Model()->Select();
    engine::PerformanceGroupService::RestoreSelection(selection, stations);
    EXPECT_TRUE(take->GetMidiLoops()[0]->Model()->IsSelected());
}

TEST_F(PerformanceGroupServiceTest, StationReorderAndDeletionNeverRedirectRecallByPosition)
{
    auto first = Station("station-a");
    auto second = Station("station-b");
    auto firstTake = first->AddTake();
    auto secondTake = second->AddTake();
    firstTake->SetGroupMutedAudio(true);
    const auto snapshot = engine::PerformanceGroupService::CaptureMuteConfiguration({ first, second });
    firstTake->SetGroupMutedAudio(false);
    secondTake->SetGroupMutedAudio(true);
    engine::PerformanceGroupService::PrepareMuteApplication(snapshot, { second, first }, 1u)->ApplyAudio();
    EXPECT_TRUE(firstTake->IsMuted());
    EXPECT_FALSE(secondTake->IsMuted());
    auto replacement = Station("station-c");
    auto replacementTake = replacement->AddTake();
    replacementTake->SetGroupMutedAudio(true);
    engine::PerformanceGroupService::PrepareMuteApplication(snapshot, { replacement, second }, 2u)->ApplyAudio();
    EXPECT_TRUE(replacementTake->IsMuted());
}

TEST_F(PerformanceGroupServiceTest, DigitCommandsConsumeAltTransitionsAndRejectAmbiguousModifiers)
{
    engine::PerformanceGroupController controller;
    auto publish = [](auto) {};
    actions::KeyAction action;
    action.KeyChar = 55u;
    action.Modifiers = base::Action::MODIFIER_NONE;
    action.KeyActionType = actions::KeyAction::KEY_DOWN;
    EXPECT_TRUE(controller.HandleKey(action, {}, engine::GroupTargetKind::Take, publish));
    EXPECT_NE(controller.Groups().Mute.Get(7u), nullptr);
    action.KeyActionType = actions::KeyAction::KEY_UP;
    EXPECT_TRUE(controller.ConsumeRelease(action));
    action.Modifiers = base::Action::MODIFIER_ALT;
    action.IsSystem = true;
    action.KeyActionType = actions::KeyAction::KEY_DOWN;
    EXPECT_TRUE(controller.HandleKey(action, {}, engine::GroupTargetKind::Take, publish));
    EXPECT_EQ(controller.Groups().Mute.Get(7u), nullptr);
    action.KeyActionType = actions::KeyAction::KEY_UP;
    EXPECT_TRUE(controller.ConsumeRelease(action));
    action.Modifiers = static_cast<base::Action::Modifiers>(base::Action::MODIFIER_ALT | base::Action::MODIFIER_SHIFT);
    action.KeyActionType = actions::KeyAction::KEY_DOWN;
    EXPECT_FALSE(controller.HandleKey(action, {}, engine::GroupTargetKind::Take, publish));
    action.KeyChar = 48u;
    action.Modifiers = base::Action::MODIFIER_NONE;
    EXPECT_FALSE(controller.HandleKey(action, {}, engine::GroupTargetKind::Take, publish));
}

TEST_F(PerformanceGroupServiceTest, ContextualHeldPressCannotActivateOnRepeatAndFocusLossResetsKeys)
{
    engine::PerformanceGroupController controller;
    auto publish = [](auto) {};
    actions::KeyAction action;
    action.KeyChar = 49u;
    action.Modifiers = base::Action::MODIFIER_NONE;
    action.KeyActionType = actions::KeyAction::KEY_DOWN;
    controller.BlockContextualPress(action);
    EXPECT_FALSE(controller.HandleKey(action, {}, engine::GroupTargetKind::Take, publish));
    EXPECT_EQ(controller.Groups().Mute.Get(1u), nullptr);
    controller.ResetKeys();
    EXPECT_TRUE(controller.HandleKey(action, {}, engine::GroupTargetKind::Take, publish));
    EXPECT_NE(controller.Groups().Mute.Get(1u), nullptr);
}

TEST_F(PerformanceGroupServiceTest, PluginFocusLossRetainsGroupReleaseOwnership)
{
    engine::PerformanceGroupController controller;
    auto publish = [](auto) {};
    actions::KeyAction key;
    key.KeyChar = 49u;
    key.KeyActionType = actions::KeyAction::KEY_DOWN;
    ASSERT_TRUE(controller.HandleKey(key, {}, engine::GroupTargetKind::Take, publish));
    controller.ResetKeys(); // Synchronous focus loss during plugin recall.
    key.KeyActionType = actions::KeyAction::KEY_UP;
    EXPECT_TRUE(controller.ConsumeRelease(key));
    EXPECT_FALSE(controller.ConsumeRelease(key));
}

TEST_F(PerformanceGroupServiceTest, FreshPressAfterLostGroupReleaseDoesNotStealContextualRelease)
{
    engine::PerformanceGroupController controller;
    auto publish = [](auto) {};
    actions::KeyAction key;
    key.KeyChar = 49u;
    key.KeyActionType = actions::KeyAction::KEY_DOWN;
    ASSERT_TRUE(controller.HandleKey(key, {}, engine::GroupTargetKind::Take, publish));
    controller.ResetKeys();
    // The old key-up went to the plugin editor. The next physical press is
    // owned by text/editor input and must retain both of its own transitions.
    controller.BlockContextualPress(key);
    key.KeyActionType = actions::KeyAction::KEY_UP;
    EXPECT_FALSE(controller.ConsumeRelease(key));
    key.KeyActionType = actions::KeyAction::KEY_DOWN;
    ASSERT_TRUE(controller.HandleKey(key, {}, engine::GroupTargetKind::Take, publish));
    controller.ResetKeys();
    key.Modifiers = base::Action::MODIFIER_CTRL;
    EXPECT_FALSE(controller.HandleKey(key, {}, engine::GroupTargetKind::Take, publish));
    key.KeyActionType = actions::KeyAction::KEY_UP;
    EXPECT_FALSE(controller.ConsumeRelease(key));
}
