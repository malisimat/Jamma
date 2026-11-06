#include "gtest/gtest.h"

#include <sstream>

#include "io/NativeMidiSidecar.h"

TEST(NativeMidiSidecar, RoundTripsExactSampleOffsetsAndBytes)
{
	io::NativeMidiSidecar::Stream stream;
	stream.LogicalLength = 1000u;
	stream.AutomationGlobalSampleOrigin = 9876543210ull;
	stream.Events = {
		{ 42u, 0x91u, 60u, 127u },
		{ 42u, 0x81u, 60u, 0u },
		{ 999u, 0xb1u, 7u, 100u }
	};
	io::JamFile::AutomationLane lane;
	lane.Mapping = io::JamFile::AutomationLane::MappingType::Cc;
	lane.Channel = 1u;
	lane.Controller = 7u;
	lane.TargetScope = "station";
	lane.TargetPluginId = "stable-plugin-id";
	lane.TargetPluginIndex = 2u;
	lane.TargetLoopIndex = 3u;
	lane.TargetParameterIndex = 4u;
	lane.Points = { { 0.0, 0.25 }, { 0.5, 0.75 } };
	stream.Lanes.push_back(lane);

	std::stringstream bytes;
	ASSERT_TRUE(io::NativeMidiSidecar::ToStream(stream, bytes));
	auto parsed = io::NativeMidiSidecar::FromStream(bytes);
	ASSERT_TRUE(parsed.has_value());
	ASSERT_EQ(stream.Events.size(), parsed->Events.size());
	EXPECT_EQ(42u, parsed->Events[0].SampleOffset);
	EXPECT_EQ(0x91u, parsed->Events[0].Status);
	EXPECT_EQ(60u, parsed->Events[0].Data1);
	EXPECT_EQ(127u, parsed->Events[0].Data2);
	EXPECT_EQ(stream.AutomationGlobalSampleOrigin, parsed->AutomationGlobalSampleOrigin);
	ASSERT_EQ(1u, parsed->Lanes.size());
	EXPECT_EQ(2u, parsed->Lanes[0].Points.size());
	EXPECT_DOUBLE_EQ(0.75, parsed->Lanes[0].Points[1].Value);
	EXPECT_EQ(3u, parsed->Lanes[0].TargetLoopIndex);
	EXPECT_EQ("stable-plugin-id", parsed->Lanes[0].TargetPluginId);
}

TEST(NativeMidiSidecar, RetainsLegacyLaneIndicesAndBoundsStableIdentities)
{
	io::NativeMidiSidecar::Stream stream;
	stream.LogicalLength = 128u;
	io::JamFile::AutomationLane lane;
	lane.TargetPluginId = "unavailable-plugin";
	lane.TargetPluginIndex = 2u;
	lane.Points = {{0.25, 0.75}};
	stream.Lanes.push_back(lane);
	std::stringstream bytes;
	ASSERT_TRUE(io::NativeMidiSidecar::ToStream(stream, bytes));
	// No events: old v0.2/v0.3 lanes have the same 20-byte fixed fields,
	// immediately followed by points rather than v0.4's length-prefixed ID.
	for (const auto minor : {2, 3})
	{
		auto legacy = bytes.str();
		legacy[10] = static_cast<char>(minor);
		legacy.erase(54u, 4u + lane.TargetPluginId.size());
		std::stringstream oldBytes(legacy);
		const auto parsed = io::NativeMidiSidecar::FromStream(oldBytes);
		ASSERT_TRUE(parsed);
		ASSERT_EQ(1u, parsed->Lanes.size());
		EXPECT_TRUE(parsed->Lanes[0].TargetPluginId.empty());
		EXPECT_EQ(2u, parsed->Lanes[0].TargetPluginIndex);
		ASSERT_EQ(1u, parsed->Lanes[0].Points.size());
		EXPECT_DOUBLE_EQ(0.75, parsed->Lanes[0].Points[0].Value);
	}
	auto malformed = bytes.str();
	malformed.replace(54u, 4u, 4u, static_cast<char>(0xff));
	std::stringstream badBytes(malformed);
	EXPECT_FALSE(io::NativeMidiSidecar::FromStream(badBytes));
	stream.Lanes[0].TargetPluginId.assign(io::NativeMidiSidecar::MaxPluginIdentityBytes + 1u, 'x');
	std::stringstream oversized;
	EXPECT_FALSE(io::NativeMidiSidecar::ToStream(stream, oversized));
	for (const auto& invalid : {std::string("embedded\0nul", 12u), std::string("\xc0\xaf"), std::string("\xed\xa0\x80")})
	{
		stream.Lanes[0].TargetPluginId = invalid;
		std::stringstream rejected;
		EXPECT_FALSE(io::NativeMidiSidecar::ToStream(stream, rejected));
	}
}

TEST(NativeMidiSidecar, RejectsTrailingPayload)
{
	io::NativeMidiSidecar::Stream stream;
	stream.LogicalLength = 8u;
	stream.Events = { { 1u, 0x90u, 60u, 100u } };
	std::stringstream bytes;
	ASSERT_TRUE(io::NativeMidiSidecar::ToStream(stream, bytes));
	bytes << 'x';
	std::string error;
	EXPECT_FALSE(io::NativeMidiSidecar::FromStream(bytes, &error).has_value());
	EXPECT_FALSE(error.empty());
}

TEST(NativeMidiSidecar, RejectsTruncatedAndInvalidAssets)
{
	std::stringstream truncated("JAMMIDI1", std::ios::in | std::ios::out | std::ios::binary);
	std::string error;
	EXPECT_FALSE(io::NativeMidiSidecar::FromStream(truncated, &error).has_value());
	EXPECT_FALSE(error.empty());

	io::NativeMidiSidecar::Stream stream;
	stream.LogicalLength = 8u;
	stream.Events = { { 8u, 0x90u, 1u, 1u } };
	std::stringstream out;
	EXPECT_FALSE(io::NativeMidiSidecar::ToStream(stream, out, &error));
}

TEST(NativeMidiSidecar, ExactEditorFlagsRoundTripAndLegacyEventsDefaultToRecordedTiming)
{
	io::NativeMidiSidecar::Stream stream;
	stream.LogicalLength = 11u;
	stream.Events = {{1u, 0x90u, 60u, 96u, 1u}, {4u, 0x80u, 60u, 0u, 1u}};
	std::stringstream bytes;
	ASSERT_TRUE(io::NativeMidiSidecar::ToStream(stream, bytes));
	const auto parsed = io::NativeMidiSidecar::FromStream(bytes);
	ASSERT_TRUE(parsed);
	EXPECT_EQ(1u, parsed->Events[0].Flags); EXPECT_EQ(1u, parsed->Events[1].Flags);
	// Version 0.2 used seven bytes per event; construct that old asset exactly.
	auto legacy = bytes.str();
	legacy[10] = 2; // minor version (little endian)
	legacy.erase(49u, 1u); legacy.erase(41u, 1u); // metadata bytes after 34-byte header
	std::stringstream oldBytes(legacy);
	const auto old = io::NativeMidiSidecar::FromStream(oldBytes);
	ASSERT_TRUE(old);
	EXPECT_EQ(0u, old->Events[0].Flags); EXPECT_EQ(0u, old->Events[1].Flags);
	stream.Events[0].Flags = 4u;
	std::stringstream invalid;
	EXPECT_FALSE(io::NativeMidiSidecar::ToStream(stream, invalid));
}

TEST(NativeMidiSidecar, BoundaryHeldClosuresRoundTripWithoutGuessingLegacyReleases)
{
	io::NativeMidiSidecar::Stream source;
	source.LogicalLength=1000u;
	source.Events={{20u,0x90u,60u,95u,0u},{100u,0x80u,60u,0u,0u},
		{900u,0x90u,60u,88u,0u},{999u,0x80u,60u,0u,4u}};
	std::stringstream bytes; ASSERT_TRUE(io::NativeMidiSidecar::ToStream(source,bytes));
	const auto restored=io::NativeMidiSidecar::FromStream(bytes); ASSERT_TRUE(restored);
	EXPECT_EQ(4u,restored->Events.back().Flags);
	auto legacy=bytes.str(); legacy[10]=4;
	std::stringstream invalidLegacy(legacy); EXPECT_FALSE(io::NativeMidiSidecar::FromStream(invalidLegacy));
	source.Events.back().Flags=0u;
	std::stringstream old; ASSERT_TRUE(io::NativeMidiSidecar::ToStream(source,old));
	legacy=old.str(); legacy[10]=4;
	std::stringstream validLegacy(legacy); const auto loaded=io::NativeMidiSidecar::FromStream(validLegacy);
	ASSERT_TRUE(loaded); EXPECT_EQ(0u,loaded->Events.back().Flags);
}
