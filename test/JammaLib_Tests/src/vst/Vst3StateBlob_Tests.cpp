///////////////////////////////////////////////////////////
//
// Author 2024 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

// TDD tests for vst::Vst3StateBlob — pure byte-level framing logic shared by
// Vst3Plugin::GetState()/SetState(). Deliberately independent of the VST3 SDK
// and JAMMA_VST3_ENABLED, so these tests always run.

#include "gtest/gtest.h"
#include "vst/Vst3StateBlob.h"

using vst::Vst3StateBlob::Frame;
using vst::Vst3StateBlob::TryParse;
using vst::Vst3StateBlob::ParsedState;
using vst::Vst3StateBlob::HeaderSize;
using vst::Vst3StateBlob::FormatVersion;
using vst::Vst3StateBlob::StateType;

static std::vector<std::uint8_t> MakeBytes(std::initializer_list<int> values)
{
	std::vector<std::uint8_t> bytes;
	bytes.reserve(values.size());
	for (auto v : values)
		bytes.push_back(static_cast<std::uint8_t>(v));
	return bytes;
}

// -----------------------------------------------------------------------
// Suite: Vst3StateBlobRoundTrip
// -----------------------------------------------------------------------

TEST(Vst3StateBlobRoundTrip, BothNonEmptyRoundTrips)
{
	const auto componentState = MakeBytes({ 1, 2, 3, 4, 5 });
	const auto controllerState = MakeBytes({ 9, 8, 7 });

	const auto blob = Frame(componentState, controllerState);
	ASSERT_FALSE(blob.empty());

	ParsedState out;
	ASSERT_TRUE(TryParse(blob, out));
	EXPECT_EQ(out.ComponentState, componentState);
	EXPECT_EQ(out.ControllerState, controllerState);
}

TEST(Vst3StateBlobRoundTrip, BothEmptyRoundTrips)
{
	const std::vector<std::uint8_t> empty;

	const auto blob = Frame(empty, empty);
	ASSERT_FALSE(blob.empty());
	EXPECT_EQ(blob.size(), HeaderSize);

	ParsedState out;
	ASSERT_TRUE(TryParse(blob, out));
	EXPECT_TRUE(out.ComponentState.empty());
	EXPECT_TRUE(out.ControllerState.empty());
}

TEST(Vst3StateBlobRoundTrip, EmptyControllerNonEmptyComponentRoundTrips)
{
	const auto componentState = MakeBytes({ 42, 43, 44 });
	const std::vector<std::uint8_t> controllerState;

	const auto blob = Frame(componentState, controllerState);
	ParsedState out;
	ASSERT_TRUE(TryParse(blob, out));
	EXPECT_EQ(out.ComponentState, componentState);
	EXPECT_TRUE(out.ControllerState.empty());
}

TEST(Vst3StateBlobRoundTrip, HeaderFieldsMatchDocumentedLayout)
{
	const auto componentState = MakeBytes({ 1, 2 });
	const auto controllerState = MakeBytes({ 3, 4, 5 });

	const auto blob = Frame(componentState, controllerState);
	ASSERT_GE(blob.size(), HeaderSize);

	EXPECT_EQ(blob[0], FormatVersion);
	EXPECT_EQ(blob[1], StateType);
	EXPECT_EQ(blob.size(), HeaderSize + componentState.size() + controllerState.size());
}

// -----------------------------------------------------------------------
// Suite: Vst3StateBlobMalformed
// -----------------------------------------------------------------------

TEST(Vst3StateBlobMalformed, EmptyBlobFailsToParse)
{
	ParsedState out;
	EXPECT_FALSE(TryParse({}, out));
}

TEST(Vst3StateBlobMalformed, ShorterThanHeaderFailsToParse)
{
	std::vector<std::uint8_t> blob(HeaderSize - 1, 0);
	blob[0] = FormatVersion;
	blob[1] = StateType;

	ParsedState out;
	EXPECT_FALSE(TryParse(blob, out));
}

TEST(Vst3StateBlobMalformed, UnknownVersionFailsToParse)
{
	auto blob = Frame({}, {});
	blob[0] = static_cast<std::uint8_t>(FormatVersion + 1);

	ParsedState out;
	EXPECT_FALSE(TryParse(blob, out));
}

TEST(Vst3StateBlobMalformed, UnknownStateTypeFailsToParse)
{
	auto blob = Frame({}, {});
	blob[1] = static_cast<std::uint8_t>(StateType + 1);

	ParsedState out;
	EXPECT_FALSE(TryParse(blob, out));
}

TEST(Vst3StateBlobMalformed, TruncatedPayloadFailsToParse)
{
	auto blob = Frame(MakeBytes({ 1, 2, 3, 4 }), MakeBytes({ 5, 6 }));
	blob.pop_back(); // drop the last byte of controllerState

	ParsedState out;
	EXPECT_FALSE(TryParse(blob, out));
}

TEST(Vst3StateBlobMalformed, TrailingExtraBytesFailToParse)
{
	auto blob = Frame(MakeBytes({ 1, 2 }), MakeBytes({ 3 }));
	blob.push_back(0xFF); // spurious trailing byte

	ParsedState out;
	EXPECT_FALSE(TryParse(blob, out));
}

TEST(Vst3StateBlobMalformed, InconsistentSubBlobSizesFailToParse)
{
	// Frame a valid blob, then corrupt the declared component-state size
	// field (offset 6, 4 bytes little-endian) so it no longer matches the
	// actual payload length.
	auto blob = Frame(MakeBytes({ 1, 2, 3 }), MakeBytes({ 4, 5 }));
	ASSERT_GE(blob.size(), HeaderSize);

	blob[6] = 0xFF;
	blob[7] = 0xFF;
	blob[8] = 0xFF;
	blob[9] = 0x7F;

	ParsedState out;
	EXPECT_FALSE(TryParse(blob, out));
}

TEST(Vst3StateBlobMalformed, OutputUntouchedOnFailure)
{
	ParsedState out;
	out.ComponentState = MakeBytes({ 1, 2, 3 });
	out.ControllerState = MakeBytes({ 4, 5, 6 });

	EXPECT_FALSE(TryParse({}, out));

	// TryParse must leave `out` alone on failure (documented contract).
	EXPECT_EQ(out.ComponentState, MakeBytes({ 1, 2, 3 }));
	EXPECT_EQ(out.ControllerState, MakeBytes({ 4, 5, 6 }));
}
