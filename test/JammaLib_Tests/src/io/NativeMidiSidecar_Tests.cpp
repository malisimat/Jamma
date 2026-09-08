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
	lane.TargetPluginIndex = 2u;
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
