#include "gtest/gtest.h"

#include "midi/MidiLoop.h"

using midi::AutomationMapping;
using midi::MidiLoop;

namespace
{
	// Editor automation only ever compares plugin pointer identity, never
	// dereferences it. Use opaque non-null sentinels so the tests stay free of any
	// real plugin construction.
	vst::IVstPlugin* FakePlugin(std::uintptr_t id) noexcept
	{
		return reinterpret_cast<vst::IVstPlugin*>(id);
	}
}

TEST(MidiAutomationLaneResolution, ClaimsFirstInactiveLaneWhenUnmapped)
{
	MidiLoop loop;
	auto* plugin = FakePlugin(0x100u);

	const auto lane = loop.ResolveAutomationLaneFor(plugin, 3u);
	ASSERT_TRUE(lane.has_value());
	EXPECT_EQ(0u, *lane);

	// Pure query: nothing should have been activated by resolving.
	EXPECT_FALSE(loop.GetLane(0u).Mapping.IsActive());
}

TEST(MidiAutomationLaneResolution, ReusesActiveLaneForSamePluginAndParam)
{
	MidiLoop loop;
	auto* plugin = FakePlugin(0x200u);

	const auto first = loop.ResolveAutomationLaneFor(plugin, 7u);
	ASSERT_TRUE(first.has_value());
	ASSERT_TRUE(loop.WireEditorAutomationLane(*first, plugin, 7u));

	// Same (plugin, param) must resolve back to the very same lane.
	const auto again = loop.ResolveAutomationLaneFor(plugin, 7u);
	ASSERT_TRUE(again.has_value());
	EXPECT_EQ(*first, *again);
}

TEST(MidiAutomationLaneResolution, IgnoresActiveLaneWithDifferentMapping)
{
	MidiLoop loop;
	auto* pluginA = FakePlugin(0x300u);
	auto* pluginB = FakePlugin(0x301u);

	const auto laneA = loop.ResolveAutomationLaneFor(pluginA, 1u);
	ASSERT_TRUE(laneA.has_value());
	ASSERT_TRUE(loop.WireEditorAutomationLane(*laneA, pluginA, 1u));

	// A different plugin/param must not reuse lane A; it claims the next inactive.
	const auto laneB = loop.ResolveAutomationLaneFor(pluginB, 1u);
	ASSERT_TRUE(laneB.has_value());
	EXPECT_NE(*laneA, *laneB);
}

TEST(MidiAutomationLaneResolution, ReturnsNulloptWhenAllLanesOccupied)
{
	MidiLoop loop;

	// Fill every lane with a distinct active mapping.
	for (std::size_t i = 0u; i < MidiLoop::MaxAutomationLanes; ++i)
	{
		auto* plugin = FakePlugin(0x400u + i);
		const auto lane = loop.ResolveAutomationLaneFor(plugin, static_cast<unsigned int>(i));
		ASSERT_TRUE(lane.has_value());
		ASSERT_TRUE(loop.WireEditorAutomationLane(*lane, plugin, static_cast<unsigned int>(i)));
	}

	// A brand new mapping has nowhere to go.
	auto* overflow = FakePlugin(0x500u);
	const auto none = loop.ResolveAutomationLaneFor(overflow, 99u);
	EXPECT_FALSE(none.has_value());
}

TEST(MidiAutomationLaneResolution, WireUsesEditorSentinelMatchKey)
{
	MidiLoop loop;
	auto* plugin = FakePlugin(0x600u);

	const auto lane = loop.ResolveAutomationLaneFor(plugin, 5u);
	ASSERT_TRUE(lane.has_value());
	ASSERT_TRUE(loop.WireEditorAutomationLane(*lane, plugin, 5u));

	auto& mapping = loop.GetLane(*lane).Mapping;
	EXPECT_TRUE(mapping.IsActive());
	EXPECT_EQ(plugin, mapping.TargetPlugin);
	EXPECT_EQ(5u, mapping.TargetParameterIndex);
	EXPECT_EQ(AutomationMapping::MakeEditorMatchKey(),
		mapping.MatchKey.load(std::memory_order_relaxed));

	// Sentinel channel/CC are 0xFF, which no real incoming CC can match.
	EXPECT_EQ(0xFFu, mapping.GetChannel());
	EXPECT_EQ(0xFFu, mapping.GetCC());
}

TEST(MidiAutomationLaneResolution, RewiringSameMappingIsIdempotent)
{
	MidiLoop loop;
	auto* plugin = FakePlugin(0x700u);

	const auto lane = loop.ResolveAutomationLaneFor(plugin, 2u);
	ASSERT_TRUE(lane.has_value());

	// First wire reports a topology change; rewiring the identical mapping does not.
	EXPECT_TRUE(loop.WireEditorAutomationLane(*lane, plugin, 2u));
	EXPECT_FALSE(loop.WireEditorAutomationLane(*lane, plugin, 2u));
}

TEST(MidiAutomationLaneResolution, ClearPointsPreservesLaneMapping)
{
	MidiLoop loop;
	auto* plugin = FakePlugin(0x710u);

	const auto lane = loop.ResolveAutomationLaneFor(plugin, 9u);
	ASSERT_TRUE(lane.has_value());
	ASSERT_TRUE(loop.WireEditorAutomationLane(*lane, plugin, 9u));

	loop.SetAutomationValueAtFrac(*lane, 0.25, 0.2f);
	loop.SetAutomationValueAtFrac(*lane, 0.75, 0.8f);

	std::array<std::pair<float, float>, midi::AutomationLane::MaxPoints> points{};
	ASSERT_GT(loop.SnapshotAutomationLanePoints(*lane, points.data(), points.size()), 0u);

	loop.ClearAutomationLanePoints(*lane);

	EXPECT_TRUE(loop.GetLane(*lane).Mapping.IsActive());
	EXPECT_EQ(plugin, loop.GetLane(*lane).Mapping.TargetPlugin);
	EXPECT_EQ(9u, loop.GetLane(*lane).Mapping.TargetParameterIndex);
	EXPECT_EQ(0u, loop.SnapshotAutomationLanePoints(*lane, points.data(), points.size()));
}

TEST(MidiAutomationLaneResolution, ClearThenReplayFullyReplacesPriorCurve)
{
	MidiLoop loop;
	auto* plugin = FakePlugin(0x720u);

	const auto lane = loop.ResolveAutomationLaneFor(plugin, 10u);
	ASSERT_TRUE(lane.has_value());
	ASSERT_TRUE(loop.WireEditorAutomationLane(*lane, plugin, 10u));

	// Existing recorded curve.
	loop.SetAutomationValueAtFrac(*lane, 0.10, 0.1f);
	loop.SetAutomationValueAtFrac(*lane, 0.40, 0.4f);
	loop.SetAutomationValueAtFrac(*lane, 0.90, 0.9f);

	// Editor overwrite session rebuilds from only its newly captured points.
	loop.ClearAutomationLanePoints(*lane);
	loop.SetAutomationValueAtFrac(*lane, 0.20, 0.2f);
	loop.SetAutomationValueAtFrac(*lane, 0.30, 0.3f);

	std::array<std::pair<float, float>, midi::AutomationLane::MaxPoints> points{};
	const auto count = loop.SnapshotAutomationLanePoints(*lane, points.data(), points.size());
	ASSERT_EQ(2u, count);
	EXPECT_NEAR(0.20f, points[0].first, 1.0e-6f);
	EXPECT_NEAR(0.2f, points[0].second, 1.0e-6f);
	EXPECT_NEAR(0.30f, points[1].first, 1.0e-6f);
	EXPECT_NEAR(0.3f, points[1].second, 1.0e-6f);
}
