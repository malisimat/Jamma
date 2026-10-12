#include "gtest/gtest.h"
#include "engine/PerformanceGroups.h"

using engine::GroupCommand;
using engine::PerformanceGroupRow;

TEST(PerformanceGroups, SparseSlotsKeepTheirNumbersAndFillLowestHole)
{
    PerformanceGroupRow<int> row;
    EXPECT_EQ(row.VisibleSlots(), 0u);
    ASSERT_TRUE(row.Store(7u, 70));
    EXPECT_EQ(row.VisibleSlots(), 7u);
    EXPECT_EQ(row.LowestEmptySlot(), 1u);
    auto capture = [] { return std::optional<int>(10); };
    auto apply = [](int) { return true; };
    ASSERT_TRUE(row.Execute(GroupCommand::Add, 0u, capture, apply));
    EXPECT_EQ(*row.Get(1u), 10);
    EXPECT_EQ(*row.Get(7u), 70);
    EXPECT_EQ(row.ActiveSlot(), 1u);
    row.Clear(1u);
    EXPECT_EQ(row.VisibleSlots(), 7u);
    EXPECT_FALSE(row.ActiveSlot());
    row.Clear(7u);
    EXPECT_EQ(row.VisibleSlots(), 0u);
}

TEST(PerformanceGroups, FullRowRejectsAddAndTrailingClearOnlyShrinksPresentation)
{
    PerformanceGroupRow<int> row;
    for (std::size_t i = 1u; i <= 9u; ++i) ASSERT_TRUE(row.Store(i, static_cast<int>(i)));
    EXPECT_FALSE(row.LowestEmptySlot());
    EXPECT_FALSE(row.Execute(GroupCommand::Add, 0u,
        [] { return std::optional<int>(99); }, [](int) { return true; }));
    row.Clear(4u);
    EXPECT_EQ(row.VisibleSlots(), 9u);
    EXPECT_EQ(row.LowestEmptySlot(), 4u);
    EXPECT_EQ(*row.Get(5u), 5);
    row.Clear(9u);
    EXPECT_EQ(row.VisibleSlots(), 8u);
}

TEST(PerformanceGroups, FailedCaptureAndApplyPreserveSlotAndActiveIndication)
{
    PerformanceGroupRow<int> row;
    row.Store(1u, 10);
    row.Store(2u, 20);
    EXPECT_FALSE(row.Execute(GroupCommand::Store, 1u,
        [] { return std::optional<int>(); }, [](int) { return true; }));
    EXPECT_EQ(*row.Get(1u), 10);
    EXPECT_EQ(row.ActiveSlot(), 2u);
    EXPECT_FALSE(row.Execute(GroupCommand::RecallOrStore, 1u,
        [] { return std::optional<int>(99); }, [](int) { return false; }));
    EXPECT_EQ(*row.Get(1u), 10);
    EXPECT_EQ(row.ActiveSlot(), 2u);
    int recalls = 0;
    auto apply = [&recalls](int value) { ++recalls; return value == 10; };
    EXPECT_TRUE(row.Execute(GroupCommand::RecallOrStore, 1u, [] { return std::optional<int>(); }, apply));
    EXPECT_TRUE(row.Execute(GroupCommand::RecallOrStore, 1u, [] { return std::optional<int>(); }, apply));
    EXPECT_EQ(recalls, 2);
}

TEST(PerformanceGroups, RecallOrStoreCapturesEmptySlotWithoutApplying)
{
    PerformanceGroupRow<int> row;
    int applications = 0;
    EXPECT_TRUE(row.Execute(GroupCommand::RecallOrStore, 6u,
        [] { return std::optional<int>(60); }, [&applications](int) { ++applications; return true; }));
    EXPECT_EQ(*row.Get(6u), 60);
    EXPECT_EQ(row.ActiveSlot(), 6u);
    EXPECT_EQ(applications, 0);
}

TEST(PerformanceGroups, EmptySnapshotsAreOccupiedAndRowsHaveIndependentActiveState)
{
    engine::PerformanceGroups groups;
    groups.Mute.Store(3u, {});
    groups.Selection.Store(8u, {});
    EXPECT_NE(groups.Selection.Get(8u), nullptr);
    EXPECT_EQ(groups.Mute.ActiveSlot(), 3u);
    EXPECT_EQ(groups.Selection.ActiveSlot(), 8u);
    groups.Selection.Clear(8u);
    EXPECT_EQ(groups.Mute.ActiveSlot(), 3u);
    EXPECT_FALSE(groups.Selection.ActiveSlot());
    EXPECT_FALSE(groups.Mute.Store(0u, {}));
    EXPECT_FALSE(groups.Mute.Store(10u, {}));
}

TEST(PerformanceGroups, PreviewUsesPositiveFlagsAndAggregatesOnlyDescendants)
{
    using engine::GroupTargetKind;
    using engine::PerformanceGroupPreview;
    const engine::GroupTargetId station{ GroupTargetKind::Station, "s", {}, {} };
    const engine::GroupTargetId take{ GroupTargetKind::Take, "s", "t", {} };
    const engine::GroupTargetId audio{ GroupTargetKind::AudioLoop, "s", "t", "a" };
    const engine::GroupTargetId midi{ GroupTargetKind::MidiLoop, "s", "t", "a" };
    engine::SelectionGroupSnapshot selection;
    selection.Targets = { { station, false }, { take, false }, { audio, false }, { midi, true } };
    EXPECT_TRUE(PerformanceGroupPreview::Contains(selection, station));
    EXPECT_TRUE(PerformanceGroupPreview::Contains(selection, take));
    EXPECT_FALSE(PerformanceGroupPreview::Contains(selection, audio));
    EXPECT_TRUE(PerformanceGroupPreview::Contains(selection, midi));
    EXPECT_FALSE(PerformanceGroupPreview::Contains(selection, { GroupTargetKind::Take, "s", "other", {} }));
    selection.Targets[3].Selected = false;
    selection.Targets[0].Selected = true;
    EXPECT_FALSE(PerformanceGroupPreview::Contains(selection, audio)); // Parent flag cannot invent a leaf.
    EXPECT_FALSE(PerformanceGroupPreview::Contains(selection, { GroupTargetKind::Station, "other", {}, {} }));
    engine::MuteGroupSnapshot mute;
    mute.Targets = { { station, false, true }, { take, true, true }, { audio, false, true } };
    EXPECT_FALSE(PerformanceGroupPreview::Contains(mute, station));
    EXPECT_FALSE(PerformanceGroupPreview::Contains(mute, take));
    EXPECT_FALSE(PerformanceGroupPreview::Contains(mute, audio));
    mute.Targets[2].EffectiveMuted = false;
    EXPECT_TRUE(PerformanceGroupPreview::Contains(mute, station));
    EXPECT_TRUE(PerformanceGroupPreview::Contains(mute, take));
    EXPECT_TRUE(PerformanceGroupPreview::Contains(mute, audio));
    EXPECT_FALSE(PerformanceGroupPreview::AudibleTake(mute, take)); // MIDI follows the saved take, not an audio sibling.
    EXPECT_FALSE(PerformanceGroupPreview::Contains(mute, midi));
    EXPECT_TRUE(mute.Targets[1].Muted); // Preview queries do not restore/mutate saved states.
    EXPECT_FALSE(selection.Targets[3].Selected);
}

TEST(PerformanceGroups, PreviewAncestorsAggregateOnlySurvivingSavedMembers)
{
    using engine::GroupTargetKind;
    using engine::PerformanceGroupPreview;
    const engine::GroupTargetId station{ GroupTargetKind::Station, "s", {}, {} };
    const engine::GroupTargetId take{ GroupTargetKind::Take, "s", "t", {} };
    const engine::GroupTargetId deleted{ GroupTargetKind::AudioLoop, "s", "t", "deleted" };
    const engine::GroupTargetId replacement{ GroupTargetKind::AudioLoop, "s", "t", "new" };
    std::vector<engine::GroupTargetId> live{ station, take, replacement };
    engine::SelectionGroupSnapshot selection;
    selection.Targets = { { station, false }, { take, false }, { deleted, true } };
    EXPECT_FALSE(PerformanceGroupPreview::ContainsLive(selection, station, live));
    EXPECT_FALSE(PerformanceGroupPreview::ContainsLive(selection, take, live));
    EXPECT_FALSE(PerformanceGroupPreview::ContainsLive(selection, replacement, live));
    live.push_back(deleted);
    EXPECT_TRUE(PerformanceGroupPreview::ContainsLive(selection, station, live));
    EXPECT_TRUE(PerformanceGroupPreview::ContainsLive(selection, take, live));
    live.pop_back();
    selection.Targets[0].Selected = true;
    EXPECT_TRUE(PerformanceGroupPreview::ContainsLive(selection, station, live)); // Exact station flag survives.
    EXPECT_FALSE(PerformanceGroupPreview::ContainsLive(selection, take, live));
    engine::MuteGroupSnapshot mute;
    mute.Targets = { { station, false, true }, { take, true, true }, { deleted, false, false } };
    EXPECT_FALSE(PerformanceGroupPreview::ContainsLive(mute, station, live));
    EXPECT_FALSE(PerformanceGroupPreview::ContainsLive(mute, take, live));
    live.push_back(deleted);
    EXPECT_TRUE(PerformanceGroupPreview::ContainsLive(mute, station, live));
    EXPECT_TRUE(PerformanceGroupPreview::ContainsLive(mute, take, live));
    EXPECT_FALSE(PerformanceGroupPreview::AudibleTake(mute, take));
}
