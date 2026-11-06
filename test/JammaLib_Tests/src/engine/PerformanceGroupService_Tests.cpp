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

TEST_F(PerformanceGroupServiceTest, MuteRecallRetainsNegativeStatesAndMutesNewTargets)
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
    newTake->SetGroupMutedAudio(false);
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

TEST_F(PerformanceGroupServiceTest, SwitchingOldAndNewGroupsRestoresOnlyTheirRecordedMaterial)
{
    auto station = Station("station-a");
    auto take = station->AddTake();
    take->AddLoop(0u, station->Id());
    take->AddLoop(1u, station->Id());
    take->CommitChanges();
    const auto a = take->GetLoops()[0];
    const auto b = take->GetLoops()[1];
    const auto original = engine::PerformanceGroupService::CaptureMuteConfiguration({ station });

    take->AddLoop(2u, station->Id());
    take->CommitChanges();
    const auto c = take->GetLoops()[2];
    a->SetGroupMutedAudio(true);
    b->SetGroupMutedAudio(true);
    c->SetGroupMutedAudio(false);
    auto newTake = station->AddTake();
    newTake->AddLoop(0u, station->Id());
    newTake->CommitChanges();
    newTake->SetGroupMutedAudio(false);
    auto newStation = Station("station-b");
    auto newStationTake = newStation->AddTake();
    newStationTake->SetGroupMutedAudio(false);
    engine::PerformanceGroupService::Stations stations{ station, newStation };
    const auto later = engine::PerformanceGroupService::CaptureMuteConfiguration(stations);
    for (std::uint64_t version = 1u; version <= 6u; ++version)
    {
        const bool oldGroup = version % 2u != 0u;
        auto application = engine::PerformanceGroupService::PrepareMuteApplication(
            oldGroup ? original : later, stations, version);
        ASSERT_TRUE(application);
        application->ApplyAudio();
        EXPECT_EQ(a->IsMuted(), !oldGroup);
        EXPECT_EQ(b->IsMuted(), !oldGroup);
        EXPECT_EQ(c->IsMuted(), oldGroup);
        EXPECT_FALSE(take->IsMuted());
        EXPECT_EQ(newTake->IsMuted(), oldGroup);
        EXPECT_EQ(newTake->GetLoops()[0]->IsMuted(), oldGroup);
        EXPECT_EQ(newStationTake->IsMuted(), oldGroup);
    }
}

class RemoteGroupTestStation final : public engine::Station
{
public:
    RemoteGroupTestStation(engine::StationParams params, audio::AudioMixerParams mixer)
        : engine::Station(params, mixer) {}
    bool IsRemote() const noexcept override { return true; }
};

TEST_F(PerformanceGroupServiceTest, EmptyMuteSnapshotMutesAllLocalMaterialAndExcludesRemoteMaterial)
{
    auto local = Station("local");
    auto take = local->AddTake();
    take->AddLoop(0u, local->Id());
    take->CommitChanges();
    const auto loop = take->GetLoops()[0];
    take->SetGroupMutedAudio(false);
    loop->SetGroupMutedAudio(false);
    engine::StationParams params;
    params.Id = "remote";
    params.Size = { 160u, 320u };
    auto remote = std::make_shared<RemoteGroupTestStation>(params,
        engine::Station::GetMixerParams(params.Size, audio::MergeMixBehaviourParams{}));
    auto remoteTake = remote->AddTake();
    remoteTake->SetGroupMutedAudio(false);
    auto application = engine::PerformanceGroupService::PrepareMuteApplication({}, { local, remote }, 1u);
    ASSERT_TRUE(application);
    ASSERT_EQ(application->Takes.size(), 1u);
    ASSERT_EQ(application->Loops.size(), 1u);
    application->ApplyAudio();
    EXPECT_TRUE(take->IsMuted());
    EXPECT_TRUE(loop->IsMuted());
    EXPECT_FALSE(remoteTake->IsMuted());
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
    newLeaf->SetGroupMutedAudio(false);
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
    replacementTake->SetGroupMutedAudio(false);
    engine::PerformanceGroupService::PrepareMuteApplication(snapshot, { replacement, second }, 2u)->ApplyAudio();
    EXPECT_TRUE(replacementTake->IsMuted());
}

TEST_F(PerformanceGroupServiceTest, FunctionKeyCommandsConsumeAltTransitionsAndRejectAmbiguousModifiers)
{
    engine::PerformanceGroupController controller;
    auto publish = [](auto) {};
    actions::KeyAction action;
    action.KeyChar = 112u;
    action.Modifiers = base::Action::MODIFIER_NONE;
    action.KeyActionType = actions::KeyAction::KEY_DOWN;
    EXPECT_TRUE(controller.HandleKey(action, {}, engine::GroupTargetKind::Take, publish));
    EXPECT_NE(controller.Groups().Mute.Get(1u), nullptr);
    action.KeyActionType = actions::KeyAction::KEY_UP;
    EXPECT_TRUE(controller.ConsumeRelease(action));
    action.Modifiers = base::Action::MODIFIER_ALT;
    action.IsSystem = true;
    action.KeyActionType = actions::KeyAction::KEY_DOWN;
    EXPECT_TRUE(controller.HandleKey(action, {}, engine::GroupTargetKind::Take, publish));
    EXPECT_EQ(controller.Groups().Mute.Get(1u), nullptr);
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
    action.KeyChar = 112u;
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
    key.KeyChar = 112u;
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
    key.KeyChar = 112u;
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

TEST_F(PerformanceGroupServiceTest, EmptyRowHiddenFunctionKeysAreConsumedWithoutCreatingOrPublishing)
{
    auto station = Station("station-a");
    auto take = station->AddTake();
    engine::PerformanceGroupController controller;
    unsigned int publications = 0u;
    auto publish = [&publications](auto) { ++publications; };
    for (const auto modifiers : { base::Action::MODIFIER_NONE, base::Action::MODIFIER_SHIFT,
        base::Action::MODIFIER_ALT })
        for (unsigned int keyCode = 113u; keyCode <= 120u; ++keyCode)
        {
            actions::KeyAction key;
            key.KeyChar = keyCode;
            key.Modifiers = modifiers;
            key.KeyActionType = actions::KeyAction::KEY_DOWN;
            EXPECT_TRUE(controller.HandleKey(key, { station }, engine::GroupTargetKind::Take, publish));
            EXPECT_TRUE(controller.HandleKey(key, { station }, engine::GroupTargetKind::Take, publish));
            EXPECT_EQ(controller.Groups().Mute.VisibleSlots(), 0u);
            EXPECT_FALSE(controller.Groups().Mute.ActiveSlot());
            EXPECT_TRUE(controller.Error().empty());
            EXPECT_FALSE(take->IsMuted());
            key.KeyActionType = actions::KeyAction::KEY_UP;
            EXPECT_TRUE(controller.ConsumeRelease(key));
            EXPECT_FALSE(controller.ConsumeRelease(key));
        }
    EXPECT_EQ(publications, 0u);
}

TEST_F(PerformanceGroupServiceTest, FunctionKeysCreateNextPositionRecallOverwriteAndClear)
{
    auto station = Station("station-a");
    auto take = station->AddTake();
    engine::PerformanceGroupController controller;
    unsigned int publications = 0u;
    auto publish = [&publications, &controller](auto application) { ++publications; application->ApplyAudio(); controller.Acknowledge(application->Version); };
    actions::KeyAction key;
    key.KeyChar = 112u;
    key.KeyActionType = actions::KeyAction::KEY_DOWN;
    ASSERT_TRUE(controller.HandleKey(key, { station }, engine::GroupTargetKind::Take, publish));
    ASSERT_NE(controller.Groups().Mute.Get(1u), nullptr);
    key.KeyActionType = actions::KeyAction::KEY_UP;
    ASSERT_TRUE(controller.ConsumeRelease(key));
    key.KeyChar = 113u;
    key.KeyActionType = actions::KeyAction::KEY_DOWN;
    ASSERT_TRUE(controller.HandleKey(key, { station }, engine::GroupTargetKind::Take, publish));
    ASSERT_NE(controller.Groups().Mute.Get(2u), nullptr);
    key.KeyActionType = actions::KeyAction::KEY_UP;
    ASSERT_TRUE(controller.ConsumeRelease(key));
    take->SetGroupMutedAudio(true);
    key.KeyActionType = actions::KeyAction::KEY_DOWN;
    ASSERT_TRUE(controller.HandleKey(key, { station }, engine::GroupTargetKind::Take, publish));
    EXPECT_FALSE(take->IsMuted());
    EXPECT_EQ(publications, 1u);
    key.KeyActionType = actions::KeyAction::KEY_UP;
    ASSERT_TRUE(controller.ConsumeRelease(key));
    take->SetGroupMutedAudio(true);
    key.Modifiers = base::Action::MODIFIER_SHIFT;
    key.KeyActionType = actions::KeyAction::KEY_DOWN;
    ASSERT_TRUE(controller.HandleKey(key, { station }, engine::GroupTargetKind::Take, publish));
    const auto& overwritten = controller.Groups().Mute.Get(2u)->Targets;
    ASSERT_EQ(overwritten.size(), 2u);
    EXPECT_EQ(overwritten[1].Target.Kind, engine::GroupTargetKind::Take);
    EXPECT_TRUE(overwritten[1].Muted);
    EXPECT_EQ(publications, 1u);
    key.KeyActionType = actions::KeyAction::KEY_UP;
    ASSERT_TRUE(controller.ConsumeRelease(key));
    key.Modifiers = base::Action::MODIFIER_ALT;
    key.IsSystem = true;
    key.KeyActionType = actions::KeyAction::KEY_DOWN;
    ASSERT_TRUE(controller.HandleKey(key, { station }, engine::GroupTargetKind::Take, publish));
    EXPECT_EQ(controller.Groups().Mute.Get(2u), nullptr);
    EXPECT_TRUE(take->IsMuted());
    EXPECT_EQ(publications, 1u);
}

TEST_F(PerformanceGroupServiceTest, SparseFunctionKeyRecallAndInteriorCreationPreserveLowestHoleAdd)
{
    engine::PerformanceGroupController controller;
    controller.Groups().Mute.Store(9u, {});
    unsigned int publications = 0u;
    auto publish = [&publications](auto) { ++publications; };
    actions::KeyAction key;
    key.KeyChar = 120u;
    key.KeyActionType = actions::KeyAction::KEY_DOWN;
    ASSERT_TRUE(controller.HandleKey(key, {}, engine::GroupTargetKind::Take, publish));
    EXPECT_EQ(publications, 1u);
    EXPECT_EQ(controller.Groups().Mute.ActiveSlot(), 9u);
    key.KeyChar = 115u;
    ASSERT_TRUE(controller.HandleKey(key, {}, engine::GroupTargetKind::Take, publish));
    EXPECT_NE(controller.Groups().Mute.Get(4u), nullptr);
    EXPECT_EQ(controller.Groups().Mute.Get(1u), nullptr);
    EXPECT_EQ(controller.Groups().Mute.Get(2u), nullptr);
    ASSERT_TRUE(controller.Execute(false, engine::GroupCommand::Add, 0u, {}, engine::GroupTargetKind::Take, publish));
    EXPECT_NE(controller.Groups().Mute.Get(1u), nullptr);
    EXPECT_EQ(controller.Groups().Mute.Get(2u), nullptr);
}

TEST_F(PerformanceGroupServiceTest, HiddenFunctionKeyAndMissingAltPreservePendingRecallAndFeedback)
{
    auto station = Station("station-a");
    auto take = station->AddTake();
    take->SetGroupSelected(true);
    engine::PerformanceGroupController controller;
    std::shared_ptr<const engine::GroupMuteApplication> pending;
    auto publish = [&pending](auto application) { pending = std::move(application); };
    actions::KeyAction key;
    key.KeyChar = 8u;
    key.KeyActionType = actions::KeyAction::KEY_DOWN;
    ASSERT_TRUE(controller.HandleKey(key, { station }, engine::GroupTargetKind::Take, publish));
    const auto original = pending;
    key.KeyChar = 113u;
    ASSERT_TRUE(controller.HandleKey(key, { station }, engine::GroupTargetKind::Take, publish));
    EXPECT_EQ(pending, original);
    key.KeyChar = 112u;
    key.Modifiers = base::Action::MODIFIER_ALT;
    ASSERT_TRUE(controller.HandleKey(key, { station }, engine::GroupTargetKind::Take, publish));
    EXPECT_EQ(pending, original);
    key.KeyChar = 8u;
    key.KeyActionType = actions::KeyAction::KEY_UP;
    ASSERT_TRUE(controller.ConsumeRelease(key));
    key.Modifiers = base::Action::MODIFIER_NONE;
    key.KeyActionType = actions::KeyAction::KEY_DOWN;
    ASSERT_TRUE(controller.HandleKey(key, { station }, engine::GroupTargetKind::Take, publish));
    EXPECT_FALSE(pending->Takes[0].Muted);
    EXPECT_EQ(pending->Version, original->Version + 1u);
}

TEST_F(PerformanceGroupServiceTest, DigitsAndOtherFunctionKeysRemainAvailableOnBothTransitions)
{
    engine::PerformanceGroupController controller;
    auto publish = [](auto) { FAIL() << "Unexpected group publication"; };
    for (const unsigned int keyCode : { 48u, 49u, 50u, 51u, 52u, 53u, 54u, 55u, 56u,
        57u, 121u, 122u, 123u })
    {
        actions::KeyAction key;
        key.KeyChar = keyCode;
        key.KeyActionType = actions::KeyAction::KEY_DOWN;
        EXPECT_FALSE(controller.HandleKey(key, {}, engine::GroupTargetKind::Take, publish));
        key.KeyActionType = actions::KeyAction::KEY_UP;
        EXPECT_FALSE(controller.ConsumeRelease(key));
    }
    EXPECT_EQ(controller.Groups().Mute.VisibleSlots(), 0u);
}

TEST_F(PerformanceGroupServiceTest, AllMuteFlagsIncludeAudioLeavesAndRetainTemporarySuppressionAndRemoteState)
{
    auto local = Station("local");
    auto take = local->AddTake();
    take->AddLoop(0u, local->Id());
    take->CommitChanges();
    auto loop = take->GetLoops().front();
    engine::StationParams params;
    params.Id = "remote";
    params.Size = { 160u, 320u };
    auto remote = std::make_shared<RemoteGroupTestStation>(params,
        engine::Station::GetMixerParams(params.Size, audio::MergeMixBehaviourParams{}));
    auto remoteTake = remote->AddTake();
    remoteTake->SetGroupMutedAudio(false);
    take->AcquireTriggerSourceMuteAudio();
    auto muted = engine::PerformanceGroupService::PrepareAllMuted({ local, remote }, true, 10u);
    ASSERT_TRUE(muted);
    ASSERT_EQ(1u, muted->Takes.size());
    ASSERT_EQ(1u, muted->Loops.size());
    muted->ApplyAudio();
    EXPECT_TRUE(loop->IsMuted());
    auto audible = engine::PerformanceGroupService::PrepareAllMuted({ local, remote }, false, 11u);
    ASSERT_TRUE(audible);
    audible->ApplyAudio();
    EXPECT_FALSE(take->GetTweakState() & base::Tweakable::TWEAKSTATE_MUTED);
    EXPECT_TRUE(take->IsMuted());
    EXPECT_FALSE(loop->GetTweakState() & base::Tweakable::TWEAKSTATE_MUTED);
    EXPECT_TRUE(loop->IsMuted());
    EXPECT_FALSE(remoteTake->IsMuted());
    take->ReleaseTriggerSourceMuteAudio();
    EXPECT_FALSE(take->IsMuted());
    EXPECT_FALSE(loop->IsMuted());
}

TEST_F(PerformanceGroupServiceTest, GlobalMuteThenBackspaceBeforeReceiptPreservesUnselectedDesiredFlags)
{
    auto station = Station("local");
    auto selected = station->AddTake();
    auto sibling = station->AddTake();
    selected->AddLoop(0u, station->Id());
    sibling->AddLoop(1u, station->Id());
    selected->CommitChanges();
    sibling->CommitChanges();
    selected->SetGroupSelected(true);
    engine::PerformanceGroupController controller;
    std::shared_ptr<const engine::GroupMuteApplication> pending;
    auto publish = [&pending](auto application) { pending = std::move(application); };
    ASSERT_TRUE(controller.SetAllMuted(true, { station }, publish));
    ASSERT_EQ(1u, pending->Version);
    controller.Acknowledge(0u);
    actions::KeyAction key;
    key.KeyChar = 8u;
    key.KeyActionType = actions::KeyAction::KEY_DOWN;
    key.Modifiers = base::Action::MODIFIER_NONE;
    ASSERT_TRUE(controller.HandleKey(key, { station }, engine::GroupTargetKind::Take, publish));
    ASSERT_EQ(2u, pending->Version);
    pending->ApplyAudio();
    EXPECT_FALSE(selected->IsMuted());
    EXPECT_FALSE(selected->GetLoops().front()->IsMuted());
    EXPECT_TRUE(sibling->IsMuted());
    EXPECT_TRUE(sibling->GetLoops().front()->IsMuted());
    ASSERT_TRUE(controller.SetAllMuted(false, { station }, publish));
    EXPECT_EQ(3u, pending->Version);
    pending->ApplyAudio();
    EXPECT_FALSE(sibling->IsMuted());
    EXPECT_FALSE(sibling->GetLoops().front()->IsMuted());
}

TEST_F(PerformanceGroupServiceTest, EmptyStationIntentSurvivesNewLeavesAndRetainsNoOwnerCycle)
{
    auto station=Station("empty-muted"); EXPECT_FALSE(station->AllTakesMuted()); ASSERT_TRUE(station->Mute());
    EXPECT_TRUE(station->RetainedMuted()); EXPECT_TRUE(station->AllTakesMuted());
    auto take=station->AddTake(); take->AddLoop(0u,station->Id()); take->CommitChanges();
    auto loop=take->GetLoops().front(); EXPECT_TRUE(take->IsMuted()); EXPECT_TRUE(loop->IsMuted());
    EXPECT_FALSE(take->GetTweakState() & base::Tweakable::TWEAKSTATE_MUTED); EXPECT_FALSE(loop->GetTweakState() & base::Tweakable::TWEAKSTATE_MUTED);
    take->UnMute(); loop->UnMute(); EXPECT_TRUE(take->IsMuted()); EXPECT_TRUE(loop->IsMuted());
    std::weak_ptr<engine::Station> weak=station;
    take->DetachGuiOwnershipForRetirement(); // Actual removal detaches staged takes too.
    station->DetachGuiOwnershipForRetirement();
    station.reset(); EXPECT_TRUE(weak.expired());
    EXPECT_TRUE(take->IsMuted()); EXPECT_TRUE(loop->IsMuted()); // Surviving scalar condition is lifetime-safe.
}

TEST_F(PerformanceGroupServiceTest, ParentRecallDoesNotFlattenChildrenButSourceSuppressionStillBecomesPersistent)
{
    auto station=Station("parent-source"); auto source=station->AddTake(); auto audible=station->AddTake(); auto rawMuted=station->AddTake();
    for (auto take : {source,audible,rawMuted}) { take->AddLoop(0u,station->Id()); take->CommitChanges(); }
    station->SetRetainedMuted(true); source->AcquireTriggerSourceMuteAudio(); rawMuted->SetGroupMutedAudio(true);
    const auto snapshot=engine::PerformanceGroupService::CaptureMuteConfiguration({station});
    for (auto take : {source,audible,rawMuted})
    {
        engine::GroupTargetId id{engine::GroupTargetKind::Take,station->Id(),take->Id(),{}};
        EXPECT_FALSE(engine::PerformanceGroupPreview::AudibleTake(snapshot,id)); EXPECT_FALSE(engine::PerformanceGroupPreview::Contains(snapshot,id));
        EXPECT_FALSE(engine::PerformanceGroupPreview::ContainsLive(snapshot,id,{id}));
    }
    station->SetRetainedMuted(false); source->ReleaseTriggerSourceMuteAudio();
    auto application=engine::PerformanceGroupService::PrepareMuteApplication(snapshot,{station},1u); ASSERT_TRUE(application); application->ApplyAudio();
    EXPECT_TRUE(station->RetainedMuted()); EXPECT_TRUE(source->GetTweakState() & base::Tweakable::TWEAKSTATE_MUTED);
    EXPECT_FALSE(audible->GetTweakState() & base::Tweakable::TWEAKSTATE_MUTED); EXPECT_FALSE(audible->GetLoops().front()->GetTweakState() & base::Tweakable::TWEAKSTATE_MUTED);
    station->SetRetainedMuted(false); EXPECT_FALSE(audible->IsMuted()); EXPECT_FALSE(audible->GetLoops().front()->IsMuted()); EXPECT_TRUE(source->IsMuted()); EXPECT_TRUE(rawMuted->IsMuted());
}

TEST_F(PerformanceGroupServiceTest, EmptyGlobalUnmuteAndStoreUseDesiredStationAndChildStateBeforeReceipt)
{
    auto empty=Station("empty"); auto station=Station("populated"); auto take=station->AddTake(); auto other=station->AddTake();
    station->SetRetainedMuted(true); empty->SetRetainedMuted(true); take->SetGroupMutedAudio(true); other->SetGroupMutedAudio(true);
    engine::PerformanceGroupController controller; std::shared_ptr<const engine::GroupMuteApplication> pending;
    auto publish=[&](auto application){pending=std::move(application);};
    ASSERT_TRUE(controller.SetAllMuted(false,{empty,station},publish)); ASSERT_EQ(2u,pending->Stations.size());
    ASSERT_TRUE(controller.Execute(false,engine::GroupCommand::Store,1u,{empty,station},engine::GroupTargetKind::Take,publish));
    const auto saved=controller.Groups().Mute.Get(1u); ASSERT_NE(nullptr,saved);
    EXPECT_TRUE(engine::PerformanceGroupPreview::AudibleTake(*saved,{engine::GroupTargetKind::Take,station->Id(),take->Id(),{}}));
    take->Select(); actions::KeyAction key; key.KeyChar=8u; key.KeyActionType=actions::KeyAction::KEY_DOWN;
    ASSERT_TRUE(controller.HandleKey(key,{empty,station},engine::GroupTargetKind::Take,publish)); ASSERT_EQ(2u,pending->Stations.size());
    pending->ApplyAudio(); EXPECT_FALSE(empty->RetainedMuted()); EXPECT_FALSE(station->RetainedMuted()); EXPECT_TRUE(take->IsMuted()); EXPECT_FALSE(other->IsMuted());
    auto restore=engine::PerformanceGroupService::PrepareMuteApplication(*saved,{empty,station},pending->Version+1u); restore->ApplyAudio();
    EXPECT_FALSE(empty->AllTakesMuted()); EXPECT_FALSE(take->IsMuted()); EXPECT_FALSE(other->IsMuted());
}

TEST_F(PerformanceGroupServiceTest, AggregateMutedChildrenRemainDistinctFromRetainedStationIntent)
{
    auto station=Station("aggregate"); auto first=station->AddTake(); auto second=station->AddTake();
    first->Mute(); second->Mute(); EXPECT_TRUE(station->AllTakesMuted()); EXPECT_FALSE(station->RetainedMuted());
    const auto saved=engine::PerformanceGroupService::CaptureMuteConfiguration({station});
    station->Mute(); auto restore=engine::PerformanceGroupService::PrepareMuteApplication(saved,{station},1u); restore->ApplyAudio();
    EXPECT_FALSE(station->RetainedMuted()); EXPECT_TRUE(station->AllTakesMuted()); ASSERT_TRUE(station->UnMute()); EXPECT_FALSE(station->AllTakesMuted());
    station->Mute(); EXPECT_TRUE(first->GetTweakState() & base::Tweakable::TWEAKSTATE_MUTED); station->UnMute(); EXPECT_FALSE(second->IsMuted());
    station->Mute(); ASSERT_TRUE(station->DitchTake(first)); ASSERT_TRUE(station->DitchTake(second)); station->CommitChanges();
    EXPECT_TRUE(station->AllTakesMuted()); auto replacement=station->AddTake(); EXPECT_TRUE(replacement->IsMuted());
    station->Reset(); EXPECT_FALSE(station->RetainedMuted());
}

TEST_F(PerformanceGroupServiceTest, DesiredRawUnmuteDoesNotEraseConcurrentSourceSuppressionInStoredGroup)
{
    auto station=Station("desired-source"); auto take=station->AddTake(); take->AddLoop(0u,station->Id()); take->CommitChanges();
    station->SetRetainedMuted(true); take->SetGroupMutedAudio(true); take->AcquireTriggerSourceMuteAudio();
    engine::PerformanceGroupController controller; std::shared_ptr<const engine::GroupMuteApplication> pending;
    auto publish=[&](auto application){pending=std::move(application);};
    ASSERT_TRUE(controller.SetAllMuted(false,{station},publish));
    ASSERT_TRUE(controller.Execute(false,engine::GroupCommand::Store,1u,{station},engine::GroupTargetKind::Take,publish));
    const auto saved=controller.Groups().Mute.Get(1u); ASSERT_NE(nullptr,saved);
    const auto takeState=std::find_if(saved->Targets.begin(),saved->Targets.end(),[&](const auto& state){return state.Target.Kind==engine::GroupTargetKind::Take && state.Target.TakeId==take->Id();});
    ASSERT_NE(saved->Targets.end(),takeState); EXPECT_FALSE(takeState->Muted); EXPECT_TRUE(takeState->EffectiveMuted);
    auto restore=engine::PerformanceGroupService::PrepareMuteApplication(*saved,{station},pending->Version+1u); restore->ApplyAudio(); take->ReleaseTriggerSourceMuteAudio();
    EXPECT_FALSE(station->RetainedMuted()); EXPECT_TRUE(take->GetTweakState() & base::Tweakable::TWEAKSTATE_MUTED); EXPECT_TRUE(take->IsMuted());
}

TEST_F(PerformanceGroupServiceTest, GlobalMutePreservesExplicitAndUnappliedDesiredParentIntent)
{
    auto station=Station("global-parent"); auto take=station->AddTake(); take->SetGroupSelected(true); station->SetRetainedMuted(true);
    engine::PerformanceGroupController controller; std::shared_ptr<const engine::GroupMuteApplication> pending;
    auto publish=[&](auto application){pending=std::move(application);};
    ASSERT_TRUE(controller.Execute(false,engine::GroupCommand::Store,1u,{station},engine::GroupTargetKind::Take,publish));
    station->SetRetainedMuted(false);
    ASSERT_TRUE(controller.Execute(false,engine::GroupCommand::RecallOrStore,1u,{station},engine::GroupTargetKind::Take,publish));
    ASSERT_TRUE(controller.SetAllMuted(true,{station},publish)); ASSERT_EQ(1u,pending->Stations.size()); EXPECT_TRUE(pending->Stations[0].Muted);
    actions::KeyAction key; key.KeyChar=8u; key.KeyActionType=actions::KeyAction::KEY_DOWN;
    ASSERT_TRUE(controller.HandleKey(key,{station},engine::GroupTargetKind::Take,publish)); pending->ApplyAudio();
    EXPECT_TRUE(station->RetainedMuted()); EXPECT_FALSE(take->GetTweakState() & base::Tweakable::TWEAKSTATE_MUTED); EXPECT_TRUE(take->IsMuted());
    ASSERT_TRUE(controller.SetAllMuted(false,{station},publish)); pending->ApplyAudio(); EXPECT_FALSE(station->RetainedMuted()); EXPECT_FALSE(take->IsMuted());
}

TEST_F(PerformanceGroupServiceTest, MarqueeRestoresRawChildrenAndLatestDesiredBeforeAudioReceipt)
{
    auto station = Station("marquee"); auto take = station->AddTake();
    take->AddLoop(0u, station->Id()); take->AddLoop(1u, station->Id()); take->CommitChanges();
    auto a = take->GetLoops()[0], b = take->GetLoops()[1]; b->SetGroupMutedAudio(true);
    engine::PerformanceGroupController controller;
    std::shared_ptr<const engine::GroupMuteApplication> pending;
    const auto publish = [&](auto image) { pending = std::move(image); };
    const engine::GroupTargetId target{engine::GroupTargetKind::Take, station->Id(), take->Id(), {}};
    ASSERT_TRUE(controller.BeginMuteMarquee({station})); EXPECT_FALSE(pending);
    ASSERT_TRUE(controller.PreviewMuteMarquee(std::array{target}, {station}, publish));
    const auto preview = pending; ASSERT_TRUE(preview); preview->ApplyAudio();
    EXPECT_TRUE(a->IsMuted()); EXPECT_TRUE(b->IsMuted());
    controller.Acknowledge(preview->Version);
    ASSERT_TRUE(controller.PreviewMuteMarquee({}, {station}, publish));
    ASSERT_TRUE(pending); EXPECT_GT(pending->Version, preview->Version);
    pending->ApplyAudio(); EXPECT_FALSE(a->IsMuted()); EXPECT_TRUE(b->IsMuted());
    const auto restored = pending;
    ASSERT_TRUE(controller.PreviewMuteMarquee({}, {station}, publish)); EXPECT_EQ(pending, restored);
    ASSERT_TRUE(controller.PreviewMuteMarquee(std::array{target}, {station}, publish));
    ASSERT_TRUE(controller.SetTargetMuted(target, false, {station}, publish));
    EXPECT_FALSE(controller.MuteMarqueeActive());
    pending->ApplyAudio(); EXPECT_FALSE(take->IsMuted()); EXPECT_FALSE(a->IsMuted()); EXPECT_TRUE(b->IsMuted());
}

TEST_F(PerformanceGroupServiceTest, ManualAuditionIsImmediateButMarqueeRestoreNeverReleasesReplacement)
{
    auto station = Station("audition"); auto source = station->AddTake();
    source->AddLoop(0u, station->Id()); source->CommitChanges();
    auto replacement = station->AddTake(); replacement->AcquireReplacementSourceMute(source);
    ASSERT_TRUE(source->IsSourceMuted());
    engine::PerformanceGroupController controller;
    std::shared_ptr<const engine::GroupMuteApplication> pending;
    const auto publish = [&](auto image) { pending = std::move(image); };
    const engine::GroupTargetId target{engine::GroupTargetKind::Take, station->Id(), source->Id(), {}};
    ASSERT_TRUE(controller.BeginMuteMarquee({station}));
    ASSERT_TRUE(controller.PreviewMuteMarquee(std::array{target}, {station}, publish)); pending->ApplyAudio();
    ASSERT_TRUE(controller.CancelMuteMarquee({station}, publish)); pending->ApplyAudio();
    EXPECT_TRUE(source->IsSourceMuted());
    ASSERT_TRUE(controller.SetTargetMuted(target, false, {station}, publish));
    EXPECT_FALSE(source->IsSourceMuted()); // Admission auditions without waiting for callback.
    auto newerReplacement = station->AddTake(); newerReplacement->AcquireReplacementSourceMute(source);
    ASSERT_TRUE(source->IsSourceMuted()); pending->ApplyAudio();
    EXPECT_TRUE(source->IsSourceMuted()); // Older queued raw command cannot audition a newer replacement.
    source->AcquireTriggerSourceMuteAudio();
    ASSERT_TRUE(controller.SetTargetMuted(target, false, {station}, publish)); pending->ApplyAudio();
    EXPECT_TRUE(source->IsSourceMuted()); // Transient/punch claim remains authoritative.
    source->ReleaseTriggerSourceMuteAudio(); EXPECT_FALSE(source->IsSourceMuted());
}

TEST_F(PerformanceGroupServiceTest, TopologyRestoreUsesSurvivingIdentityAndDoesNotMuteNewOwners)
{
    auto station = Station("topology"); auto old = station->AddTake();
    engine::PerformanceGroupController controller;
    std::shared_ptr<const engine::GroupMuteApplication> pending;
    const auto publish = [&](auto image) { pending = std::move(image); };
    ASSERT_TRUE(controller.BeginMuteMarquee({station}));
    ASSERT_TRUE(controller.PreviewMuteMarquee(std::array{engine::GroupTargetId{engine::GroupTargetKind::Take,station->Id(),old->Id(),{}}}, {station}, publish));
    pending->ApplyAudio(); ASSERT_TRUE(old->IsMuted());
    controller.PrepareForTopology({station});
    auto added = station->AddTake();
    controller.RepublishForTopology({station}, publish); ASSERT_TRUE(pending); pending->ApplyAudio();
    EXPECT_FALSE(old->IsMuted()); EXPECT_FALSE(added->IsMuted()); EXPECT_FALSE(controller.MuteMarqueeActive());
}

TEST_F(PerformanceGroupServiceTest, RemoteManualIntentSurvivesLocalGlobalPublicationAndIsNotStored)
{
    engine::StationParams params; params.Id="remote-marquee"; params.Size={160u,320u};
    auto remote=std::make_shared<RemoteGroupTestStation>(params,engine::Station::GetMixerParams(params.Size,audio::MergeMixBehaviourParams{}));
    auto remoteTake=remote->AddTake(); auto local=Station("local-marquee"); auto localTake=local->AddTake();
    engine::PerformanceGroupController controller; std::shared_ptr<const engine::GroupMuteApplication> pending;
    const auto publish=[&](auto image){pending=std::move(image);};
    const engine::GroupTargetId target{engine::GroupTargetKind::Take,remote->Id(),remoteTake->Id(),{}};
    ASSERT_TRUE(controller.SetTargetMuted(target,true,{local,remote},publish));
    ASSERT_TRUE(controller.SetAllMuted(false,{local,remote},publish));
    pending->ApplyAudio(); EXPECT_TRUE(remoteTake->IsMuted()); EXPECT_FALSE(localTake->IsMuted());
    const auto saved=engine::PerformanceGroupService::CaptureMuteConfiguration({local,remote});
    EXPECT_TRUE(std::none_of(saved.Targets.begin(),saved.Targets.end(),[&](const auto& state){return state.Target.StationId==remote->Id();}));
}
