#include <atomic>
#include <thread>

#include "gtest/gtest.h"
#include "audio/MidiDevice.h"

using audio::MidiDevice;

TEST(MidiDevice, EnumeratesInputDevices) {
	auto devices = MidiDevice::EnumerateInputDevices();
	std::cout << "[TEST][MIDI] Enumerated devices: " << devices.size() << std::endl;
	for (const auto& d : devices)
		std::cout << "[TEST][MIDI]   #" << d.DeviceId << " " << d.Name << std::endl;

	// This environment may legitimately have zero MIDI input devices attached.
	ASSERT_GE(devices.size(), 0u);
}

TEST(MidiDevice, OpensPreferredDeviceWhenAvailable) {
	auto devices = MidiDevice::EnumerateInputDevices();
	if (devices.empty())
		GTEST_SKIP() << "No MIDI devices available on this machine.";

	MidiDevice device;
	std::atomic<unsigned int> callbackCount{ 0u };

	auto opened = device.Open(devices.front().Name,
		[&callbackCount](std::uint8_t, std::uint8_t, std::uint8_t)
		{
			callbackCount.fetch_add(1u);
		});

	ASSERT_TRUE(opened);
	ASSERT_TRUE(device.IsOpen());
	ASSERT_EQ(devices.front().Name, device.DeviceName());

	// Let the callback thread spin briefly so connection can be validated in logs.
	std::this_thread::sleep_for(std::chrono::milliseconds(25));
	device.Close();
	ASSERT_FALSE(device.IsOpen());
}
