#include "../../../../console/Protocol.h"
#include <gtest/gtest.h>

TEST(ConsoleProtocol, RoundTripUtf8AndRequestId)
{
	const console::Message sent{ console::MessageType::CommandRequest, 42, "h\xC3\xA9llo" };
	const auto bytes = console::EncodeFrame(sent);
	ASSERT_TRUE(bytes.has_value());
	const auto received = console::DecodeFrame(*bytes);
	ASSERT_EQ(received.Error, console::FrameError::None);
	ASSERT_TRUE(received.Value.has_value());
	EXPECT_EQ(received.Value->Type, sent.Type);
	EXPECT_EQ(received.Value->RequestId, sent.RequestId);
	EXPECT_EQ(received.Value->Text, sent.Text);
}

TEST(ConsoleProtocol, RejectsTruncationAndLengthMismatches)
{
	auto bytes = *console::EncodeFrame({ console::MessageType::Hello, 0, "token" });
	bytes.pop_back();
	EXPECT_EQ(console::DecodeFrame(bytes).Error, console::FrameError::Incomplete);
	bytes.push_back(0);
	bytes.push_back(0);
	EXPECT_EQ(console::DecodeFrame(bytes).Error, console::FrameError::InvalidSize);
	bytes[0] = 0xff;
	bytes[1] = 0xff;
	bytes[2] = 0xff;
	bytes[3] = 0xff;
	EXPECT_EQ(console::DecodeFrame(bytes).Error, console::FrameError::TooLarge);
}

TEST(ConsoleProtocol, RejectsVersionTypeAndInvalidUtf8)
{
	auto bytes = *console::EncodeFrame({ console::MessageType::Event, 0, "ok" });
	bytes[4] = 2;
	EXPECT_EQ(console::DecodeFrame(bytes).Error, console::FrameError::InvalidVersion);
	bytes[4] = console::ProtocolVersion;
	bytes[5] = 0;
	EXPECT_EQ(console::DecodeFrame(bytes).Error, console::FrameError::InvalidType);
	bytes[5] = static_cast<std::uint8_t>(console::MessageType::Event);
	bytes[14] = 0xc0;
	EXPECT_EQ(console::DecodeFrame(bytes).Error, console::FrameError::InvalidUtf8);
}

TEST(ConsoleProtocol, EnforcesPerMessageSizeAndUtf8)
{
	EXPECT_FALSE(console::EncodeFrame({ console::MessageType::CommandRequest, 1,
		std::string(console::MaxInputBytes + 1, 'a') }).has_value());
	EXPECT_FALSE(console::EncodeFrame({ console::MessageType::Event, 0,
		std::string(console::MaxEventBytes + 1, 'a') }).has_value());
	EXPECT_FALSE(console::EncodeFrame({ console::MessageType::Event, 0, "\xED\xA0\x80" }).has_value());
	EXPECT_FALSE(console::EncodeFrame({ console::MessageType::Event, 0, "\xF4\x90\x80\x80" }).has_value());
}

TEST(ConsoleProtocol, RejectsControlPayloadAndRequestIdViolations)
{
	EXPECT_FALSE(console::EncodeFrame({ console::MessageType::Shutdown, 0, "bad" }));
	EXPECT_FALSE(console::EncodeFrame({ console::MessageType::Hello, 1, "token" }));
	EXPECT_FALSE(console::EncodeFrame({ console::MessageType::CommandRequest, 0, "/" }));
	EXPECT_FALSE(console::EncodeFrame({ console::MessageType::Event, 1, "line" }));
	auto hello = *console::EncodeFrame({ console::MessageType::Hello, 0, "token" });
	hello[6] = 1;
	EXPECT_EQ(console::DecodeFrame(hello).Error, console::FrameError::InvalidMessage);
}
