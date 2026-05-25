#include "gtest/gtest.h"
#include <cstdint>
#include "io/SerialTriggerProtocol.h"

using io::SerialTriggerEvent;
using io::SerialTriggerProtocol;

TEST(SerialTriggerProtocol, ParsesWholePacket) {
	SerialTriggerProtocol protocol;
	SerialTriggerEvent event{};

	EXPECT_FALSE(protocol.PushByte(SerialTriggerProtocol::PacketHeader, event));
	EXPECT_FALSE(protocol.PushByte(7u, event));
	EXPECT_TRUE(protocol.PushByte(1u, event));
	EXPECT_EQ(7u, event.ButtonIndex);
	EXPECT_TRUE(event.IsPressed);
}

TEST(SerialTriggerProtocol, IgnoresNoiseUntilHeader) {
	SerialTriggerProtocol protocol;
	SerialTriggerEvent event{};

	EXPECT_FALSE(protocol.PushByte(0x00u, event));
	EXPECT_FALSE(protocol.PushByte(0x14u, event));
	EXPECT_FALSE(protocol.PushByte(SerialTriggerProtocol::PacketHeader, event));
	EXPECT_FALSE(protocol.PushByte(2u, event));
	EXPECT_TRUE(protocol.PushByte(0u, event));
	EXPECT_EQ(2u, event.ButtonIndex);
	EXPECT_FALSE(event.IsPressed);
}

TEST(SerialTriggerProtocol, ResynchronisesOnHeaderMidPacket) {
	SerialTriggerProtocol protocol;
	SerialTriggerEvent event{};

	EXPECT_FALSE(protocol.PushByte(SerialTriggerProtocol::PacketHeader, event));
	EXPECT_FALSE(protocol.PushByte(9u, event));
	EXPECT_FALSE(protocol.PushByte(SerialTriggerProtocol::PacketHeader, event));
	EXPECT_FALSE(protocol.PushByte(3u, event));
	EXPECT_TRUE(protocol.PushByte(1u, event));
	EXPECT_EQ(3u, event.ButtonIndex);
	EXPECT_TRUE(event.IsPressed);
}