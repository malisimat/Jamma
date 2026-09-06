///////////////////////////////////////////////////////////
//
// Author 2024 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#include "Vst3MidiMapping.h"

namespace vst
{
	namespace Vst3MidiMapping
	{
		// Raw MIDI status-byte constants used only by TryClassify() below.
		// Internal linkage via `static` (never an anonymous namespace) per
		// repo convention.
		static constexpr std::uint8_t ControlChangeStatus = 0xB0;
		static constexpr std::uint8_t ChannelPressureStatus = 0xD0;
		static constexpr std::uint8_t PitchBendStatus = 0xE0;

		ParameterTable MakeEmptyTable() noexcept
		{
			ParameterTable table{};
			for (auto& channel : table)
				channel.fill(NoParamId);
			return table;
		}

		bool TryClassify(const midi::MidiEvent& event, ControllerValue& out) noexcept
		{
			switch (event.MessageType())
			{
			case ControlChangeStatus:
				out.ControllerNumber = event.data1;
				out.NormalizedValue = std::clamp(static_cast<float>(event.data2) / 127.0f, 0.0f, 1.0f);
				return true;

			case ChannelPressureStatus:
				out.ControllerNumber = AfterTouchControllerNumber;
				out.NormalizedValue = std::clamp(static_cast<float>(event.data1) / 127.0f, 0.0f, 1.0f);
				return true;

			case PitchBendStatus:
			{
				const std::uint32_t combined = static_cast<std::uint32_t>(event.data1)
					| (static_cast<std::uint32_t>(event.data2) << 7);
				out.ControllerNumber = PitchBendControllerNumber;
				out.NormalizedValue = std::clamp(static_cast<float>(combined) / 16383.0f, 0.0f, 1.0f);
				return true;
			}

			default:
				return false;
			}
		}

		bool TryLookup(const ParameterTable& table,
			std::uint8_t channel,
			std::uint8_t controllerNumber,
			std::uint32_t& outParamId) noexcept
		{
			if (channel >= ChannelCount || controllerNumber >= ControllerCount)
				return false;

			const auto paramId = table[channel][controllerNumber];
			if (paramId == NoParamId)
				return false;

			outParamId = paramId;
			return true;
		}
	}
}
