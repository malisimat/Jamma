#include <limits>

#include "gtest/gtest.h"

#include "midi/LoopGridGeometry.h"

using midi::LoopGridGeometry;
using midi::MidiQuantisationFraction;
using midi::MidiQuantisationSettings;

TEST(LoopGridGeometry, DisplayRotationAlignsOffGridWholeGrainLoopsWithoutChangingSourceGrid)
{
	MidiQuantisationSettings settings;
	settings.Enabled = true;
	settings.GrainSamps = 100u;
	settings.Fraction = MidiQuantisationFraction::Quarter;
	for (const auto start : { 13u, 63u, 213u, 263u })
	{
		const auto origin = LoopGridGeometry::DisplayOrigin(400u, settings, start);
		EXPECT_EQ(0u, (start + origin) % settings.GrainSamps);
		const auto grid = LoopGridGeometry::Resolve(400u, settings, start);
		ASSERT_TRUE(grid);
		std::vector<std::uint32_t> displayed;
		for (std::size_t i = 1u; i + 1u < grid->Boundaries.size(); ++i)
			displayed.push_back(LoopGridGeometry::DisplaySample(grid->Boundaries[i], 400u, origin));
		std::sort(displayed.begin(), displayed.end());
		ASSERT_EQ(16u, displayed.size());
		for (std::size_t i = 0u; i < displayed.size(); ++i)
			EXPECT_EQ(i * 25u, displayed[i]);
		for (std::uint32_t sample = 0; sample < 400u; ++sample)
			EXPECT_EQ(sample, (LoopGridGeometry::DisplaySample(sample, 400u, origin) + origin) % 400u);
		EXPECT_EQ(0u, grid->Boundaries.front());
		EXPECT_EQ(400u, grid->Boundaries.back());
	}
}

TEST(LoopGridGeometry, DisplayRotationRetainsExplicitOffsetsFreeTimingAndUnequalGeometry)
{
	MidiQuantisationSettings settings;
	settings.Enabled = true;
	settings.GrainSamps = 100u;
	EXPECT_EQ(387u, LoopGridGeometry::DisplayOrigin(400u, settings, 13u));
	EXPECT_EQ(0u, LoopGridGeometry::DisplayOrigin(0u, settings, 13u));
	EXPECT_EQ(0u, LoopGridGeometry::DisplayOrigin(401u, settings, 13u));
	settings.PhaseOffsetSamps = -1;
	EXPECT_EQ(0u, LoopGridGeometry::DisplayOrigin(400u, settings, 13u));
	settings.PhaseOffsetSamps = 1;
	EXPECT_EQ(0u, LoopGridGeometry::DisplayOrigin(400u, settings, 13u));
	settings.PhaseOffsetSamps = 0;
	settings.Enabled = false;
	EXPECT_EQ(0u, LoopGridGeometry::DisplayOrigin(400u, settings, 13u));
	settings.Enabled = true;
	settings.RemoteIntervalSamps = 400u;
	settings.RemoteBpi = 4u;
	EXPECT_EQ(0u, LoopGridGeometry::DisplayOrigin(400u, settings, 13u));
}

TEST(LoopGridGeometry, UsesClippedIntegerBoundariesAndLocalLoopLength)
{
	MidiQuantisationSettings settings;
	settings.Enabled = true;
	settings.GrainSamps = 10u;
	settings.Fraction = MidiQuantisationFraction::Quarter;
	settings.PhaseOffsetSamps = 1;
	const auto grid = LoopGridGeometry::Resolve(11u, settings, 3u);
	ASSERT_TRUE(grid);
	EXPECT_EQ((std::vector<std::uint32_t>{ 0u, 1u, 3u, 6u, 8u, 11u }), grid->Boundaries);
	EXPECT_EQ(0u, grid->CellAt(0u));
	EXPECT_EQ(4u, grid->CellAt(10u));
	EXPECT_EQ(4u, grid->CellAt(11u));
}

TEST(LoopGridGeometry, RemoteOriginOverridesLocalGrain)
{
	MidiQuantisationSettings settings;
	settings.Enabled = true;
	settings.GrainSamps = 2u;
	settings.Fraction = MidiQuantisationFraction::Whole;
	settings.RemoteIntervalSamps = 20u;
	settings.RemoteBpi = 2u;
	settings.RemoteOriginSamps = 7;
	const auto grid = LoopGridGeometry::Resolve(25u, settings, 12u);
	ASSERT_TRUE(grid);
	EXPECT_EQ((std::vector<std::uint32_t>{ 0u, 5u, 15u, 25u }), grid->Boundaries);
}

TEST(LoopGridGeometry, IncludesWrappedSnapsForNonDividingLength)
{
	MidiQuantisationSettings settings;
	settings.Enabled = true;
	settings.GrainSamps = 4u;
	settings.Fraction = MidiQuantisationFraction::Whole;
	const auto grid = LoopGridGeometry::Resolve(11u, settings, 0u);
	ASSERT_TRUE(grid);
	// Raw onset 10 snaps to absolute 12, then wraps to local sample 1.
	EXPECT_EQ((std::vector<std::uint32_t>{ 0u, 1u, 4u, 8u, 11u }), grid->Boundaries);
	settings.PhaseOffsetSamps = 2;
	const auto shifted = LoopGridGeometry::Resolve(11u, settings, 0u);
	ASSERT_TRUE(shifted);
	EXPECT_EQ((std::vector<std::uint32_t>{ 0u, 2u, 3u, 6u, 10u, 11u }), shifted->Boundaries);
}

TEST(LoopGridGeometry, UnresolvedAndZeroLengthBecomeFreeEditing)
{
	MidiQuantisationSettings settings;
	settings.Enabled = true;
	EXPECT_FALSE(LoopGridGeometry::Resolve(100u, settings, 0u));
	settings.GrainSamps = 10u;
	EXPECT_FALSE(LoopGridGeometry::Resolve(0u, settings, 0u));
	settings.Enabled = false;
	EXPECT_FALSE(LoopGridGeometry::Resolve(100u, settings, 0u));
	settings.Enabled = true;
	settings.Fraction = MidiQuantisationFraction::ThirtySecond;
	settings.GrainSamps = 1u;
	settings.RemoteOriginSamps = (std::numeric_limits<std::int64_t>::max)();
	const auto dense = LoopGridGeometry::Resolve(3u, settings, 0u);
	ASSERT_TRUE(dense);
	EXPECT_EQ((std::vector<std::uint32_t>{ 0u, 1u, 2u, 3u }), dense->Boundaries);
}

TEST(LoopGridGeometry, SampleAndPitchMappingRemainStableAfterResize)
{
	EXPECT_DOUBLE_EQ(0.0, LoopGridGeometry::SampleU(0u, 100u));
	EXPECT_DOUBLE_EQ(1.0, LoopGridGeometry::SampleU(100u, 100u));
	EXPECT_DOUBLE_EQ(0.0, LoopGridGeometry::SampleU(5u, 0u));
	EXPECT_EQ(99u, LoopGridGeometry::SampleAtU(1.0, 100u));
	EXPECT_EQ(0u, LoopGridGeometry::SampleAtU(-1.0, 100u));
	EXPECT_EQ(50u, LoopGridGeometry::SampleAtU(0.5, 100u));
	EXPECT_EQ(71u, LoopGridGeometry::PitchAtY(0.0, 240.0, 48, 24));
	EXPECT_EQ(71u, LoopGridGeometry::PitchAtY(0.0, 480.0, 48, 24));
	EXPECT_EQ(48u, LoopGridGeometry::PitchAtY(239.0, 240.0, 48, 24));
	EXPECT_FALSE(LoopGridGeometry::PitchAtY(240.0, 240.0, 48, 24));
	EXPECT_DOUBLE_EQ(115.0, LoopGridGeometry::PitchY(60u, 240.0, 48, 24));
}

TEST(LoopGridGeometry, CrossesSkippedCellsAndLoopSeamWithoutDuplicates)
{
	MidiQuantisationSettings settings;
	settings.Enabled = true;
	settings.GrainSamps = 10u;
	settings.Fraction = MidiQuantisationFraction::Whole;
	const auto grid = LoopGridGeometry::Resolve(100u, settings, 0u);
	ASSERT_TRUE(grid);
	const auto skip = grid->CrossedCells({ 2u, 60u }, { 5u, 60u });
	EXPECT_EQ((std::vector<LoopGridGeometry::Cell>{ { 2u, 60u }, { 3u, 60u },
		{ 4u, 60u }, { 5u, 60u } }), skip);
	const auto seam = grid->CrossedCells({ 9u, 60u }, { 1u, 62u }, 1);
	EXPECT_EQ((std::vector<LoopGridGeometry::Cell>{ { 9u, 60u }, { 0u, 61u },
		{ 1u, 62u } }), seam);
	EXPECT_EQ((std::vector<LoopGridGeometry::Cell>{ { 1u, 62u }, { 0u, 61u },
		{ 9u, 60u } }), grid->CrossedCells({ 1u, 62u }, { 9u, 60u }, -1));
	EXPECT_EQ(8u, grid->CrossedCells({ 2u, 60u }, { 9u, 60u }).size());
	EXPECT_TRUE(grid->CrossedCells({ 2u, 60u }, { 9u, 60u }, 1).empty());
}

TEST(LoopGridGeometry, HitZonesAndSpanValidation)
{
	using Zone = LoopGridGeometry::HitZone;
	EXPECT_EQ(Zone::LeftEdge, LoopGridGeometry::NoteHitZone(15.0, 10.0, 100.0));
	EXPECT_EQ(Zone::Body, LoopGridGeometry::NoteHitZone(50.0, 10.0, 100.0));
	EXPECT_EQ(Zone::RightEdge, LoopGridGeometry::NoteHitZone(95.0, 10.0, 100.0));
	EXPECT_EQ(Zone::LeftEdge, LoopGridGeometry::NoteHitZone(13.0, 10.0, 18.0));
	EXPECT_EQ(Zone::RightEdge, LoopGridGeometry::NoteHitZone(17.0, 10.0, 18.0));
	EXPECT_EQ(Zone::LeftEdge, LoopGridGeometry::NoteHitZone(12.0, 10.0, 15.0));
	EXPECT_EQ(Zone::RightEdge, LoopGridGeometry::NoteHitZone(12.5, 10.0, 15.0));
	EXPECT_TRUE(LoopGridGeometry::ValidNoteSpan(90u, 10u, 100u, 60, 0, 100));
	EXPECT_FALSE(LoopGridGeometry::ValidNoteSpan(90u, 11u, 100u, 60, 0, 100));
	EXPECT_FALSE(LoopGridGeometry::ValidNoteSpan(90u, 0u, 100u, 60, 0, 100));
	EXPECT_FALSE(LoopGridGeometry::ValidNoteSpan(0u, 1u, 100u, 128, 0, 100));
}

TEST(LoopGridGeometry, OnlySplitRepeatingCellsShareThePhysicalSeam)
{
	MidiQuantisationSettings settings;
	settings.Enabled = true; settings.GrainSamps = 10u; settings.Fraction = MidiQuantisationFraction::Whole;
	const auto aligned = LoopGridGeometry::Resolve(100u, settings, 0u);
	ASSERT_TRUE(aligned); EXPECT_EQ(0u, aligned->SeamHeadEnd);
	settings.PhaseOffsetSamps = 1;
	const auto split = LoopGridGeometry::Resolve(100u, settings, 0u);
	ASSERT_TRUE(split); EXPECT_EQ(1u, split->SeamHeadEnd); EXPECT_EQ(91u, split->SeamTailStart);
	EXPECT_EQ((std::pair{91u, 101u}), split->EditorSpan(0u, 1u));
	EXPECT_EQ((std::pair{91u, 101u}), split->EditorSpan(91u, 100u));
	EXPECT_EQ((std::pair{11u, 21u}), split->EditorSpan(11u, 21u));
	const auto unequal = LoopGridGeometry::Resolve(101u, settings, 0u);
	ASSERT_TRUE(unequal); EXPECT_EQ(0u, unequal->SeamHeadEnd);
}
