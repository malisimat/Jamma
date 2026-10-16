#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "MidiQueue.h"

namespace rt { namespace midi { class RtMidiIn; } }

namespace midi
{
	struct MidiInputDeviceInfo
	{
		unsigned int DeviceId = 0u;
		std::string Name;
		// RtMidi's WinMM display name, including its current enumeration suffix.
		std::string PortName;
	};
	struct MidiInputInventory
	{
		std::vector<MidiInputDeviceInfo> Devices;
		std::string Error;
	};
	class MidiInputEnumerator
	{
	public:
		MidiInputEnumerator();
		~MidiInputEnumerator();
		// Off-callback only. Serializes client creation and device queries.
		MidiInputInventory Inventory();

	private:
		std::mutex _mutex;
		std::unique_ptr<rt::midi::RtMidiIn> _client;
	};
	enum class MidiConnectionStatus { Disabled, Missing, Ambiguous, Failed, Connected, CapacityExceeded };
	struct MidiConnectionAttempt
	{
		std::string RequestedName;
		std::string RegistryId;
		std::string ConnectedName;
		unsigned int DeviceId = 0u;
		MidiConnectionStatus Status = MidiConnectionStatus::Missing;
		bool DuplicateName = false;
		std::string Error;
	};
	struct MidiConnectionResult
	{
		MidiInputInventory Inventory;
		std::vector<MidiConnectionAttempt> Attempts;
		std::vector<MidiInputDeviceInfo> Connected;
		bool Accepted = false;
		std::string Error; // Reconciliation/worker failure; inventory errors remain separate.
		// Registry IDs aligned with Connected in successful endpoint order.
		std::vector<std::string> ConnectedRegistryIds;
	};

	class MidiDevice
	{
	public:
		using MidiMessageCallback = std::function<void(std::uint8_t status,
			std::uint8_t data1,
			std::uint8_t data2,
			double deltaSeconds,
			std::int64_t callbackArrivalMicros)>;

		MidiDevice();
		~MidiDevice();

		MidiDevice(const MidiDevice&) = delete;
		MidiDevice& operator=(const MidiDevice&) = delete;

		static std::vector<MidiInputDeviceInfo> EnumerateInputDevices();
		static MidiInputInventory InventoryInputDevices();
		// Job-side comparison of successful actual connections, never inventory
		// membership. WinMM enumeration indices/suffixes are not stable identities.
		static std::vector<std::string> LostConnectedInputIds(
			const MidiConnectionResult& previous, const MidiConnectionResult& refreshed);
		static std::vector<std::string> LostConnectedInputNames(
			const MidiConnectionResult& previous, const MidiConnectionResult& refreshed);
		static std::string BaseInputName(const std::string& portName, unsigned int portId);
		static std::string ResolveSavedInputName(const std::string& savedName,
			const std::vector<MidiInputDeviceInfo>& devices);
		static const MidiInputDeviceInfo* FindExactInput(const std::vector<MidiInputDeviceInfo>& devices,
			const std::string& name) noexcept;
		static const MidiInputDeviceInfo* SelectUnclaimedExactInput(
			const std::vector<MidiInputDeviceInfo>& devices, const std::string& name,
			const std::vector<unsigned int>& claimedIds, bool& ambiguous) noexcept;
		static const MidiInputDeviceInfo* SelectStartupInput(
			const std::vector<MidiInputDeviceInfo>& devices, const std::string& name,
			const std::vector<unsigned int>& claimedIds, size_t requestIndex,
			bool generatedRig, bool& ambiguous) noexcept;
		bool OpenPort(const MidiInputDeviceInfo& port, MidiMessageCallback callback,
			std::string& error, bool loggingVerbose = false);

		// Opens an exact named input; empty and missing names never fall back.
		// loggingVerbose: capture bounded packet details for the MIDI pump to print.
		bool Open(const std::string& preferredDeviceName,
			MidiMessageCallback callback,
			bool loggingVerbose = false);
		void Close();
		// Lifecycle owner only: stop callback writes and drain admitted callbacks.
		// Retains the backend connection for resumable session writer exclusion.
		void SuspendCallbacks() noexcept;
		void ResumeCallbacks() noexcept;
		// Single consumer: call from the MIDI pump, never from the device callback.
		void DrainVerbosePackets(const std::string& configuredName);

		bool IsOpen() const noexcept { return _isOpen && !_closeFailed; }
		const std::string& DeviceName() const noexcept { return _deviceName; }
		unsigned int DeviceId() const noexcept { return _deviceId; }

	private:
		static void _LogMidiMessageDetail(std::ostream& out,
			const unsigned char* message, std::size_t size);
		static void _RtMidiCallback(double deltatime,
			std::vector<unsigned char>* message,
			void* userData);
		void _OnMidiData(const std::vector<unsigned char>& message,
			double deltaSeconds,
			std::int64_t callbackArrivalMicros) noexcept;

		std::unique_ptr<rt::midi::RtMidiIn> _midiIn;
		bool _isOpen;
		bool _closeFailed = false;
		std::string _deviceName;
		unsigned int _deviceId;
		bool _loggingVerbose;
		MidiMessageCallback _callback;
		// Lifecycle owner closes admission before backend stop. Callback readers
		// drain before callback state is cleared; destruction follows backend close.
		// One admission word orders closure against reader entry; separate counters
		// would allow reset to race a callback observing the old open flag.
		friend class MidiDeviceCallbackTestAccess;
		bool TryEnterCallback() noexcept;
		void LeaveCallback() noexcept;
		static constexpr std::uint32_t CallbackClosed = 2u;
		static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
		std::atomic<std::uint32_t> _callbackState{ CallbackClosed };
		struct VerbosePacket
		{
			std::array<unsigned char, 256> Bytes{};
			std::size_t Size = 0u;
			bool Truncated = false;
		};
		// The WinMM callback is the sole producer; PumpMidi is the sole consumer.
		MidiQueue<64, VerbosePacket> _verbosePackets;
		std::uint64_t _lastVerboseDroppedCount = 0u;
	};
}
