#include <cmath>

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

// ============================================================================
// Phase anchor and frac arithmetic
// ============================================================================

// After EndRecord the loop stores the phase anchor so the pump can compute
// a loop-relative frac from a global sample position with:
//   frac = (globalSample - LoopPhaseAnchor()) % LoopLengthSamps() / LoopLengthSamps()
TEST(MidiAutomationPhaseAnchor, PhaseAnchorIsStoredByEndRecord)
{
	MidiLoop loop;
	const std::uint32_t loopLen     = 48000u;  // 1 s at 48 kHz
	const std::uint32_t phaseAnchor = 96000u;  // loop position 0 at global sample 96000

	loop.EndRecord(loopLen, phaseAnchor);

	EXPECT_EQ(loopLen,     loop.LoopLengthSamps());
	EXPECT_EQ(phaseAnchor, loop.LoopPhaseAnchor());
}

// The loop-relative frac for a given global sample must land at the correct
// fractional position regardless of where in the global timeline the loop
// started. This mirrors the frac math used by the playback dispatch and the
// CC-record path.
TEST(MidiAutomationPhaseAnchor, FracReflectsPhaseAnchor)
{
	const std::uint32_t loopLen     = 48000u;  // 1 s
	const std::uint32_t phaseAnchor = 96000u;

	// Reference calculation matching the phase-anchored frac used across the
	// dispatch and CC-record paths.
	auto calcFrac = [&](std::uint32_t globalSample) -> float {
		return static_cast<float>(
			std::fmod(static_cast<double>(globalSample - phaseAnchor),
				static_cast<double>(loopLen))
			/ static_cast<double>(loopLen));
	};

	// Loop start (frac = 0.0), midpoint (0.5), and wrap-around.
	EXPECT_NEAR(0.0f,  calcFrac(96000u), 1.0e-6f);
	EXPECT_NEAR(0.5f,  calcFrac(120000u), 1.0e-6f);
	EXPECT_NEAR(0.0f,  calcFrac(144000u), 1.0e-6f);  // second pass
	EXPECT_NEAR(0.25f, calcFrac(108000u), 1.0e-6f);
}

// Writing the same frac twice must replace the existing point's value rather
// than accumulate a duplicate point.
TEST(MidiAutomationPhaseAnchor, RepeatWriteAtSameFracReplacesValue)
{
	MidiLoop loop;
	auto* plugin = FakePlugin(0x800u);

	const auto lane = loop.ResolveAutomationLaneFor(plugin, 0u);
	ASSERT_TRUE(lane.has_value());
	ASSERT_TRUE(loop.WireEditorAutomationLane(*lane, plugin, 0u));

	// Two writes at the same frac with different values.
	loop.SetAutomationValueAtFrac(*lane, 0.5, 0.3f);  // first write
	loop.SetAutomationValueAtFrac(*lane, 0.5, 0.7f);  // second write (same frac)

	std::array<std::pair<float, float>, midi::AutomationLane::MaxPoints> points{};
	const auto count = loop.SnapshotAutomationLanePoints(*lane, points.data(), points.size());

	// There must be exactly one point at frac 0.5, holding the latest value.
	ASSERT_EQ(1u, count);
	EXPECT_NEAR(0.5f, points[0].first,  1.0e-5f);
	EXPECT_NEAR(0.7f, points[0].second, 1.0e-5f);
}

// Distinct frac positions each get their own point; rewriting those same
// positions replaces values in place rather than appending duplicates.
TEST(MidiAutomationPhaseAnchor, DistinctFracsFillLaneThenReplaceInPlace)
{
	MidiLoop loop;
	auto* plugin = FakePlugin(0x810u);

	const auto lane = loop.ResolveAutomationLaneFor(plugin, 1u);
	ASSERT_TRUE(lane.has_value());
	ASSERT_TRUE(loop.WireEditorAutomationLane(*lane, plugin, 1u));

	// Write four distinct frac positions.
	loop.SetAutomationValueAtFrac(*lane, 0.00, 0.1f);
	loop.SetAutomationValueAtFrac(*lane, 0.25, 0.2f);
	loop.SetAutomationValueAtFrac(*lane, 0.50, 0.3f);
	loop.SetAutomationValueAtFrac(*lane, 0.75, 0.4f);

	std::array<std::pair<float, float>, midi::AutomationLane::MaxPoints> points{};
	const auto count = loop.SnapshotAutomationLanePoints(*lane, points.data(), points.size());
	ASSERT_EQ(4u, count);

	// Points are sorted by frac.
	for (std::size_t i = 1u; i < count; ++i)
		EXPECT_LT(points[i - 1u].first, points[i].first);

	// Rewrite each position with a new value.
	loop.SetAutomationValueAtFrac(*lane, 0.00, 0.9f);
	loop.SetAutomationValueAtFrac(*lane, 0.25, 0.8f);
	loop.SetAutomationValueAtFrac(*lane, 0.50, 0.7f);
	loop.SetAutomationValueAtFrac(*lane, 0.75, 0.6f);

	const auto count2 = loop.SnapshotAutomationLanePoints(*lane, points.data(), points.size());

	// Count must not grow — writes at existing fracs replace, not append.
	EXPECT_EQ(count, count2);
	EXPECT_NEAR(0.9f, points[0].second, 1.0e-5f);
	EXPECT_NEAR(0.8f, points[1].second, 1.0e-5f);
	EXPECT_NEAR(0.7f, points[2].second, 1.0e-5f);
	EXPECT_NEAR(0.6f, points[3].second, 1.0e-5f);
}

TEST(MidiAutomationPhaseAnchor, OverwriteWindowReplacesTouchedFutureRange)
{
	MidiLoop loop;
	auto* plugin = FakePlugin(0x820u);

	const auto lane = loop.ResolveAutomationLaneFor(plugin, 2u);
	ASSERT_TRUE(lane.has_value());
	ASSERT_TRUE(loop.WireEditorAutomationLane(*lane, plugin, 2u));

	loop.EndRecord(1000u, 0u);

	// Existing curve spanning the loop.
	loop.SetAutomationValueAtFrac(*lane, 0.10, 0.1f);
	loop.SetAutomationValueAtFrac(*lane, 0.40, 0.4f);
	loop.SetAutomationValueAtFrac(*lane, 0.80, 0.8f);

	// Replace everything from sample 200 through sample 600 with a held value.
	loop.OverwriteAutomationWindow(*lane, 200u, 400u, 0.7f);

	std::array<std::pair<float, float>, midi::AutomationLane::MaxPoints> points{};
	const auto count = loop.SnapshotAutomationLanePoints(*lane, points.data(), points.size());
	ASSERT_EQ(4u, count);
	EXPECT_NEAR(0.10f, points[0].first, 1.0e-6f);
	EXPECT_NEAR(0.1f, points[0].second, 1.0e-6f);
	EXPECT_NEAR(0.20f, points[1].first, 1.0e-6f);
	EXPECT_NEAR(0.7f, points[1].second, 1.0e-6f);
	EXPECT_NEAR(0.60f, points[2].first, 1.0e-6f);
	EXPECT_NEAR(0.7f, points[2].second, 1.0e-6f);
	EXPECT_NEAR(0.80f, points[3].first, 1.0e-6f);
	EXPECT_NEAR(0.8f, points[3].second, 1.0e-6f);
}

TEST(MidiAutomationPhaseAnchor, OverwriteWindowWrapsAcrossLoopBoundary)
{
	MidiLoop loop;
	auto* plugin = FakePlugin(0x821u);

	const auto lane = loop.ResolveAutomationLaneFor(plugin, 3u);
	ASSERT_TRUE(lane.has_value());
	ASSERT_TRUE(loop.WireEditorAutomationLane(*lane, plugin, 3u));

	loop.EndRecord(1000u, 0u);

	loop.SetAutomationValueAtFrac(*lane, 0.05, 0.05f);
	loop.SetAutomationValueAtFrac(*lane, 0.25, 0.25f);
	loop.SetAutomationValueAtFrac(*lane, 0.70, 0.70f);
	loop.SetAutomationValueAtFrac(*lane, 0.95, 0.95f);

	// Window [900, 1200) wraps and should replace [0.90, 1.0) plus [0.0, 0.20).
	loop.OverwriteAutomationWindow(*lane, 900u, 300u, 0.6f);

	std::array<std::pair<float, float>, midi::AutomationLane::MaxPoints> points{};
	const auto count = loop.SnapshotAutomationLanePoints(*lane, points.data(), points.size());
	ASSERT_EQ(4u, count);
	EXPECT_NEAR(0.20f, points[0].first, 1.0e-6f);
	EXPECT_NEAR(0.6f, points[0].second, 1.0e-6f);
	EXPECT_NEAR(0.25f, points[1].first, 1.0e-6f);
	EXPECT_NEAR(0.25f, points[1].second, 1.0e-6f);
	EXPECT_NEAR(0.70f, points[2].first, 1.0e-6f);
	EXPECT_NEAR(0.70f, points[2].second, 1.0e-6f);
	EXPECT_NEAR(0.90f, points[3].first, 1.0e-6f);
	EXPECT_NEAR(0.6f, points[3].second, 1.0e-6f);
}
